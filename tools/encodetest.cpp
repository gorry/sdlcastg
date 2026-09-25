// encodetest - sdlcastg のエンコードと WebM の書き出しを、受信側なしで確かめる
//
//   encodetest <出力.webm> [秒] [--late] [--bench] [--size WxH] [--src WxH] [--fps N]
//
// StreamEncoder に作った絵（動く縦帯）と音（毎秒の頭のビープ）を実時間で入れ、
// LiveSource から読んだものをファイルに書く。ffprobe / ffmpeg で読めるか、
// 時刻がそろっているかを見る。--late は、絵を渡すのを途中で 3 秒止める
// （最後の絵の出し直しを確かめる）。音を途中で 2 秒止めるのは常に行う
// （無音の足しを確かめる）。
//
// --bench は止めずに流し、CPU の重さを出す（Android の端末で測るため。
// docs/design.md）。--size は送る大きさ（既定 640x360）、--src は渡す絵の
// 大きさ（既定 800x600）。絵を作る時間は CPU の時間から引いて出す。--static は
// 一部だけが動く絵（音楽プレーヤーのような、ほとんど止まった画面に近い）。--threads は VP8 のスレッド数。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "encoder.h"

using namespace sdlcastg;

namespace {

// このプロセスが使った CPU の時間（秒。全スレッドの合計）。
double ProcessCpuSeconds() {
#ifdef _WIN32
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
	const auto sec = [](const FILETIME &f) {
		return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7;
	};
	return sec(k) + sec(u);
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
	return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec +
	       ru.ru_stime.tv_usec / 1e6;
#endif
}

bool ParseSize(const char *s, int *w, int *h) {
	return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr,
		        "usage: encodetest <out.webm> [seconds] [--late] [--bench] [--size WxH] [--src WxH] "
		        "[--fps N] [--threads N] [--static] [--kbps N] [--video-lag MS] [--advance-at SEC:MS]\n");
		return 2;
	}
	double seconds = 10.0;
	bool late = false;
	bool bench = false;
	bool staticImage = false;
	int videoLagMs = 0;  // --video-lag: 絵の時刻を音より遅らせて渡す（いま聞こえている音の時刻で絵を渡すアプリと同じ形）
	// --advance-at SEC:MS: SEC 秒から、絵の時刻を MS だけ早めて渡す（受信側での
	// 絵の遅れを補う設定を試すため。中身はそのまま、時刻だけ前へ）
	double advanceAtSec = -1;
	int advanceAtMs = 0;
	int advanceMs = 0;
	StreamConfig cfg;
	cfg.width = 640;
	cfg.height = 360;
	int W = 800, H = 600;  // 送る大きさと違う縦横比（黒帯と縮小を確かめる）
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--late") == 0) {
			late = true;
		} else if (strcmp(argv[i], "--bench") == 0) {
			bench = true;
		} else if (strcmp(argv[i], "--static") == 0) {
			staticImage = true;
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			if (!ParseSize(argv[++i], &cfg.width, &cfg.height)) return 2;
		} else if (strcmp(argv[i], "--src") == 0 && i + 1 < argc) {
			if (!ParseSize(argv[++i], &W, &H)) return 2;
		} else if (strcmp(argv[i], "--kbps") == 0 && i + 1 < argc) {
			cfg.videoKbps = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--advance-at") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%lf:%d", &advanceAtSec, &advanceAtMs) != 2) return 2;
		} else if (strcmp(argv[i], "--video-lag") == 0 && i + 1 < argc) {
			videoLagMs = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			cfg.threads = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
			cfg.fps = atoi(argv[++i]);
		} else {
			seconds = atof(argv[i]);
		}
	}

	StreamEncoder enc;
	std::string err;
	if (!enc.Start(cfg, &err)) {
		fprintf(stderr, "start failed: %s\n", err.c_str());
		return 1;
	}

	FILE *out = fopen(argv[1], "wb");
	if (out == 0) return 1;
	std::shared_ptr<LiveSource> src = enc.source();
	HttpStream *reader = src->Open(0);
	std::thread readThread([&]() {
		std::vector<char> buf(65536);
		for (;;) {
			const int n = reader->Read(&buf[0], (int)buf.size());
			if (n <= 0) break;
			fwrite(&buf[0], 1, (size_t)n, out);
		}
	});

	std::vector<uint8_t> rgba((size_t)W * H * 4);
	if (staticImage) {  // 動かないところ（右下の 3/4）は最初に 1 回だけ描く
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				uint8_t *p = &rgba[((size_t)y * W + x) * 4];
				p[0] = (uint8_t)(x * 255 / W);
				p[1] = (uint8_t)(y * 255 / H);
				p[2] = ((x / 32 + y / 32) & 1) ? 160 : 80;
				p[3] = 255;
			}
		}
	}
	std::vector<int16_t> pcm;
	const auto start = std::chrono::steady_clock::now();
	const double cpuStart = ProcessCpuSeconds();
	double drawSeconds = 0;  // 絵を作るのにかかった時間（CPU の時間から引く）
	uint64_t frames = 0;
	for (;;) {
		const double now =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (now >= seconds) break;
		const bool audioGap = !bench && (now >= 4.0 && now < 6.0);  // 音を 2 秒止める
		const bool videoGap = late && (now >= 2.0 && now < 5.0);

		const uint64_t want = (uint64_t)(now * 48000);
		if (!audioGap && want > frames) {
			const int n = (int)(want - frames);
			pcm.resize((size_t)n * 2);
			for (int i = 0; i < n; i++) {
				const uint64_t k = frames + (uint64_t)i;
				const int16_t v = (k % 48000 < 4800)
				                      ? (int16_t)(8000 * std::sin(2 * 3.14159265 * 880 * k / 48000.0))
				                      : 0;
				pcm[(size_t)i * 2] = pcm[(size_t)i * 2 + 1] = v;
			}
			enc.SubmitAudio(&pcm[0], n);
			frames += (uint64_t)n;
		}
		if (audioGap) frames = want;  // 止めている間も呼ぶ側の時計は進む

		if (!videoGap && enc.WantVideoFrame()) {
			if (advanceAtSec >= 0 && now >= advanceAtSec) advanceMs = advanceAtMs;
			const int64_t t = enc.submittedAudioMs() - videoLagMs;
			const int bx = (int)(t / 10 % W);
			const auto drawStart = std::chrono::steady_clock::now();
			// --static: 左上の 1/3 x 1/4 だけに、音量の棒のように伸び縮みする
			// 縦棒を描く（ほとんど止まった絵）。
			const int drawW = staticImage ? W / 3 : W;
			const int drawH = staticImage ? H / 4 : H;
			for (int y = 0; y < drawH; y++) {
				for (int x = 0; x < drawW; x++) {
					uint8_t *p = &rgba[((size_t)y * W + x) * 4];
					if (staticImage) {
						const int bar = x / 16;
						const int level = (int)((t / 20 + bar * 37) % 97) * drawH / 97;
						const bool on = (x % 16) < 12 && drawH - y <= level;
						p[0] = on ? 255 : 0;
						p[1] = on ? (uint8_t)(y * 255 / drawH) : 0;
						p[2] = on ? 0 : 40;
						p[3] = 255;
						continue;
					}
					const bool band = (x >= bx && x < bx + 40);
					const bool flash = (t % 1000 < 100) && x > W - 150 && y < 150;
					p[0] = band ? 255 : (uint8_t)(x * 255 / W);
					p[1] = flash ? 255 : (uint8_t)(y * 255 / H);
					p[2] = band || flash ? 255 : 80;
					p[3] = 255;
				}
			}
			drawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStart)
			                   .count();
			enc.SubmitVideoRGBA(&rgba[0], W, H, W * 4, t - advanceMs);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	const double wall =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const double cpu = ProcessCpuSeconds() - cpuStart;
	enc.Stop();
	readThread.join();
	delete reader;
	fclose(out);
	const StreamStats s = enc.stats();
	printf("audio=%lldms video=%lldms silence=%lldms frames=%llu dropped=%llu enc=%.2fms\n",
	       (long long)s.audioMs, (long long)s.videoMs, (long long)s.silenceMs,
	       (unsigned long long)s.videoFrames, (unsigned long long)s.droppedFrames, s.encodeMsAvg);
	// 1 コアを 100% とした割合。絵を作る分（呼ぶ側の仕事）は引く。
	printf("size=%dx%d src=%dx%d fps=%d cpu=%.1f%% (draw %.1f%% excluded)\n", cfg.width, cfg.height,
	       W, H, cfg.fps, (cpu - drawSeconds) * 100.0 / wall, drawSeconds * 100.0 / wall);
	return 0;
}
