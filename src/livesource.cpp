// sdlcastg - ライブの WebM を HTTP で配る中身

#include "livesource.h"

#include <string.h>

#include <chrono>

#include "webm.h"

namespace sdlcastg {

// クラスの中で値を与えた定数の定義（C++11 では、参照で渡すと定義が要る。
// std::chrono::milliseconds に渡しているところがそれで、最適化しないと
// リンクで見つからなくなる）。
const int64_t LiveSource::kKeepMs;
const int64_t LiveSource::kJoinWaitMs;

// 1 つの HTTP 応答ぶんの読み手。
class LiveStream : public HttpStream {
public:
	explicit LiveStream(LiveSource *src)
	    : src_(src), headerSent_(0), started_(false), seq_(0), offset_(0), baseMs_(-1), joinSeq_(0),
	      joinTime_(std::chrono::steady_clock::now()) {
		std::lock_guard<std::mutex> lock(src_->mutex_);
		src_->clients_++;
		// ここから先にできるキーフレームを待つ。エンコーダーに頼んでおく。
		joinSeq_ = src_->nextSeq_;
		src_->keyRequested_ = true;
	}
	~LiveStream() {
		std::lock_guard<std::mutex> lock(src_->mutex_);
		src_->clients_--;
		for (size_t i = 0; i < src_->clientBases_.size(); i++) {
			if (src_->clientBases_[i].first == this) {
				src_->clientBases_.erase(src_->clientBases_.begin() + (long)i);
				break;
			}
		}
	}

	int Read(void *buf, int size) {
		// 先頭（Header）から。
		if (headerSent_ < src_->header_.size()) {
			size_t n = src_->header_.size() - headerSent_;
			if (n > (size_t)size) n = (size_t)size;
			memcpy(buf, src_->header_.data() + headerSent_, n);
			headerSent_ += n;
			Count((int64_t)n);
			return (int)n;
		}

		// 読みかけの Cluster があればその続き。
		if (offset_ < framed_.size()) return Copy(buf, size);

		std::unique_lock<std::mutex> lock(src_->mutex_);
		for (;;) {
			if (!started_) {
				// つないだあとにできたキーフレームの Cluster から。待ちきれなければ
				// 溜めてある直近のキーフレームから。
				const uint64_t key = src_->LatestKeySeqLocked();
				const bool waited =
				    std::chrono::steady_clock::now() - joinTime_ >
				    std::chrono::milliseconds(LiveSource::kJoinWaitMs);
				if (key != UINT64_MAX && (key >= joinSeq_ || waited)) {
					seq_ = key;
					started_ = true;
				}
			}
			LiveSource::Cluster c;
			if (started_ && src_->FindLocked(seq_, &c)) {
				if (c.seq != seq_) {
					// 読むのが遅れて、読むはずのものが捨てられた。最新の
					// キーフレームまで飛ばす。
					const uint64_t key = src_->LatestKeySeqLocked();
					if (key != UINT64_MAX && src_->FindLocked(key, &c)) seq_ = key;
				}
				seq_ = c.seq + 1;
				if (baseMs_ < 0) {
					baseMs_ = c.timeMs;
					src_->lastClientBaseMs_ = baseMs_;
					src_->clientBases_.push_back(std::make_pair((const void *)this, baseMs_));
				}
				cur_ = c.data;
				offset_ = 0;
				lock.unlock();
				// 見出しを付ける（この受信側にとっての時刻で）。
				framed_ = WebmWriter::ClusterBytes(c.timeMs - baseMs_, *cur_);
				return Copy(buf, size);
			}
			// 閉じられたら、残りを読み切ってから終わる。
			if (src_->closed_) return 0;
			src_->cv_.wait_for(lock, std::chrono::milliseconds(100));
		}
	}

private:
	int Copy(void *buf, int size) {
		size_t n = framed_.size() - offset_;
		if (n > (size_t)size) n = (size_t)size;
		memcpy(buf, framed_.data() + offset_, n);
		offset_ += n;
		Count((int64_t)n);
		return (int)n;
	}

	void Count(int64_t n) {
		std::lock_guard<std::mutex> lock(src_->mutex_);
		src_->bytesSent_ += n;
	}

	LiveSource *src_;
	size_t headerSent_;
	bool started_;
	uint64_t seq_;
	std::shared_ptr<const std::string> cur_;
	std::string framed_;  // cur_ に見出しを付けたもの
	size_t offset_;
	int64_t baseMs_;      // この受信側にとっての時刻 0（流れの上の時刻）
	uint64_t joinSeq_;
	std::chrono::steady_clock::time_point joinTime_;
};

LiveSource::LiveSource(const std::string &header)
    : header_(header), nextSeq_(0), closed_(false), clients_(0), bytesSent_(0),
      lastClientBaseMs_(0), keyRequested_(false) {}

HttpStream *LiveSource::Open(int64_t offset) {
	(void)offset;  // ライブなので Range には応えない
	return new LiveStream(this);
}

int64_t LiveSource::clientBaseMs() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return clientBases_.empty() ? lastClientBaseMs_ : clientBases_.front().second;
}

void LiveSource::AddCluster(const std::string &data, int64_t timeMs, bool keyframe) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Cluster c;
		c.seq = nextSeq_++;
		c.timeMs = timeMs;
		c.key = keyframe;
		c.data = std::make_shared<const std::string>(data);
		clusters_.push_back(c);
		// 古いものを捨てる。ただしキーフレームで始まるものを 1 つは残す。
		while (clusters_.size() > 1 && timeMs - clusters_.front().timeMs > kKeepMs) {
			bool laterKey = false;
			for (size_t i = 1; i < clusters_.size(); i++) {
				if (clusters_[i].key) {
					laterKey = true;
					break;
				}
			}
			if (!laterKey) break;
			clusters_.pop_front();
		}
	}
	cv_.notify_all();
}

void LiveSource::Close() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		closed_ = true;
	}
	cv_.notify_all();
}

int LiveSource::clients() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return clients_;
}

int64_t LiveSource::bytesSent() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return bytesSent_;
}

bool LiveSource::FindLocked(uint64_t seq, Cluster *out) {
	for (size_t i = 0; i < clusters_.size(); i++) {
		if (clusters_[i].seq >= seq) {
			*out = clusters_[i];
			return true;
		}
	}
	return false;
}

uint64_t LiveSource::LatestKeySeqLocked() const {
	for (size_t i = clusters_.size(); i > 0; i--) {
		if (clusters_[i - 1].key) return clusters_[i - 1].seq;
	}
	return UINT64_MAX;
}

}  // namespace sdlcastg
