// sdlcastg - Cast V2 の通り道（TLS の上で CastMessage をやりとりする）

#include "channel.h"

#include <stdio.h>
#include <string.h>

#include <chrono>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

#ifndef _WIN32
#include <errno.h>
#endif

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)
// 乱数の種（mbedtls_user_config.h）。getrandom(2) は古いカーネルで長く止まるので、
// /dev/urandom を読む。mbedcrypto から呼ばれるので、必ず取り込まれるこのファイルに置く。
extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                                     size_t *olen) {
	(void)data;
	*olen = 0;
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f) return -1;
	const size_t n = fread(output, 1, len, f);
	fclose(f);
	if (n != len) return -1;
	*olen = n;
	return 0;
}
#endif

namespace sdlcastg {

const char kSenderId[] = "sender-0";
const char kReceiverId[] = "receiver-0";

namespace {

int64_t MsSince(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now() - t)
	    .count();
}

#ifndef _WIN32
// mbedTLS の送信（mbedtls_net_send の代わり）。SIGPIPE を出さない net::Send を使う。
int TlsSendNoSignal(void *ctx, const unsigned char *buf, size_t len) {
	const int fd = ((mbedtls_net_context *)ctx)->fd;
	if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
	const int n = net::Send((net::Socket)fd, buf, (int)len);
	if (n >= 0) return n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
	if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}
#endif

// 読み取りを待つ長さ (ms)。この間隔で送る待ち行列を見に行く。
const uint32_t kReadTimeoutMs = 50;
// こちらから PING を送る間隔と、何も届かなければ切れたとみなす長さ (ms)。
const int64_t kPingMs = 5000;
const int64_t kDeadMs = 15000;

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

std::string TlsError(const char *what, int code) {
	char buf[160];
	char detail[100];
	mbedtls_strerror(code, detail, sizeof(detail));
	snprintf(buf, sizeof(buf), "%s: -0x%04x %s", what, (unsigned)-code, detail);
	return buf;
}

}  // namespace

struct CastChannel::Tls {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;

	Tls() {
		mbedtls_net_init(&net);
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&conf);
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
	}
	~Tls() {
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		mbedtls_net_free(&net);  // ソケットも閉じる
	}
};

CastChannel::CastChannel() : tls_(0), running_(false), quit_(false), tickMs_(1000) {}

CastChannel::~CastChannel() {
	Close();
}

void CastChannel::SetHandlers(MessageHandler onMessage, ClosedHandler onClosed,
                              TickHandler onTick, int tickMs) {
	onMessage_ = onMessage;
	onClosed_ = onClosed;
	onTick_ = onTick;
	tickMs_ = (tickMs > 0) ? tickMs : 1000;
}

bool CastChannel::Open(const std::string &host, int port, int timeoutMs, std::string *err) {
	Close();
	const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

	// TLS 1.3 を持つ構成の mbedTLS 3.x は PSA の暗号を使うので、先に起こしておく
	// （何度呼んでもよい）。
	if (psa_crypto_init() != PSA_SUCCESS) {
		if (err) *err = "psa_crypto_init failed";
		return false;
	}

	const int64_t psaMs = MsSince(t0);

	std::string e;
	const net::Socket s = net::ConnectTcp(host, port, timeoutMs, &e);
	if (s == net::kInvalidSocket) {
		if (err) *err = e;
		return false;
	}
	localAddress_ = net::LocalAddress(s);

	const int64_t tcpMs = MsSince(t0);

	Tls *t = new Tls;
	t->net.fd = (int)s;

	static const char kPers[] = "sdlcastg";
	int r = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
	                              (const unsigned char *)kPers, sizeof(kPers) - 1);
	if (r == 0) {
		r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
		                                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	}
	if (r != 0) {
		if (err) *err = TlsError("TLS setup", r);
		delete t;
		return false;
	}
	const int64_t seedMs = MsSince(t0);
	// 受信側の証明書は自己署名。検証しない。
	mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
	mbedtls_ssl_conf_read_timeout(&t->conf, (uint32_t)timeoutMs);
	r = mbedtls_ssl_setup(&t->ssl, &t->conf);
	if (r != 0) {
		if (err) *err = TlsError("mbedtls_ssl_setup", r);
		delete t;
		return false;
	}
#ifdef _WIN32
	mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, 0, mbedtls_net_recv_timeout);
#else
	// mbedtls_net_send は write() を使うので、受信側が切ると SIGPIPE で落ちる。
	mbedtls_ssl_set_bio(&t->ssl, &t->net, TlsSendNoSignal, 0, mbedtls_net_recv_timeout);
#endif

	for (;;) {
		r = mbedtls_ssl_handshake(&t->ssl);
		if (r == 0) break;
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (err) *err = TlsError("TLS handshake", r);
		delete t;
		return false;
	}

	// 遅い端末（XS17 で 2 分超）の調べ用に、時間がかかったときだけ内訳を出す。
	const int64_t totalMs = MsSince(t0);
	if (totalMs >= 2000) {
		printf("sdlcastg : slow connect %lld ms (psa %lld, tcp %lld, seed %lld, handshake %lld)\n",
		       (long long)totalMs, (long long)psaMs, (long long)(tcpMs - psaMs),
		       (long long)(seedMs - tcpMs), (long long)(totalMs - seedMs));
	}

	// 握手が済んだら、読み取りは短い待ちで区切る（その間に書き出すため）。
	mbedtls_ssl_conf_read_timeout(&t->conf, kReadTimeoutMs);

	tls_ = t;
	quit_ = false;
	running_ = true;
	thread_ = std::thread(&CastChannel::Run, this);
	return true;
}

void CastChannel::Close() {
	if (thread_.joinable()) {
		quit_ = true;
		thread_.join();
	}
	delete tls_;
	tls_ = 0;
	running_ = false;
	std::lock_guard<std::mutex> lock(mutex_);
	outgoing_.clear();
}

void CastChannel::Send(const CastMessage &m) {
	const std::string wire = EncodeCastMessage(m);
	std::lock_guard<std::mutex> lock(mutex_);
	outgoing_.push_back(wire);
}

bool CastChannel::FlushOutgoing() {
	for (;;) {
		std::string wire;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (outgoing_.empty()) return true;
			wire.swap(outgoing_.front());
			outgoing_.pop_front();
		}
		size_t done = 0;
		while (done < wire.size()) {
			const int r = mbedtls_ssl_write(&tls_->ssl, (const unsigned char *)wire.data() + done,
			                                wire.size() - done);
			if (r > 0) {
				done += (size_t)r;
				continue;
			}
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			return false;
		}
	}
}

void CastChannel::HandleFrame(const std::string &body) {
	CastMessage m;
	if (!DecodeCastMessage((const uint8_t *)body.data(), body.size(), &m)) return;

	// 心拍はここで片付ける。受信側の PING には、その宛先の名前で返す。
	if (m.ns == kNsHeartbeat) {
		if (m.payload.find("\"PING\"") != std::string::npos) {
			Send(CastMessage(m.destination, m.source, kNsHeartbeat, "{\"type\":\"PONG\"}"));
		}
		return;
	}
	if (onMessage_) onMessage_(m);
}

void CastChannel::Run() {
	std::string inbuf;
	unsigned char buf[4096];
	int64_t lastRecv = NowMs();
	int64_t nextPing = lastRecv + kPingMs;
	int64_t nextTick = lastRecv + tickMs_;
	std::string reason;

	while (!quit_) {
		if (!FlushOutgoing()) {
			reason = "write failed";
			break;
		}

		const int r = mbedtls_ssl_read(&tls_->ssl, buf, sizeof(buf));
		const int64_t now = NowMs();
		if (r > 0) {
			lastRecv = now;
			inbuf.append((const char *)buf, (size_t)r);
			// 溜まったぶんから、そろったフレームを全部取り出す。
			for (;;) {
				if (inbuf.size() < 4) break;
				const uint32_t n = ((uint32_t)(unsigned char)inbuf[0] << 24) |
				                   ((uint32_t)(unsigned char)inbuf[1] << 16) |
				                   ((uint32_t)(unsigned char)inbuf[2] << 8) |
				                   (uint32_t)(unsigned char)inbuf[3];
				if (n > kMaxCastMessageSize) {
					reason = "message too large";
					quit_ = true;
					break;
				}
				if (inbuf.size() < 4 + (size_t)n) break;
				const std::string body = inbuf.substr(4, n);
				inbuf.erase(0, 4 + (size_t)n);
				HandleFrame(body);
			}
		} else if (r == MBEDTLS_ERR_SSL_TIMEOUT || r == MBEDTLS_ERR_SSL_WANT_READ ||
		           r == MBEDTLS_ERR_SSL_WANT_WRITE
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
		           || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
		) {
			// 何も届かなかった（ふつうのこと）。
		} else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
			reason = "closed by the receiver";
			break;
		} else {
			reason = TlsError("TLS read", r);
			break;
		}

		if (now >= nextPing) {
			Send(CastMessage(kSenderId, kReceiverId, kNsHeartbeat, "{\"type\":\"PING\"}"));
			nextPing = now + kPingMs;
		}
		if (now - lastRecv > kDeadMs) {
			reason = "heartbeat timed out";
			break;
		}
		if (onTick_ && now >= nextTick) {
			onTick_();
			nextTick = now + tickMs_;
		}
	}

	if (quit_ && reason.empty()) {
		// 自分から閉じる。積んであった別れの挨拶（CLOSE / STOP）を書き出してから。
		FlushOutgoing();
		mbedtls_ssl_close_notify(&tls_->ssl);
	}
	running_ = false;
	if (!reason.empty() && onClosed_) onClosed_(reason);
}

}  // namespace sdlcastg
