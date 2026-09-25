// sdlcastg - 映像と音声をエンコードして、ライブの WebM にする
//
// 映像は VP8（libvpx の実時間モード）、音声は Opus（48kHz ステレオ）。
// 出来た Cluster は LiveSource へ渡し、HTTP で受信側へ流れる。
//
// **時計は音声。** 渡された音声の長さが、そのまま流れの時刻になる
// （0 から始まる ms）。映像のフレームには同じ時計の上の時刻（ptsMs）を付けて
// もらい、音声がその時刻まで進んだところで流れに差し込む。こうすると
// 画面と音がずれない。
//
// 流れを途切れさせないための手当て（受信側は溜めたぶんを使い切ると止まる）:
//   - 音声が来なくなったら（呼ぶ側が止まった・バックグラウンド）、実時間に
//     合わせて無音を足す
//   - 映像が来なくなったら、最後の絵を 1 秒ごとに出し直す
//
// 入力はどのスレッドからでもよい。エンコードは専用のスレッドで行う。

#ifndef SDLCASTG_ENCODER_H
#define SDLCASTG_ENCODER_H

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "livesource.h"
#include "webm.h"

namespace sdlcastg {

struct StreamConfig {
	int width;       // 送る映像の大きさ（偶数に丸める）
	int height;
	int fps;
	int videoKbps;
	int audioKbps;
	// 流す量の下限。動きの少ない絵はエンコードすると小さくなりすぎ、受信側が
	// 一定のバイト数を溜めてから読む作りのため、読むたびに再生が詰まる
	// （docs/design.md）。足りないぶんは WebM の Void で詰める。0 なら詰めない。
	int minKbps;
	// VP8 のエンコードのスレッド数。0 なら決め打ちの既定（encoder.cpp）。
	int threads;

	StreamConfig()
	    : width(1280), height(720), fps(30), videoKbps(2000), audioKbps(128), minKbps(1000),
	      threads(0) {}
};

struct StreamStats {
	int64_t audioMs;         // 流れに入れた音声の長さ
	int64_t videoMs;         // 最後に入れた映像の時刻
	int64_t silenceMs;       // 足した無音の長さ
	uint64_t videoFrames;    // エンコードした映像の数（出し直しを含む）
	uint64_t droppedFrames;  // 間に合わず捨てた映像の数
	uint64_t sameTimeFrames; // 前の絵と同じ時刻（枠）だったので捨てた映像の数
	double encodeMsAvg;      // 映像 1 枚のエンコードにかかった時間（変換込み）
	int clients;             // つないでいる受信側の数
	int64_t bytesSent;
	int64_t clientBaseMs;    // 最後につないだ受信側の時刻 0 が、流れの上のどこか

	StreamStats()
	    : audioMs(0), videoMs(0), silenceMs(0), videoFrames(0), droppedFrames(0), sameTimeFrames(0),
	      encodeMsAvg(0.0),
	      clients(0), bytesSent(0), clientBaseMs(0) {}
};

class StreamEncoder {
public:
	// Opus に渡す形（固定）。
	static const int kSampleRate = 48000;
	static const int kChannels = 2;

	StreamEncoder();
	~StreamEncoder();

	bool Start(const StreamConfig &cfg, std::string *err);
	void Stop();
	bool running() const { return running_.load(); }
	// Start で渡された設定（流している間だけ意味がある）。
	const StreamConfig &config() const { return cfg_; }

	std::shared_ptr<LiveSource> source() const { return source_; }

	// 48kHz ステレオの int16（左右交互）。
	void SubmitAudio(const int16_t *pcm, int frames);

	// RGBA（メモリ上で R, G, B, A の順）。大きさは何でもよく、縦横比を保って
	// 送る大きさへ縮める（余りは黒）。ptsMs が負なら「いま」（渡された音声の末尾）。
	void SubmitVideoRGBA(const void *pixels, int width, int height, int pitch, int64_t ptsMs);

	// 次の映像を受け取る頃合いか（fps で間引く）。画面の読み出しは重いので、
	// 呼ぶ側はこれが偽なら読み出さなくてよい。
	bool WantVideoFrame() const;

	// これまでに渡された音声の長さ (ms)。映像の時刻を決めるのに使える。
	int64_t submittedAudioMs() const;

	StreamStats stats() const;

private:
	struct Frame {
		std::vector<uint8_t> rgba;
		int width, height, pitch;
		int64_t ptsMs;
	};
	struct Codecs;

	void Run();
	void EncodeAudioChunk(const int16_t *pcm);
	void EncodeVideo(const Frame *f, int64_t ptsMs, bool forceKey);
	void ConvertToI420(const Frame &f);
	int64_t SlotOf(int64_t ms) const;
	// エンコードした塊を時刻順に並べてから WebM へ書く（下の out*_）。
	void QueuePacket(int track, int64_t timeMs, bool key, const void *data, size_t size);
	void WriteOrdered(int64_t limitMs);
	int64_t StreamNowMsLocked() const;
	int64_t SlotMs(int64_t slot) const;
	void FlushClusters();

	StreamConfig cfg_;
	Codecs *codecs_;
	std::shared_ptr<LiveSource> source_;
	WebmWriter writer_;

	std::thread thread_;
	std::atomic<bool> running_;
	std::atomic<bool> quit_;

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::vector<int16_t> pendingAudio_;   // まだエンコードしていない音声
	uint64_t submittedFrames_;            // 渡された音声のフレーム数（無音の足し分を含む）
	int64_t lastSubmitWallMs_;            // 最後に音声を渡された実時間
	std::deque<std::unique_ptr<Frame> > frames_;  // 差し込み待ちの映像
	int64_t lastWantSlot_;                // 最後に映像を受け取った枠（下の SlotOf）

	// エンコードのスレッドだけが触る
	uint64_t encodedFrames_;              // エンコードした音声のフレーム数
	int64_t startWallMs_;
	int64_t lastVideoMs_;
	int64_t lastVideoSlot_;               // 最後に流れへ入れた映像の枠
	bool haveLastImage_;
	// 書く前の塊（エンコードのスレッドだけ）。映像は「その時刻の音」より後から
	// 来る（呼ぶ側は、いま聞こえている音の時刻の絵を渡すので、渡し終えた音より
	// 装置のバッファぶん古い）。WebM は時刻順に書かないといけないので、音を
	// kReorderMs だけ溜め、両方を時刻順に混ぜて書く（docs/design.md）。
	struct Packet {
		int track;
		int64_t timeMs;
		bool key;
		std::string data;
	};
	std::deque<Packet> outAudio_;
	std::deque<Packet> outVideo_;
	std::vector<uint8_t> i420_;
	std::vector<uint8_t> scaled_;

	StreamStats stats_;  // mutex_ で守る
};

}  // namespace sdlcastg

#endif  // SDLCASTG_ENCODER_H
