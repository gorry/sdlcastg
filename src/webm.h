// sdlcastg - ライブ向けの WebM（Matroska）の書き出し
//
// 流しっぱなしにするので、ふつうの WebM と違って:
//   - Segment の大きさは「不明」（0x01FFFFFFFFFFFFFF）
//   - 索引（Cues）も SeekHead も書かない（あとから戻って書き直せないため）
//   - Cluster は溜めてから大きさを確定して書く（受信側が読みやすいように）
// libwebm を使うほどのことではないので自前で持つ。
//
// 時刻の単位は ms（TimecodeScale = 1000000ns）。トラックは 1 = 映像（VP8）、
// 2 = 音声（Opus）の固定。

#ifndef SDLCASTG_WEBM_H
#define SDLCASTG_WEBM_H

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>

namespace sdlcastg {

class WebmWriter {
public:
	enum { kVideoTrack = 1, kAudioTrack = 2 };

	// 先頭（EBML の見出し・Segment の始まり・Info・Tracks）。受信側が
	// つないでくるたびに、まずこれを渡す。
	// opusHead は Opus の CodecPrivate（"OpusHead" で始まる 19 バイト）。
	static std::string Header(int width, int height, const std::string &opusHead,
	                          int opusPreSkip, int channels, int sampleRate);

	WebmWriter();

	// ブロックを足す。timeMs は減らないこと（減ったら前の時刻に寄せる）。
	// 映像のキーフレームが来たら、そこで Cluster を区切る（途中からつないだ
	// 受信側がキーフレームから読み始められるように）。長くなりすぎても区切る。
	void AddBlock(int track, int64_t timeMs, bool keyframe, const void *data, size_t size);

	// 区切った Cluster を 1 つ取り出す。無ければ false。
	// *blocks は Cluster の中のブロックだけ（Cluster の見出しと Timecode は含まない）。
	// **受信側ごとに時刻を 0 から振り直す**ため、見出しは配るときに ClusterBytes で
	// 付ける（ブロックの時刻は Cluster からの相対なので、そのまま使える）。
	// *startsWithKeyframe は、その Cluster の最初の映像がキーフレームか。
	bool TakeCluster(std::string *blocks, int64_t *timeMs, bool *startsWithKeyframe);

	// Cluster の見出し（大きさと Timecode）を付けて、線に載せる形にする。
	static std::string ClusterBytes(int64_t timecodeMs, const std::string &blocks);

	// 溜めている途中の Cluster を区切る（終わるとき用）。
	void Flush();

	// Cluster を区切る長さ (ms)。短いほど遅れは減るが、受信側の手間が増える。
	static const int64_t kMaxClusterMs = 500;

	// 流す量の下限（バイト/秒）。0 なら詰めない。足りないぶんは Cluster の末尾に
	// Void（読み飛ばされる詰め物）を足す。
	void SetMinBytesPerSec(int64_t v) { minBytesPerSec_ = v; }

private:
	void CloseCluster();

	std::string body_;       // 今の Cluster のブロック
	int64_t clusterTime_;    // -1 なら Cluster が始まっていない
	bool clusterKey_;
	int64_t lastTime_;
	int64_t minBytesPerSec_;

	struct Ready {
		std::string data;
		int64_t timeMs;
		bool key;
	};
	std::deque<Ready> ready_;  // 区切り終えた Cluster
};

}  // namespace sdlcastg

#endif  // SDLCASTG_WEBM_H
