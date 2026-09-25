// sdlcastg - 公開 API（include/sdlcastg.h）

#include "sdlcastg.h"

#include <stdio.h>
#include <string.h>

#include <memory>
#include <mutex>
#include <string>

#include "discovery.h"
#include "encoder.h"
#include "httpserver.h"
#include "net.h"
#include "sdlcastg_version.h"  // CMake が Profile.ini から生成する
#include "session.h"

namespace {

using namespace sdlcastg;

struct Global {
	std::mutex mutex;
	bool initialized;
	std::string error;
	Discovery discovery;
	Session session;
	HttpServer http;
	int servedCount;
	// 流す（SDLCastG_StartStream）。Submit* はこの鍵を取らずに直に呼ぶ
	// （音声のコールバックから毎回呼ばれるため。StreamEncoder が自分で守る）。
	StreamEncoder encoder;
	std::string livePath;
	std::string liveTitle;

	Global() : initialized(false), servedCount(0) {}
};

Global &G() {
	static Global g;
	return g;
}

// 流れを新しい URL で配り直し、受信アプリに読み込ませる（鍵を持って呼ぶ）。
// URL を替えるのは、同じ URL だと受信側が続きとして扱うかもしれないため。
// 受信側からは新しい読み手として、つないだあとのキーフレームから時刻 0 で届く
// （LiveSource）ので、溜まっていたぶんを捨てて今の位置から再生し直す。
void LoadLiveLocked(const std::string &local);

int Fail(const std::string &e) {
	G().error = e;
	return -1;
}

void CopyStr(char *dst, size_t size, const std::string &src) {
	if (size == 0) return;
	const size_t n = (src.size() < size - 1) ? src.size() : size - 1;
	memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

void ToC(const CastDevice &d, SDLCastG_Device *out) {
	memset(out, 0, sizeof(*out));
	CopyStr(out->id, sizeof(out->id), d.id);
	CopyStr(out->name, sizeof(out->name), d.name);
	CopyStr(out->model, sizeof(out->model), d.model);
	CopyStr(out->address, sizeof(out->address), d.address);
	out->port = d.port;
	out->capabilities = d.capabilities;
}

// 配るファイルの path に使う名前（URL に載せられる文字だけにする）。
std::string SafeName(const std::string &path) {
	size_t slash = path.find_last_of("/\\");
	const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
	std::string out;
	for (size_t i = 0; i < base.size(); i++) {
		const char c = base[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		                c == '.' || c == '-' || c == '_';
		out.push_back(ok ? c : '_');
	}
	return out.empty() ? std::string("media") : out;
}

void LoadLiveLocked(const std::string &local) {
	Global &g = G();
	if (!g.livePath.empty()) g.http.Unserve(g.livePath);
	char path[48];
	snprintf(path, sizeof(path), "/live/%d.webm", ++g.servedCount);
	g.livePath = path;
	g.http.Serve(g.livePath, g.encoder.source());

	char url[256];
	snprintf(url, sizeof(url), "http://%s:%d%s", local.c_str(), g.http.port(), path);
	LoadRequest req;
	req.url = url;
	req.contentType = "video/webm";
	req.live = true;
	req.title = g.liveTitle;
	g.session.Load(req);
}

}  // namespace

extern "C" {

int SDLCastG_Init(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.initialized) return 0;
	if (!net::Init()) return Fail("network init failed");
	g.initialized = true;
	return 0;
}

void SDLCastG_Quit(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	if (!g.initialized) return;
	g.encoder.Stop();
	g.session.Disconnect();
	g.discovery.Stop();
	g.http.Stop();
	net::Quit();
	g.initialized = false;
}

const char *SDLCastG_GetVersion(void) {
	return SDLCASTG_VERSION;
}

const char *SDLCastG_GetError(void) {
	return G().error.c_str();
}

int SDLCastG_StartDiscovery(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.discovery.running()) return 0;
	std::string e;
	if (!g.discovery.Start(&e)) return Fail(e);
	return 0;
}

void SDLCastG_StopDiscovery(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	g.discovery.Stop();
}

int SDLCastG_GetDevices(SDLCastG_Device *out, int max) {
	const std::vector<CastDevice> list = G().discovery.devices();
	for (int i = 0; i < max && i < (int)list.size(); i++) ToC(list[(size_t)i], &out[i]);
	return (int)list.size();
}

int SDLCastG_ProbeDevice(const char *address, SDLCastG_Device *out) {
	CastDevice d;
	if (!Discovery::Probe(address ? address : "", 2000, &d)) {
		std::lock_guard<std::mutex> lock(G().mutex);
		return Fail(std::string("no answer from ") + (address ? address : ""));
	}
	ToC(d, out);
	return 0;
}

int SDLCastG_Connect(const char *address, int port) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	std::string e;
	if (!g.session.Connect(address ? address : "", (port > 0) ? port : 8009, &e)) return Fail(e);
	return 0;
}

void SDLCastG_Disconnect(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	g.session.Disconnect();
}

int SDLCastG_LoadURL(const char *url, const char *contentType, int live, const char *title) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	const SessionStatus st = g.session.status();
	if (st.state == SessionStatus::kDisconnected || st.state == SessionStatus::kConnecting) {
		return Fail("not connected");
	}
	LoadRequest req;
	req.url = url ? url : "";
	req.contentType = contentType ? contentType : GuessContentType(req.url);
	req.live = (live != 0);
	req.title = title ? title : "";
	g.session.Load(req);
	return 0;
}

int SDLCastG_ServeFile(const char *path, const char *contentType, char *urlOut, size_t urlSize) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	const std::string local = g.session.localAddress();
	if (local.empty()) return Fail("connect first (the URL needs the local address)");
	const std::string p = path ? path : "";
	std::shared_ptr<FileSource> src(
	    new FileSource(p, contentType ? contentType : GuessContentType(p)));
	if (!src->valid()) return Fail("cannot open " + p);
	std::string e;
	if (!g.http.Start(0, &e)) return Fail(e);
	char prefix[32];
	snprintf(prefix, sizeof(prefix), "/media/%d/", ++g.servedCount);
	const std::string urlPath = prefix + SafeName(p);
	g.http.Serve(urlPath, src);
	char url[256];
	snprintf(url, sizeof(url), "http://%s:%d%s", local.c_str(), g.http.port(), urlPath.c_str());
	CopyStr(urlOut, urlSize, url);
	return 0;
}

int SDLCastG_StopApp(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	g.session.Stop();
	return 0;
}

int SDLCastG_SetVolume(float level) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	g.session.SetVolume(level);
	return 0;
}

int SDLCastG_SeekToLive(int behindMs) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	if (!g.encoder.running()) return Fail("not streaming");
	// 受信側の時刻は、つないだところから 0（LiveSource）。流れの今の位置をそれに直す。
	const StreamStats st = g.encoder.stats();
	const int64_t clientMs = st.audioMs - st.clientBaseMs - behindMs;
	g.session.Seek(clientMs / 1000.0);
	return 0;
}

void SDLCastG_DefaultStreamConfig(SDLCastG_StreamConfig *cfg) {
	const StreamConfig d;
	memset(cfg, 0, sizeof(*cfg));
	cfg->width = d.width;
	cfg->height = d.height;
	cfg->fps = d.fps;
	cfg->videoKbps = d.videoKbps;
	cfg->audioKbps = d.audioKbps;
	cfg->minKbps = d.minKbps;
	cfg->title = 0;
}

int SDLCastG_StartStream(const SDLCastG_StreamConfig *cfgIn) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	const std::string local = g.session.localAddress();
	const SessionStatus st = g.session.status();
	if (local.empty() || st.state == SessionStatus::kDisconnected ||
	    st.state == SessionStatus::kConnecting) {
		return Fail("not connected");
	}
	SDLCastG_StreamConfig c;
	SDLCastG_DefaultStreamConfig(&c);
	if (cfgIn != 0) c = *cfgIn;
	StreamConfig cfg;
	if (c.width > 0) cfg.width = c.width;
	if (c.height > 0) cfg.height = c.height;
	if (c.fps > 0) cfg.fps = c.fps;
	if (c.videoKbps > 0) cfg.videoKbps = c.videoKbps;
	if (c.audioKbps > 0) cfg.audioKbps = c.audioKbps;
	cfg.minKbps = (c.minKbps < 0) ? 0 : c.minKbps;

	std::string e;
	if (!g.http.Start(0, &e)) return Fail(e);
	if (!g.encoder.Start(cfg, &e)) return Fail(e);
	g.liveTitle = c.title ? c.title : "";
	LoadLiveLocked(local);
	return 0;
}

int SDLCastG_ReloadStream(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	const std::string local = g.session.localAddress();
	if (!g.encoder.running() || g.livePath.empty() || local.empty()) return Fail("not streaming");
	LoadLiveLocked(local);
	return 0;
}

void SDLCastG_StopStream(void) {
	Global &g = G();
	std::lock_guard<std::mutex> lock(g.mutex);
	g.encoder.Stop();
	if (!g.livePath.empty()) {
		g.http.Unserve(g.livePath);
		g.livePath.clear();
	}
}

int SDLCastG_SubmitAudio(const int16_t *pcm, int frames) {
	G().encoder.SubmitAudio(pcm, frames);
	return 0;
}

int SDLCastG_SubmitVideoRGBA(const void *pixels, int width, int height, int pitch, int64_t ptsMs) {
	G().encoder.SubmitVideoRGBA(pixels, width, height, pitch, ptsMs);
	return 0;
}

int SDLCastG_WantVideoFrame(void) {
	return G().encoder.WantVideoFrame() ? 1 : 0;
}

int64_t SDLCastG_StreamTimeMs(void) {
	return G().encoder.submittedAudioMs();
}

int SDLCastG_GetStreamSize(int *w, int *h) {
	const StreamEncoder &e = G().encoder;
	if (!e.running()) return 0;
	if (w != 0) *w = e.config().width;
	if (h != 0) *h = e.config().height;
	return 1;
}

void SDLCastG_GetStreamStats(SDLCastG_StreamStats *out) {
	const StreamStats s = G().encoder.stats();
	memset(out, 0, sizeof(*out));
	out->audioMs = s.audioMs;
	out->videoMs = s.videoMs;
	out->silenceMs = s.silenceMs;
	out->videoFrames = s.videoFrames;
	out->droppedFrames = s.droppedFrames;
	out->sameTimeFrames = s.sameTimeFrames;
	out->encodeMsAvg = s.encodeMsAvg;
	out->clients = s.clients;
	out->bytesSent = s.bytesSent;
	out->clientBaseMs = s.clientBaseMs;
}

void SDLCastG_GetStatus(SDLCastG_Status *out) {
	const SessionStatus s = G().session.status();
	memset(out, 0, sizeof(*out));
	out->state = (SDLCastG_State)s.state;
	CopyStr(out->error, sizeof(out->error), s.error);
	CopyStr(out->runningApp, sizeof(out->runningApp), s.runningAppName);
	out->volume = s.volume;
	out->muted = s.muted ? 1 : 0;
	CopyStr(out->playerState, sizeof(out->playerState), s.playerState);
	CopyStr(out->prevPlayerState, sizeof(out->prevPlayerState), s.prevPlayerState);
	CopyStr(out->idleReason, sizeof(out->idleReason), s.idleReason);
	out->currentTime = s.currentTime;
}

const char *SDLCastG_StateName(SDLCastG_State s) {
	return StateName((SessionStatus::State)s);
}

}  // extern "C"
