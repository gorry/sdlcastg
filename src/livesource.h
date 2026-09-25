// sdlcastg - ライブの WebM を HTTP で配る中身
//
// エンコーダーが Cluster を 1 つずつ足していき、受信側（HTTP の接続）は
// 「先頭（Header）→ キーフレームで始まる Cluster → 以後の Cluster」の順に読む。
//
// **受信側がつないできたら、エンコーダーにキーフレームを頼み、つないだあとに
// できたキーフレームから渡す。** 溜めてある古いキーフレームから渡すと、その
// ぶん最初から遅れる（2026-09-25 に Chromecast 内蔵の TV で測った: 直近のキーフレームから
// 渡したら 2 秒古いところから始まり、遅れが 3.8 秒 → 7.6 秒になった）。
// 頼んだキーフレームが kJoinWaitMs のうちに来なければ、溜めてある直近のもので妥協する。
//
// **時刻は受信側ごとに 0 から振り直す**（最初に渡す Cluster の時刻を引く）。
// 途中の時刻から始まる流れを渡すと、受信側（TV の Cast 受信部）は 0 から再生を
// 始めてから最初の Cluster の時刻へ飛び、そこで溜め直して遅れが 8 秒に
// 延びた（2026-09-25）。
//
// 溜めておくのは直近 kKeepMs ぶんだけ。読むのが遅れて取りこぼしそうになった
// 接続は、最新のキーフレームまで飛ばす（受信側からは一瞬の飛びに見える）。

#ifndef SDLCASTG_LIVESOURCE_H
#define SDLCASTG_LIVESOURCE_H

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <utility>
#include <memory>
#include <mutex>
#include <string>

#include "httpserver.h"

namespace sdlcastg {

class LiveSource : public HttpSource {
public:
	explicit LiveSource(const std::string &header);

	std::string contentType() const { return "video/webm"; }
	int64_t size() const { return -1; }
	HttpStream *Open(int64_t offset);

	// エンコーダーから。blocks は Cluster の中のブロック（WebmWriter::TakeCluster）。
	void AddCluster(const std::string &blocks, int64_t timeMs, bool keyframe);

	// 配るのをやめる（読んでいる接続は終わりを受け取る）。
	void Close();

	// 受信側がつないできて、キーフレームを待っているか。エンコーダーが見て、
	// 真なら次の映像をキーフレームにする（読むと下りる）。
	bool TakeKeyRequest() { return keyRequested_.exchange(false); }

	// 見本・状態の表示用。
	int clients() const;
	int64_t bytesSent() const;
	// つないでいる読み手のうち一番古いもの（ふつうは受信側）にとっての時刻 0 が、
	// 流れの上のどこか (ms)。受信側の再生位置（currentTime）にこれを足すと、
	// 流れの時刻になる。誰もいなければ最後に分かっていた値。
	// （最後につないだ読み手のものにすると、ほかの読み手が途中でつなぐだけで
	// 受信側の遅れの計算が狂った。）
	int64_t clientBaseMs() const;

private:
	friend class LiveStream;

	struct Cluster {
		uint64_t seq;
		int64_t timeMs;
		bool key;
		std::shared_ptr<const std::string> data;
	};

	static const int64_t kKeepMs = 10000;
	static const int64_t kJoinWaitMs = 3000;

	// seq 以降で最初に読むべき Cluster を探す（鍵を持って呼ぶ）。
	bool FindLocked(uint64_t seq, Cluster *out);
	uint64_t LatestKeySeqLocked() const;

	const std::string header_;
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Cluster> clusters_;
	uint64_t nextSeq_;
	bool closed_;
	int clients_;
	int64_t bytesSent_;
	int64_t lastClientBaseMs_;
	// 時刻 0 が決まった読み手（つないだ順）。読み手が消えたら外す。
	std::deque<std::pair<const void *, int64_t> > clientBases_;
	std::atomic<bool> keyRequested_;
};

}  // namespace sdlcastg

#endif  // SDLCASTG_LIVESOURCE_H
