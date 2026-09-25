// castplay - sdlcastg の見本（コマンドライン）
//
//   castplay --list [秒]                   探して一覧を出す（既定 5 秒）
//   castplay --status <IP|名前>            つないで受信側の状態だけ見る
//                                          （受信側の表示は変わらない）
//   castplay <IP|名前> <ファイル> [秒]      ファイルを配って再生させる
//   castplay <IP|名前> --url <URL> [型] [秒] URL を再生させる（型の既定は拡張子から）
//   castplay <IP|名前> --live [秒] [WxH]    作った絵と音をその場でエンコードして流す
//                                          （SDL の要らない sdlcastgdemo。Android の
//                                          端末のシェルで試すため。既定 1280x720）
//
// 再生は、秒数を過ぎるか、受信側で終わるか、Ctrl+C で止める（受信アプリも止める）。
// 表示は英語（コンソールの文字コードに左右されないように）。

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "sdlcastg.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

volatile sig_atomic_t g_interrupted = 0;

void OnSignal(int) {
	g_interrupted = 1;
}

void SleepMs(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

double NowSec() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	           .count() /
	       1000.0;
}

bool IsIPv4(const char *s) {
	int a, b, c, d;
	char tail;
	return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4;
}

std::vector<SDLCastG_Device> Discover(int seconds) {
	std::vector<SDLCastG_Device> out;
	if (SDLCastG_StartDiscovery() != 0) {
		fprintf(stderr, "discovery failed: %s\n", SDLCastG_GetError());
		return out;
	}
	const double end = NowSec() + seconds;
	while (NowSec() < end && !g_interrupted) SleepMs(100);
	SDLCastG_Device list[32];
	const int n = SDLCastG_GetDevices(list, 32);
	for (int i = 0; i < n && i < 32; i++) out.push_back(list[i]);
	SDLCastG_StopDiscovery();
	return out;
}

// IP ならそのまま（名前も尋ねてみる）、名前なら探して決める。
bool Resolve(const char *target, SDLCastG_Device *out) {
	memset(out, 0, sizeof(*out));
	if (IsIPv4(target)) {
		if (SDLCastG_ProbeDevice(target, out) != 0) {
			snprintf(out->address, sizeof(out->address), "%s", target);
			snprintf(out->name, sizeof(out->name), "%s", target);
			out->port = 8009;
		}
		return true;
	}
	const std::vector<SDLCastG_Device> list = Discover(4);
	for (size_t i = 0; i < list.size(); i++) {
		if (strcmp(list[i].name, target) == 0 || strcmp(list[i].id, target) == 0) {
			*out = list[i];
			return true;
		}
	}
	fprintf(stderr, "device not found: %s\n", target);
	return false;
}

void PrintStatus(const SDLCastG_Status &s) {
	printf("[%7.2f] state=%-12s", NowSec(), SDLCastG_StateName(s.state));
	if (s.runningApp[0]) printf(" app=\"%s\"", s.runningApp);
	if (s.playerState[0]) printf(" player=%s", s.playerState);
	if (s.idleReason[0]) printf(" idle=%s", s.idleReason);
	if (s.state == SDLCASTG_PLAYING || s.state == SDLCASTG_BUFFERING || s.state == SDLCASTG_PAUSED) {
		printf(" t=%.2f", s.currentTime);
	}
	printf(" vol=%.2f%s", s.volume, s.muted ? "(muted)" : "");
	if (s.error[0]) printf(" error=\"%s\"", s.error);
	printf("\n");
	fflush(stdout);
}

// 状態が変わるか、every 秒たつたびに表示する。stopWhen のどれかになったら抜ける。
SDLCastG_Status Watch(double seconds, double every, bool untilConnected) {
	SDLCastG_Status s, last;
	memset(&last, 0, sizeof(last));
	last.state = (SDLCastG_State)-1;
	const double end = NowSec() + seconds;
	double nextPrint = 0.0;
	for (;;) {
		SDLCastG_GetStatus(&s);
		const double now = NowSec();
		if (s.state != last.state || strcmp(s.playerState, last.playerState) != 0 ||
		    strcmp(s.runningApp, last.runningApp) != 0 || now >= nextPrint) {
			PrintStatus(s);
			nextPrint = now + every;
			last = s;
		}
		if (g_interrupted || now >= end) break;
		if (s.state == SDLCASTG_ERROR || s.state == SDLCASTG_DISCONNECTED) break;
		if (untilConnected && s.state != SDLCASTG_CONNECTING) break;
		if (s.state == SDLCASTG_IDLE && s.idleReason[0]) break;
		SleepMs(100);
	}
	return s;
}

// --live の音は毎秒の頭のビープと小さい通奏音。
// --live の絵: 右下の 3/4 は止まった模様、左上に音量の棒のように伸び縮みする
// 縦棒、毎秒の頭に右上の白い四角（音のビープと同じ時刻。ずれを目で見る）。
void MakeLiveImage(int64_t t, int w, int h, std::vector<uint8_t> *rgba, bool first) {
	if (first) {
		rgba->assign((size_t)w * h * 4, 255);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint8_t *p = &(*rgba)[((size_t)y * w + x) * 4];
				p[0] = (uint8_t)(x * 255 / w);
				p[1] = (uint8_t)(y * 255 / h);
				p[2] = ((x / 32 + y / 32) & 1) ? 160 : 80;
			}
		}
	}
	const int bw = w / 3, bh = h / 4;
	for (int y = 0; y < bh; y++) {
		for (int x = 0; x < bw; x++) {
			uint8_t *p = &(*rgba)[((size_t)y * w + x) * 4];
			const int level = (int)((t / 20 + (x / 16) * 37) % 97) * bh / 97;
			const bool on = (x % 16) < 12 && bh - y <= level;
			p[0] = on ? 255 : 0;
			p[1] = on ? (uint8_t)(y * 255 / bh) : 0;
			p[2] = on ? 0 : 40;
		}
	}
	const bool flash = (t % 1000) < 100;
	for (int y = 0; y < h / 5; y++) {
		for (int x = w - w / 6; x < w; x++) {
			uint8_t *p = &(*rgba)[((size_t)y * w + x) * 4];
			p[0] = p[1] = p[2] = flash ? 255 : 30;
		}
	}
}

// 作った絵と音を流す。流れの時計は渡した音の長さ。
int RunLive(double seconds, int w, int h) {
	SDLCastG_StreamConfig cfg;
	SDLCastG_DefaultStreamConfig(&cfg);
	cfg.width = w;
	cfg.height = h;
	cfg.title = "sdlcastg castplay --live";
	Watch(5.0, 10.0, true);  // 受信側の状態が来るまで
	if (SDLCastG_StartStream(&cfg) != 0) {
		fprintf(stderr, "stream failed: %s\n", SDLCastG_GetError());
		return 1;
	}
	int rc = 0;
	const double start = NowSec();
	uint64_t audioFrames = 0;
	std::vector<int16_t> pcm;
	std::vector<uint8_t> rgba;
	bool first = true;
	double nextPrint = 2.0;
	SDLCastG_State lastState = (SDLCastG_State)-1;
	for (;;) {
		const double now = NowSec() - start;
		if (g_interrupted || now >= seconds) break;

		const uint64_t want = (uint64_t)(now * 48000);
		if (want > audioFrames) {
			const int n = (int)(want - audioFrames);
			pcm.resize((size_t)n * 2);
			for (int i = 0; i < n; i++) {
				const uint64_t k = audioFrames + (uint64_t)i;
				// 毎秒の頭のビープ（880Hz）と、小さい通奏音（220Hz。途切れたら耳で分かる）。
				double a = 600 * std::sin(2 * 3.14159265 * 220 * k / 48000.0);
				if (k % 48000 < 4800) a += 8000 * std::sin(2 * 3.14159265 * 880 * k / 48000.0);
				const int16_t v = (int16_t)a;
				pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = v;
			}
			SDLCastG_SubmitAudio(&pcm[0], n);
			audioFrames += (uint64_t)n;
		}
		if (SDLCastG_WantVideoFrame()) {
			const int64_t t = (int64_t)(audioFrames * 1000 / 48000);
			MakeLiveImage(t, w, h, &rgba, first);
			first = false;
			SDLCastG_SubmitVideoRGBA(&rgba[0], w, h, w * 4, t);
		}

		SDLCastG_Status st;
		SDLCastG_GetStatus(&st);
		if (st.state != lastState || now >= nextPrint) {
			SDLCastG_StreamStats ss;
			SDLCastG_GetStreamStats(&ss);
			// 受信側の再生位置は、つないだところからの時刻（clientBaseMs）。
			const double lag = (ss.audioMs - ss.clientBaseMs) / 1000.0 - st.currentTime;
			printf("[%6.1f] %-10s t=%7.2f stream=%7.2f lag=%5.2f clients=%d frames=%llu drop=%llu "
			       "enc=%.1fms silence=%lldms sent=%lldKB%s%s\n",
			       now, SDLCastG_StateName(st.state), st.currentTime, ss.audioMs / 1000.0,
			       (st.state == SDLCASTG_PLAYING) ? lag : 0.0, ss.clients,
			       (unsigned long long)ss.videoFrames, (unsigned long long)ss.droppedFrames,
			       ss.encodeMsAvg, (long long)ss.silenceMs, (long long)(ss.bytesSent / 1024),
			       st.error[0] ? " error=" : "", st.error);
			fflush(stdout);
			lastState = st.state;
			if (now >= nextPrint) nextPrint = now + 2.0;
		}
		if (st.state == SDLCASTG_ERROR || st.state == SDLCASTG_DISCONNECTED ||
		    (st.state == SDLCASTG_IDLE && st.idleReason[0])) {
			fprintf(stderr, "receiver stopped: %s %s\n", st.idleReason, st.error);
			rc = 1;
			break;
		}
		SleepMs(8);
	}
	printf("stopping\n");
	SDLCastG_StopApp();
	SleepMs(1000);
	SDLCastG_StopStream();
	return rc;
}

int Usage() {
	fprintf(stderr,
	        "usage:\n"
	        "  castplay --list [seconds]\n"
	        "  castplay --status <ip|name>\n"
	        "  castplay <ip|name> <file> [seconds]\n"
	        "  castplay <ip|name> --url <url> [content-type] [seconds]\n"
	        "  castplay <ip|name> --live [seconds] [WxH]\n");
	return 2;
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);  // 受信側の名前は UTF-8
#endif
	signal(SIGINT, OnSignal);
	if (argc < 2) return Usage();
	if (SDLCastG_Init() != 0) {
		fprintf(stderr, "init failed: %s\n", SDLCastG_GetError());
		return 1;
	}

	int rc = 0;
	const std::string cmd = argv[1];
	if (cmd == "--list") {
		const int sec = (argc > 2) ? atoi(argv[2]) : 5;
		const std::vector<SDLCastG_Device> list = Discover(sec > 0 ? sec : 5);
		printf("%d device(s)\n", (int)list.size());
		for (size_t i = 0; i < list.size(); i++) {
			printf("  %-20s %-16s %s:%d  ca=%d%s  id=%s\n", list[i].name, list[i].model,
			       list[i].address, list[i].port, list[i].capabilities,
			       (list[i].capabilities >= 0 && !(list[i].capabilities & SDLCASTG_CAP_VIDEO_OUT))
			           ? " (no video)"
			           : "",
			       list[i].id);
		}
	} else if (cmd == "--status") {
		SDLCastG_Device d;
		if (argc < 3 || !Resolve(argv[2], &d)) {
			rc = 1;
		} else {
			printf("device: %s (%s) %s:%d\n", d.name, d.model, d.address, d.port);
			if (SDLCastG_Connect(d.address, d.port) != 0) {
				fprintf(stderr, "connect failed: %s\n", SDLCastG_GetError());
				rc = 1;
			} else {
				Watch(5.0, 10.0, true);
				SDLCastG_Disconnect();
			}
		}
	} else {
		SDLCastG_Device d;
		if (argc < 3) {
			rc = Usage();
		} else if (!Resolve(argv[1], &d)) {
			rc = 1;
		} else {
			std::string url, type;
			double seconds = 1e9;
			const bool isUrl = (strcmp(argv[2], "--url") == 0);
			const bool isLive = (strcmp(argv[2], "--live") == 0);
			printf("device: %s (%s) %s:%d\n", d.name, d.model, d.address, d.port);
			if (SDLCastG_Connect(d.address, d.port) != 0) {
				fprintf(stderr, "connect failed: %s\n", SDLCastG_GetError());
				rc = 1;
			} else if (isLive) {
				int w = 1280, h = 720;
				if (argc > 3) seconds = atof(argv[3]);
				if (argc > 4 && sscanf(argv[4], "%dx%d", &w, &h) != 2) rc = Usage();
				if (rc == 0) rc = RunLive(seconds, w & ~1, h & ~1);
				SDLCastG_Disconnect();
			} else {
				char buf[512] = { 0 };
				if (isUrl) {
					if (argc < 4) {
						rc = Usage();
					} else {
						url = argv[3];
						if (argc > 4) type = argv[4];
						if (argc > 5) seconds = atof(argv[5]);
					}
				} else {
					if (argc > 3) seconds = atof(argv[3]);
					if (SDLCastG_ServeFile(argv[2], 0, buf, sizeof(buf)) != 0) {
						fprintf(stderr, "serve failed: %s\n", SDLCastG_GetError());
						rc = 1;
					} else {
						url = buf;
					}
				}
				if (rc == 0) {
					printf("url: %s\n", url.c_str());
					Watch(5.0, 10.0, true);  // 受信側の状態が来るまで
					if (SDLCastG_LoadURL(url.c_str(), type.empty() ? 0 : type.c_str(), 0,
					                     "sdlcastg castplay") != 0) {
						fprintf(stderr, "load failed: %s\n", SDLCastG_GetError());
						rc = 1;
					} else {
						const SDLCastG_Status s = Watch(seconds, 2.0, false);
						if (s.state == SDLCASTG_ERROR) rc = 1;
					}
					printf("stopping the receiver app\n");
					SDLCastG_StopApp();
					SleepMs(1500);
					SDLCastG_Status s;
					SDLCastG_GetStatus(&s);
					PrintStatus(s);
				}
				SDLCastG_Disconnect();
			}
		}
	}

	SDLCastG_Quit();
	return rc;
}
