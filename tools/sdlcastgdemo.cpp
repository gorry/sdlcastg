// sdlcastgdemo - sdlcastg の見本（SDL2 で描いた絵と作った音を Google Cast 対応機器へ流す）
//
//   sdlcastgdemo <IP|名前> [秒] [--show] [--min-kbps N]
//
// 窓は既定で隠したまま（--show で出す）。描くのは 1280x720 の描画先テクスチャで、
// そこから読み出して送る。音は毎秒の頭に 100ms のビープ。
// **ビープと同じ時刻に白い四角を出す**ので、TV で両者がそろっていれば
// 映像と音の時刻合わせが合っている。
//
// 2 秒ごとに、受信側の再生位置と流れの時刻の差（遅れ）などを出す。
// 表示は英語（コンソールの文字コードに左右されないように）。

#define _USE_MATH_DEFINES
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "sdlcastg_sdl.h"

namespace {

const int kWidth = 1280;
const int kHeight = 720;
const int kSampleRate = 48000;

volatile sig_atomic_t g_interrupted = 0;

void OnSignal(int) {
	g_interrupted = 1;
}

bool IsIPv4(const char *s) {
	int a, b, c, d;
	char tail;
	return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4;
}

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
	if (SDLCastG_StartDiscovery() != 0) return false;
	const Uint32 end = SDL_GetTicks() + 4000;
	while (SDL_GetTicks() < end) SDL_Delay(100);
	SDLCastG_Device list[32];
	const int n = SDLCastG_GetDevices(list, 32);
	SDLCastG_StopDiscovery();
	for (int i = 0; i < n && i < 32; i++) {
		if (strcmp(list[i].name, target) == 0 || strcmp(list[i].id, target) == 0) {
			*out = list[i];
			return true;
		}
	}
	fprintf(stderr, "device not found: %s\n", target);
	return false;
}

// 7 セグメントで数字を描く（字の素材を持たずに済ませる）。
void DrawDigit(SDL_Renderer *r, int d, int x, int y, int s) {
	static const unsigned char kSeg[10] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66,
	                                        0x6d, 0x7d, 0x07, 0x7f, 0x6f };
	const int w = s * 4, h = s * 7, t = s;
	const SDL_Rect segs[7] = {
		{ x + t, y, w - 2 * t, t },              // a
		{ x + w - t, y + t, t, h / 2 - t },      // b
		{ x + w - t, y + h / 2, t, h / 2 - t },  // c
		{ x + t, y + h - t, w - 2 * t, t },      // d
		{ x, y + h / 2, t, h / 2 - t },          // e
		{ x, y + t, t, h / 2 - t },              // f
		{ x + t, y + h / 2 - t / 2, w - 2 * t, t },  // g
	};
	for (int i = 0; i < 7; i++) {
		if (kSeg[d] & (1 << i)) SDL_RenderFillRect(r, &segs[i]);
	}
}

void DrawNumber(SDL_Renderer *r, long v, int x, int y, int s) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", v);
	for (int i = 0; buf[i]; i++) DrawDigit(r, buf[i] - '0', x + i * s * 5, y, s);
}

// 流れの時刻 tMs の絵。
void DrawScene(SDL_Renderer *r, int64_t tMs) {
	const double sec = tMs / 1000.0;
	SDL_SetRenderDrawColor(r, (Uint8)(40 + 30 * sin(sec * 0.5)), 40, (Uint8)(70 + 30 * cos(sec * 0.3)),
	                       255);
	SDL_RenderClear(r);

	// 横に流れる帯（4 秒で一巡）。
	const int bx = (int)((tMs % 4000) * (kWidth + 200) / 4000) - 200;
	SDL_SetRenderDrawColor(r, 80, 200, 255, 255);
	SDL_Rect band = { bx, 80, 200, 60 };
	SDL_RenderFillRect(r, &band);

	// 跳ねる四角。
	const double ph = fmod(sec, 2.0);
	const int by = (int)(560 - 300 * fabs(sin(ph * M_PI)));
	SDL_SetRenderDrawColor(r, 255, 160, 60, 255);
	SDL_Rect ball = { 200 + (int)(tMs / 10 % 800), by, 60, 60 };
	SDL_RenderFillRect(r, &ball);

	// ビープと同じ時刻の白い四角（毎秒の頭の 100ms）。
	if (tMs % 1000 < 100) {
		SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
		SDL_Rect flash = { kWidth - 260, 200, 200, 200 };
		SDL_RenderFillRect(r, &flash);
	}

	// 経過秒と、1/10 秒。
	SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
	DrawNumber(r, (long)(tMs / 1000), 80, 220, 16);
	SDL_SetRenderDrawColor(r, 180, 180, 180, 255);
	DrawNumber(r, (long)(tMs / 100 % 10), 80, 380, 8);
}

// 流れの時刻 fromFrame からの frames ぶんの音（毎秒の頭に 880Hz を 100ms）。
void MakeAudio(uint64_t fromFrame, int frames, std::vector<int16_t> *out) {
	out->resize((size_t)frames * 2);
	for (int i = 0; i < frames; i++) {
		const uint64_t n = fromFrame + (uint64_t)i;
		const uint64_t inSec = n % kSampleRate;
		// 小さい通奏音（220Hz）を鳴らし続ける。途切れたら耳で分かる。
		int16_t v = (int16_t)(600 * sin(2.0 * M_PI * 220.0 * (double)n / kSampleRate));
		if (inSec < (uint64_t)kSampleRate / 10) {
			v = (int16_t)(v + 8000 * sin(2.0 * M_PI * 880.0 * (double)n / kSampleRate));
		}
		(*out)[(size_t)i * 2] = v;
		(*out)[(size_t)i * 2 + 1] = v;
	}
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: sdlcastgdemo <ip|name> [seconds] [--show] [--min-kbps N]\n");
		return 2;
	}
	double seconds = 60.0;
	bool show = false;
	int minKbps = 0;  // 0 なら既定
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--show") == 0) {
			show = true;
		} else if (strcmp(argv[i], "--min-kbps") == 0 && i + 1 < argc) {
			minKbps = atoi(argv[++i]);
		} else {
			seconds = atof(argv[i]);
		}
	}
	signal(SIGINT, OnSignal);

	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window *win = SDL_CreateWindow("sdlcastgdemo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                   kWidth / 2, kHeight / 2, show ? 0 : SDL_WINDOW_HIDDEN);
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
	                                                          SDL_RENDERER_TARGETTEXTURE)
	                        : 0;
	SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
	                                           SDL_TEXTUREACCESS_TARGET, kWidth, kHeight)
	                       : 0;
	if (tex == 0) {
		fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
		return 1;
	}

	int rc = 0;
	SDLCastG_Init();
	SDLCastG_Device d;
	if (!Resolve(argv[1], &d)) {
		rc = 1;
	} else {
		printf("device: %s (%s) %s:%d\n", d.name, d.model, d.address, d.port);
		SDLCastG_StreamConfig cfg;
		SDLCastG_DefaultStreamConfig(&cfg);
		cfg.width = kWidth;
		cfg.height = kHeight;
		cfg.title = "sdlcastg demo";
		if (minKbps != 0) cfg.minKbps = minKbps;
		if (SDLCastG_Connect(d.address, d.port) != 0) {
			fprintf(stderr, "connect failed: %s\n", SDLCastG_GetError());
			rc = 1;
		} else {
			// 受信側の状態が来るまで待ってから始める。
			SDLCastG_Status st;
			for (int i = 0; i < 50; i++) {
				SDLCastG_GetStatus(&st);
				if (st.state != SDLCASTG_CONNECTING) break;
				SDL_Delay(100);
			}
			if (SDLCastG_StartStream(&cfg) != 0) {
				fprintf(stderr, "stream failed: %s\n", SDLCastG_GetError());
				rc = 1;
			}
		}

		if (rc == 0) {
			const Uint64 freq = SDL_GetPerformanceFrequency();
			const Uint64 start = SDL_GetPerformanceCounter();
			uint64_t audioFrames = 0;
			std::vector<int16_t> pcm;
			double nextPrint = 2.0;
			SDLCastG_State lastState = (SDLCastG_State)-1;
			for (;;) {
				SDL_Event ev;
				while (SDL_PollEvent(&ev)) {
					if (ev.type == SDL_QUIT) g_interrupted = 1;
				}
				const double now = (double)(SDL_GetPerformanceCounter() - start) / freq;
				if (g_interrupted || now >= seconds) break;

				// 実時間ぶんの音を作って渡す（これが流れの時計になる）。
				const uint64_t want = (uint64_t)(now * kSampleRate);
				if (want > audioFrames) {
					const int n = (int)(want - audioFrames);
					MakeAudio(audioFrames, n, &pcm);
					SDLCastG_SubmitAudio(&pcm[0], n);
					audioFrames += (uint64_t)n;
				}

				// 同じ時計の上の時刻で絵を描いて渡す。
				const int64_t t = (int64_t)(audioFrames * 1000 / kSampleRate);
				SDL_SetRenderTarget(ren, tex);
				DrawScene(ren, t);
				SDLCastG_SubmitVideoFromRenderer(ren, t);
				SDL_SetRenderTarget(ren, 0);
				if (show) {
					SDL_RenderCopy(ren, tex, 0, 0);
					SDL_RenderPresent(ren);
				}

				SDLCastG_Status st;
				SDLCastG_GetStatus(&st);
				if (st.state != lastState || now >= nextPrint) {
					SDLCastG_StreamStats ss;
					SDLCastG_GetStreamStats(&ss);
					// 受信側の再生位置は、つないだところからの時刻（clientBaseMs）。
					const double lag = (ss.audioMs - ss.clientBaseMs) / 1000.0 - st.currentTime;
					printf("[%6.1f] %-10s t=%7.2f stream=%7.2f lag=%5.2f clients=%d frames=%llu "
					       "drop=%llu enc=%.1fms silence=%lldms sent=%lldKB%s%s\n",
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
				SDL_Delay(8);
			}
			printf("stopping\n");
			SDLCastG_StopApp();
			SDL_Delay(1000);
			SDLCastG_StopStream();
		}
		SDLCastG_Disconnect();
	}
	SDLCastG_Quit();

	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return rc;
}
