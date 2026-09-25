// sdlcastg - 受信側との 1 回のやりとり

#include "session.h"

#include <stdio.h>

#include <chrono>

#include "json.h"

namespace sdlcastg {

const char kDefaultMediaReceiver[] = "CC1AD845";

namespace {

// supportedMediaCommands のビット（Cast の MediaCommand）をログに出す。
void LogSupportedCommands(long long cmds) {
	static const struct {
		long long bit;
		const char *name;
	} kBits[] = {
		{ 1, "PAUSE" },
		{ 2, "SEEK" },
		{ 4, "STREAM_VOLUME" },
		{ 8, "STREAM_MUTE" },
		{ 16, "SKIP_FORWARD" },
		{ 32, "SKIP_BACKWARD" },
		{ 64, "QUEUE_NEXT" },
		{ 128, "QUEUE_PREV" },
		{ 256, "QUEUE_SHUFFLE" },
		{ 512, "SKIP_AD" },
		{ 1024, "QUEUE_REPEAT_ALL" },
		{ 2048, "QUEUE_REPEAT_ONE" },
		{ 4096, "EDIT_TRACKS" },
		{ 8192, "PLAYBACK_RATE" },
		{ 16384, "LIKE" },
		{ 32768, "DISLIKE" },
		{ 65536, "FOLLOW" },
		{ 131072, "UNFOLLOW" },
		{ 262144, "STREAM_TRANSFER" },
		{ 524288, "LYRICS" },
	};
	std::string names;
	long long known = 0;
	for (size_t i = 0; i < sizeof(kBits) / sizeof(kBits[0]); i++) {
		known |= kBits[i].bit;
		if (cmds & kBits[i].bit) {
			if (!names.empty()) names += ' ';
			names += kBits[i].name;
		}
	}
	if (cmds & ~known) {
		char rest[32];
		snprintf(rest, sizeof(rest), "%s0x%llx", names.empty() ? "" : " ", cmds & ~known);
		names += rest;
	}
	printf("sdlcastg : supportedMediaCommands %lld (%s)\n", cmds, names.c_str());
}

const int kConnectTimeoutMs = 5000;
// 状態の問い合わせの間隔（入出力スレッドの tick の倍数）。
const int kTickMs = 1000;
const int kMediaPollTicks = 2;

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

}  // namespace

const char *StateName(SessionStatus::State s) {
	switch (s) {
		case SessionStatus::kDisconnected: return "disconnected";
		case SessionStatus::kConnecting: return "connecting";
		case SessionStatus::kConnected: return "connected";
		case SessionStatus::kLaunching: return "launching";
		case SessionStatus::kLoading: return "loading";
		case SessionStatus::kBuffering: return "buffering";
		case SessionStatus::kPlaying: return "playing";
		case SessionStatus::kPaused: return "paused";
		case SessionStatus::kIdle: return "idle";
		case SessionStatus::kError: return "error";
	}
	return "?";
}

Session::Session()
    : loadPending_(false), appSeen_(false), requestId_(0), loadRequestId_(0),
      ourMediaSessionId_(0), awaitingLoadStatus_(false), loadSentAtMs_(0),
      supportedCommands_(-1), ticks_(0), currentTimeAtMs_(0) {}

Session::~Session() {
	Disconnect();
}

int Session::NextRequestId() {
	return ++requestId_;
}

bool Session::Connect(const std::string &host, int port, std::string *err) {
	Disconnect();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		status_ = SessionStatus();
		status_.state = SessionStatus::kConnecting;
		loadPending_ = false;
		appSeen_ = false;
		ticks_ = 0;
	}
	host_ = host;
	channel_.SetHandlers([this](const CastMessage &m) { OnMessage(m); },
	                     [this](const std::string &r) { OnClosed(r); }, [this]() { OnTick(); },
	                     kTickMs);
	std::string e;
	if (!channel_.Open(host, port, kConnectTimeoutMs, &e)) {
		std::lock_guard<std::mutex> lock(mutex_);
		status_.state = SessionStatus::kError;
		status_.error = e;
		if (err) *err = e;
		return false;
	}
	channel_.Send(CastMessage(kSenderId, kReceiverId, kNsConnection, "{\"type\":\"CONNECT\"}"));
	std::lock_guard<std::mutex> lock(mutex_);
	char buf[64];
	snprintf(buf, sizeof(buf), "{\"type\":\"GET_STATUS\",\"requestId\":%d}", NextRequestId());
	SendReceiver(buf);
	return true;
}

void Session::SendReceiver(const std::string &json) {
	channel_.Send(CastMessage(kSenderId, kReceiverId, kNsReceiver, json));
}

void Session::SendMedia(const std::string &json) {
	channel_.Send(CastMessage(kSenderId, status_.transportId, kNsMedia, json));
}

void Session::Load(const LoadRequest &req, const std::string &appId) {
	std::lock_guard<std::mutex> lock(mutex_);
	pendingLoad_ = req;
	loadPending_ = true;
	wantAppId_ = appId;
	if (!status_.transportId.empty() && status_.runningAppId == appId) {
		// 受信アプリはもう動いている（前の LOAD の続きなど）。すぐ送る。
		SendLoadLocked();
		return;
	}
	status_.state = SessionStatus::kLaunching;
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"type\":\"LAUNCH\",\"appId\":%s,\"requestId\":%d}",
	         Json::Quote(appId).c_str(), NextRequestId());
	SendReceiver(buf);
}

void Session::SendLoadLocked() {
	loadPending_ = false;
	status_.state = SessionStatus::kLoading;
	ourMediaSessionId_ = 0;
	awaitingLoadStatus_ = true;
	loadSentAtMs_ = NowMs();
	status_.mediaSessionId = 0;
	status_.playerState.clear();
	status_.idleReason.clear();
	status_.currentTime = 0.0;

	std::string media = "{\"contentId\":" + Json::Quote(pendingLoad_.url) +
	                    ",\"contentType\":" + Json::Quote(pendingLoad_.contentType) +
	                    ",\"streamType\":" + (pendingLoad_.live ? "\"LIVE\"" : "\"BUFFERED\"");
	if (!pendingLoad_.title.empty()) {
		media += ",\"metadata\":{\"metadataType\":0,\"title\":" + Json::Quote(pendingLoad_.title) +
		         "}";
	}
	media += "}";
	char head[64];
	loadRequestId_ = NextRequestId();
	snprintf(head, sizeof(head), "{\"type\":\"LOAD\",\"requestId\":%d,", loadRequestId_);
	SendMedia(std::string(head) + "\"autoplay\":true,\"media\":" + media + "}");
}

void Session::Stop() {
	std::lock_guard<std::mutex> lock(mutex_);
	loadPending_ = false;
	if (status_.sessionId.empty()) return;
	char head[64];
	snprintf(head, sizeof(head), "{\"type\":\"STOP\",\"requestId\":%d,", NextRequestId());
	SendReceiver(std::string(head) + "\"sessionId\":" + Json::Quote(status_.sessionId) + "}");
}

void Session::SetVolume(float level) {
	if (level < 0.0f) level = 0.0f;
	if (level > 1.0f) level = 1.0f;
	std::lock_guard<std::mutex> lock(mutex_);
	char buf[128];
	snprintf(buf, sizeof(buf),
	         "{\"type\":\"SET_VOLUME\",\"volume\":{\"level\":%.3f},\"requestId\":%d}", level,
	         NextRequestId());
	SendReceiver(buf);
}

void Session::Seek(double seconds) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (status_.transportId.empty() || status_.mediaSessionId == 0) return;
	if (seconds < 0.0) seconds = 0.0;
	char buf[192];
	snprintf(buf, sizeof(buf),
	         "{\"type\":\"SEEK\",\"mediaSessionId\":%d,\"currentTime\":%.3f,"
	         "\"resumeState\":\"PLAYBACK_START\",\"requestId\":%d}",
	         status_.mediaSessionId, seconds, NextRequestId());
	SendMedia(buf);
}

void Session::Disconnect() {
	if (channel_.open()) {
		std::string transport;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			transport = status_.transportId;
		}
		if (!transport.empty()) {
			channel_.Send(CastMessage(kSenderId, transport, kNsConnection, "{\"type\":\"CLOSE\"}"));
		}
		channel_.Send(CastMessage(kSenderId, kReceiverId, kNsConnection, "{\"type\":\"CLOSE\"}"));
	}
	channel_.Close();
	std::lock_guard<std::mutex> lock(mutex_);
	if (status_.state != SessionStatus::kError) status_.state = SessionStatus::kDisconnected;
}

SessionStatus Session::status() const {
	std::lock_guard<std::mutex> lock(mutex_);
	SessionStatus s = status_;
	// 再生中なら、知らされてからの経過を足す。足さないと問い合わせの間隔
	// （2 秒）ぶん古い値になり、遅れを測ると 0〜2 秒のずれがのこぎり形に
	// 乗る（2026-09-25、30 分の試験で見えた）。
	if (s.state == SessionStatus::kPlaying && currentTimeAtMs_ != 0) {
		s.currentTime += (NowMs() - currentTimeAtMs_) / 1000.0;
	}
	return s;
}

void Session::OnClosed(const std::string &reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	status_.state = SessionStatus::kDisconnected;
	status_.error = reason;
}

void Session::OnTick() {
	std::lock_guard<std::mutex> lock(mutex_);
	ticks_++;
	// 再生中は位置（currentTime）を読むために、ときどき尋ねる。
	if (status_.mediaSessionId != 0 && !status_.transportId.empty() &&
	    (ticks_ % kMediaPollTicks) == 0) {
		char buf[96];
		snprintf(buf, sizeof(buf), "{\"type\":\"GET_STATUS\",\"mediaSessionId\":%d,\"requestId\":%d}",
		         status_.mediaSessionId, NextRequestId());
		SendMedia(buf);
	}
}

void Session::OnMessage(const CastMessage &m) {
	Json p;
	if (!Json::Parse(m.payload, &p)) return;
	const std::string type = p["type"].str();

	std::lock_guard<std::mutex> lock(mutex_);

	if (m.ns == kNsConnection && type == "CLOSE") {
		// 受信アプリの側から切られた（TV のリモコンで止めた・別の送り手が
		// 乗っ取った）。
		if (!status_.transportId.empty() && m.source == status_.transportId) {
			status_.transportId.clear();
			status_.sessionId.clear();
			status_.mediaSessionId = 0;
			status_.state = SessionStatus::kIdle;
			status_.idleReason = "CLOSED";
		}
		return;
	}

	if (m.ns == kNsReceiver) {
		if (type == "RECEIVER_STATUS") {
			const Json &st = p["status"];
			const Json &vol = st["volume"];
			status_.volume = (float)vol["level"].num(status_.volume);
			status_.muted = vol["muted"].boolean(status_.muted);

			const Json &apps = st["applications"];
			status_.runningAppId.clear();
			status_.runningAppName.clear();
			const Json *mine = 0;
			for (size_t i = 0; i < apps.size(); i++) {
				const Json &a = apps[i];
				if (i == 0) {
					status_.runningAppId = a["appId"].str();
					status_.runningAppName = a["displayName"].str();
				}
				if (!wantAppId_.empty() && a["appId"].str() == wantAppId_) mine = &a;
			}

			if (mine != 0) {
				const std::string transport = (*mine)["transportId"].str();
				status_.sessionId = (*mine)["sessionId"].str();
				status_.runningAppId = (*mine)["appId"].str();
				status_.runningAppName = (*mine)["displayName"].str();
				if (transport != status_.transportId) {
					status_.transportId = transport;
					channel_.Send(
					    CastMessage(kSenderId, transport, kNsConnection, "{\"type\":\"CONNECT\"}"));
				}
				appSeen_ = true;
				if (loadPending_) SendLoadLocked();
			} else if (appSeen_ && !status_.transportId.empty()) {
				// 自分の受信アプリが消えた（止めた・TV で別のものに替えた）。
				status_.transportId.clear();
				status_.sessionId.clear();
				status_.mediaSessionId = 0;
				status_.state = SessionStatus::kIdle;
				if (status_.idleReason.empty()) status_.idleReason = "STOPPED";
			} else if (status_.state == SessionStatus::kConnecting) {
				status_.state = SessionStatus::kConnected;
			}
			return;
		}
		if (type == "LAUNCH_ERROR" || type == "INVALID_REQUEST") {
			status_.state = SessionStatus::kError;
			status_.error = type + ": " + p["reason"].str();
			loadPending_ = false;
		}
		return;
	}

	if (m.ns == kNsMedia) {
		if (type == "MEDIA_STATUS") {
			const Json &list = p["status"];
			if (list.size() == 0) return;
			const Json &s = list[(size_t)0];
			const int id = (int)s["mediaSessionId"].num(0);
			// こちらの再生の知らせだけを見る（ourMediaSessionId_ の説明）。
			const int rid = p["requestId"].isNumber() ? (int)p["requestId"].num() : 0;
			if (awaitingLoadStatus_) {
				if (rid == loadRequestId_ && id != 0) {
					ourMediaSessionId_ = id;
					awaitingLoadStatus_ = false;
				} else if (NowMs() - loadSentAtMs_ < 10000) {
					return;  // 前の再生の知らせ
				} else {
					awaitingLoadStatus_ = false;  // 返事が来ない。以後は来たものを見る
				}
			} else if (ourMediaSessionId_ != 0 && id != 0 && id < ourMediaSessionId_) {
				return;  // 前の再生の、遅れて来た知らせ
			}
			if (id != 0) status_.mediaSessionId = id;
			if (s.has("supportedMediaCommands")) {
				const long long cmds = (long long)s["supportedMediaCommands"].num();
				if (cmds != supportedCommands_) {
					supportedCommands_ = cmds;
					LogSupportedCommands(cmds);
				}
			}
			{
				// 受信側の再生の状態が変わったらログに出す（TV のリモコンのキーは届かないので、
				// 状態の移り変わりから推し量る手掛かり）。
				const std::string prev = status_.playerState;
				const std::string next = s["playerState"].str(status_.playerState);
				const std::string reason = s["idleReason"].str();
				if (next != prev || (next == "IDLE" && !reason.empty())) {
					printf("sdlcastg : media %s -> %s%s%s (session %d)\n",
					       prev.empty() ? "-" : prev.c_str(), next.c_str(),
					       reason.empty() ? "" : " ", reason.c_str(), id);
				}
				if (next != prev) status_.prevPlayerState = prev;
				status_.playerState = next;
			}
			if (s.has("currentTime")) {
				status_.currentTime = s["currentTime"].num();
				currentTimeAtMs_ = NowMs();
			}
			status_.idleReason = s["idleReason"].str();
			if (status_.playerState == "PLAYING") {
				status_.state = SessionStatus::kPlaying;
			} else if (status_.playerState == "BUFFERING") {
				status_.state = SessionStatus::kBuffering;
			} else if (status_.playerState == "PAUSED") {
				status_.state = SessionStatus::kPaused;
			} else if (status_.playerState == "IDLE" && !status_.idleReason.empty()) {
				status_.state = SessionStatus::kIdle;
			}
			return;
		}
		if (type == "LOAD_FAILED" || type == "LOAD_CANCELLED" || type == "INVALID_REQUEST" ||
		    type == "ERROR") {
			// 調べる手掛かりに、受け取ったものをそのまま出す（まれにしか来ない）。
			printf("sdlcastg : media %s\n", m.payload.c_str());
			// こちらが送った LOAD 以外への返事は、誤りとして扱わない。受信側で一時停止中の
			// ライブを TV のリモコンで再開すると、受信側が自分で読み込み直し、それを
			// こちらの LOAD（SDLCastG_ReloadStream）が遮って LOAD_FAILED が来ることがある。
			const int rid = p["requestId"].isNumber() ? (int)p["requestId"].num() : -1;
			if (rid >= 0 && rid != loadRequestId_) return;
			status_.state = SessionStatus::kError;
			status_.error = type;
			const std::string detail = p["detailedErrorCode"].isNumber()
			                               ? std::to_string((int)p["detailedErrorCode"].num())
			                               : p["reason"].str();
			if (!detail.empty()) status_.error += ": " + detail;
		}
	}
}

}  // namespace sdlcastg
