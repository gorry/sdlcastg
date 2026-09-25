// sdlcastg - Cast の受信側を探す（mDNS / DNS-SD の _googlecast._tcp）

#include "discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <set>

#include "net.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace sdlcastg {

namespace {

const char kService[] = "_googlecast._tcp.local";
const char kMdnsGroup[] = "224.0.0.251";
const int kMdnsPort = 5353;

// 問い合わせの間隔。最初は短く数回、あとは間を空ける。
const int64_t kQueryFirstMs[] = { 0, 1000, 3000 };
const int64_t kQueryIntervalMs = 15000;

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

std::string Lower(const std::string &s) {
	std::string out(s);
	for (size_t i = 0; i < out.size(); i++) {
		if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
	}
	return out;
}

// PTR の問い合わせ（QU ビット付き）。
std::string BuildQuery() {
	std::string q;
	const unsigned char header[12] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };  // qdcount=1
	q.append((const char *)header, sizeof(header));
	const char *p = kService;
	while (*p) {
		const char *dot = strchr(p, '.');
		const size_t len = dot ? (size_t)(dot - p) : strlen(p);
		q.push_back((char)len);
		q.append(p, len);
		p += len;
		if (*p == '.') p++;
	}
	q.push_back(0);
	q.push_back(0);
	q.push_back(12);           // PTR
	q.push_back((char)(0x80 - 256));  // QU（ユニキャストで返して）。0x80 を char へ
	q.push_back(1);            // IN
	return q;
}

// 圧縮（ポインタ）を辿りながら名前を読む。
bool ReadName(const uint8_t *p, size_t n, size_t *i, std::string *out) {
	size_t pos = *i;
	bool jumped = false;
	int hops = 0;
	out->clear();
	for (;;) {
		if (pos >= n) return false;
		const uint8_t len = p[pos];
		if ((len & 0xc0) == 0xc0) {
			if (pos + 1 >= n) return false;
			const size_t off = ((size_t)(len & 0x3f) << 8) | p[pos + 1];
			if (!jumped) *i = pos + 2;
			jumped = true;
			pos = off;
			if (++hops > 32) return false;
			continue;
		}
		if (len == 0) {
			if (!jumped) *i = pos + 1;
			return true;
		}
		pos++;
		if (pos + len > n) return false;
		if (!out->empty()) out->push_back('.');
		out->append((const char *)p + pos, len);
		pos += len;
	}
}

uint16_t Be16(const uint8_t *p) {
	return (uint16_t)((p[0] << 8) | p[1]);
}

struct Parsed {
	std::set<std::string> instances;                       // PTR の答え（小文字）
	std::map<std::string, std::pair<int, std::string> > srv;  // 名前 → (ポート, 相手の名前)
	std::map<std::string, std::map<std::string, std::string> > txt;
	std::map<std::string, std::string> a;                  // 名前 → IPv4
};

bool ParseResponse(const uint8_t *p, size_t n, Parsed *out) {
	if (n < 12) return false;
	const int qd = Be16(p + 4);
	const int rr = Be16(p + 6) + Be16(p + 8) + Be16(p + 10);
	size_t i = 12;
	std::string name;
	for (int k = 0; k < qd; k++) {
		if (!ReadName(p, n, &i, &name) || i + 4 > n) return false;
		i += 4;
	}
	for (int k = 0; k < rr; k++) {
		if (!ReadName(p, n, &i, &name) || i + 10 > n) return false;
		const uint16_t type = Be16(p + i);
		const uint16_t rdlen = Be16(p + i + 8);
		i += 10;
		if (i + rdlen > n) return false;
		const size_t rd = i;
		const std::string key = Lower(name);
		if (type == 12) {  // PTR
			size_t j = rd;
			std::string target;
			if (ReadName(p, n, &j, &target) && key == kService) out->instances.insert(Lower(target));
		} else if (type == 33 && rdlen >= 6) {  // SRV
			size_t j = rd + 6;
			std::string target;
			if (ReadName(p, n, &j, &target)) {
				out->srv[key] = std::make_pair((int)Be16(p + rd + 4), Lower(target));
			}
		} else if (type == 16) {  // TXT
			std::map<std::string, std::string> &kv = out->txt[key];
			size_t j = rd;
			while (j < rd + rdlen) {
				const size_t len = p[j++];
				if (j + len > rd + rdlen) break;
				const std::string item((const char *)p + j, len);
				j += len;
				const size_t eq = item.find('=');
				if (eq != std::string::npos) kv[item.substr(0, eq)] = item.substr(eq + 1);
			}
		} else if (type == 1 && rdlen == 4) {  // A
			char buf[16];
			snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[rd], p[rd + 1], p[rd + 2], p[rd + 3]);
			out->a[key] = buf;
		}
		i = rd + rdlen;
	}
	return true;
}

// 読んだ答えから、1 台ずつの情報を作る。アドレスが答えに無ければ、
// 返してきた相手のアドレスを使う。
void Collect(const Parsed &ps, const std::string &from, std::vector<CastDevice> *out) {
	std::set<std::string> names(ps.instances);
	for (std::map<std::string, std::map<std::string, std::string> >::const_iterator it =
	         ps.txt.begin();
	     it != ps.txt.end(); ++it) {
		names.insert(it->first);
	}
	const std::string suffix = std::string(".") + kService;
	for (std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it) {
		const std::string &inst = *it;
		if (inst.size() <= suffix.size() ||
		    inst.compare(inst.size() - suffix.size(), suffix.size(), suffix) != 0) {
			continue;
		}
		std::map<std::string, std::map<std::string, std::string> >::const_iterator t =
		    ps.txt.find(inst);
		if (t == ps.txt.end()) continue;
		CastDevice d;
		const std::map<std::string, std::string> &kv = t->second;
		std::map<std::string, std::string>::const_iterator v;
		if ((v = kv.find("id")) != kv.end()) d.id = v->second;
		if ((v = kv.find("fn")) != kv.end()) d.name = v->second;
		if ((v = kv.find("md")) != kv.end()) d.model = v->second;
		if ((v = kv.find("ca")) != kv.end()) d.capabilities = atoi(v->second.c_str());
		if (d.id.empty()) d.id = inst.substr(0, inst.size() - suffix.size());
		if (d.name.empty()) d.name = d.id;
		d.address = from;
		std::map<std::string, std::pair<int, std::string> >::const_iterator s = ps.srv.find(inst);
		if (s != ps.srv.end()) {
			d.port = s->second.first;
			std::map<std::string, std::string>::const_iterator a = ps.a.find(s->second.second);
			if (a != ps.a.end()) d.address = a->second;
		}
		out->push_back(d);
	}
}

void SetMulticastOptions(net::Socket s, const std::string &iface) {
	unsigned char ttl = 255;  // mDNS の決まり
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
	if (!iface.empty()) {
		in_addr a;
		if (inet_pton(AF_INET, iface.c_str(), &a) == 1) {
			setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&a, sizeof(a));
		}
	}
}

net::Socket OpenUdp(const std::string &bindAddr, int port, bool reuse) {
	net::Socket s = (net::Socket)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == net::kInvalidSocket) return s;
	if (reuse) {
		int one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#ifdef SO_REUSEPORT
		setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof(one));
#endif
	}
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (bindAddr.empty() || inet_pton(AF_INET, bindAddr.c_str(), &sa.sin_addr) != 1) {
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
	}
	if (bind(s, (sockaddr *)&sa, sizeof(sa)) != 0) {
		net::Close(s);
		return net::kInvalidSocket;
	}
	return s;
}

bool SendTo(net::Socket s, const std::string &data, const std::string &host, int port) {
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) return false;
	return sendto(s, data.data(), (int)data.size(), 0, (sockaddr *)&sa, sizeof(sa)) ==
	       (int)data.size();
}

// 届いていれば 1 つ読んで、見つかったものを out へ足す。
bool ReceiveOne(net::Socket s, std::vector<CastDevice> *out) {
	uint8_t buf[9000];
	sockaddr_in from;
	socklen_t len = sizeof(from);
	const int n = (int)recvfrom(s, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &len);
	if (n <= 0) return false;
	char ip[INET_ADDRSTRLEN] = { 0 };
	inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
	Parsed ps;
	if (ParseResponse(buf, (size_t)n, &ps)) Collect(ps, ip, out);
	return true;
}

}  // namespace

Discovery::Discovery() : running_(false), quit_(false) {}

Discovery::~Discovery() {
	Stop();
}

bool Discovery::Start(std::string *err) {
	Stop();
	if (!net::Init()) {
		if (err) *err = "network init failed";
		return false;
	}
	quit_ = false;
	running_ = true;
	thread_ = std::thread(&Discovery::Run, this);
	return true;
}

void Discovery::Stop() {
	if (thread_.joinable()) {
		quit_ = true;
		thread_.join();
		net::Quit();
	}
	running_ = false;
}

std::vector<CastDevice> Discovery::devices() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return devices_;
}

void Discovery::Run() {
	// 口ごとに問い合わせ用のソケットを作る（その口の住所に縛るので、
	// 答えもそのソケットへ返ってくる）。口が取れなければ 1 本で済ませる。
	std::vector<std::string> ifaces = net::LocalIPv4Addresses();
	if (ifaces.empty()) ifaces.push_back(std::string());
	std::vector<net::Socket> queriers;
	for (size_t i = 0; i < ifaces.size(); i++) {
		net::Socket s = OpenUdp(ifaces[i], 0, false);
		if (s == net::kInvalidSocket) continue;
		SetMulticastOptions(s, ifaces[i]);
		queriers.push_back(s);
	}

	// 5353 番でも待つ（取れなければ無しで進める）。
	net::Socket listener = OpenUdp(std::string(), kMdnsPort, true);
	if (listener != net::kInvalidSocket) {
		for (size_t i = 0; i < ifaces.size(); i++) {
			ip_mreq mr;
			memset(&mr, 0, sizeof(mr));
			inet_pton(AF_INET, kMdnsGroup, &mr.imr_multiaddr);
			if (ifaces[i].empty() || inet_pton(AF_INET, ifaces[i].c_str(), &mr.imr_interface) != 1) {
				mr.imr_interface.s_addr = htonl(INADDR_ANY);
			}
			setsockopt(listener, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mr, sizeof(mr));
		}
	}

	const std::string query = BuildQuery();
	const int64_t start = NowMs();
	size_t firstIndex = 0;
	int64_t nextQuery = start;

	while (!quit_) {
		const int64_t now = NowMs();
		if (now >= nextQuery) {
			for (size_t i = 0; i < queriers.size(); i++) {
				SendTo(queriers[i], query, kMdnsGroup, kMdnsPort);
			}
			firstIndex++;
			const size_t nFirst = sizeof(kQueryFirstMs) / sizeof(kQueryFirstMs[0]);
			nextQuery = (firstIndex < nFirst) ? start + kQueryFirstMs[firstIndex]
			                                  : now + kQueryIntervalMs;
		}

		fd_set rd;
		FD_ZERO(&rd);
		int maxfd = 0;
		for (size_t i = 0; i < queriers.size(); i++) {
			FD_SET(queriers[i], &rd);
			if ((int)queriers[i] > maxfd) maxfd = (int)queriers[i];
		}
		if (listener != net::kInvalidSocket) {
			FD_SET(listener, &rd);
			if ((int)listener > maxfd) maxfd = (int)listener;
		}
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000;
		if (select(maxfd + 1, &rd, 0, 0, &tv) <= 0) continue;

		std::vector<CastDevice> found;
		for (size_t i = 0; i < queriers.size(); i++) {
			if (FD_ISSET(queriers[i], &rd)) ReceiveOne(queriers[i], &found);
		}
		if (listener != net::kInvalidSocket && FD_ISSET(listener, &rd)) {
			ReceiveOne(listener, &found);
		}
		if (found.empty()) continue;

		std::lock_guard<std::mutex> lock(mutex_);
		for (size_t k = 0; k < found.size(); k++) {
			found[k].lastSeenMs = NowMs();
			bool merged = false;
			for (size_t j = 0; j < devices_.size(); j++) {
				if (devices_[j].id == found[k].id) {
					devices_[j] = found[k];
					merged = true;
					break;
				}
			}
			if (!merged) devices_.push_back(found[k]);
		}
	}

	for (size_t i = 0; i < queriers.size(); i++) net::Close(queriers[i]);
	net::Close(listener);
}

bool Discovery::Probe(const std::string &address, int timeoutMs, CastDevice *out) {
	if (!net::Init()) return false;
	bool ok = false;
	net::Socket s = OpenUdp(std::string(), 0, false);
	if (s != net::kInvalidSocket) {
		if (SendTo(s, BuildQuery(), address, kMdnsPort)) {
			const int64_t end = NowMs() + timeoutMs;
			while (!ok && NowMs() < end) {
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(s, &rd);
				timeval tv;
				tv.tv_sec = 0;
				tv.tv_usec = 100 * 1000;
				if (select((int)s + 1, &rd, 0, 0, &tv) <= 0) continue;
				std::vector<CastDevice> found;
				ReceiveOne(s, &found);
				if (!found.empty()) {
					*out = found[0];
					out->address = address;
					out->lastSeenMs = NowMs();
					ok = true;
				}
			}
		}
		net::Close(s);
	}
	net::Quit();
	return ok;
}

}  // namespace sdlcastg
