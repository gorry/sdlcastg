// sdlcastg - 映像と音声をエンコードして、ライブの WebM にする

#include "encoder.h"

#include <string.h>

#include <algorithm>
#include <chrono>
#include <limits>

#include "libyuv/convert.h"
#include "libyuv/scale_argb.h"
#include "opus.h"
#include "vpx/vp8cx.h"
#include "vpx/vpx_encoder.h"

namespace sdlcastg {

// クラスの中で値を与えた定数の定義（livesource.cpp と同じ理由）。
const int StreamEncoder::kSampleRate;
const int StreamEncoder::kChannels;

namespace {

// Opus の 1 回ぶん（20ms）。
const int kOpusFrame = 960;
// 音声がこれだけ来なかったら、無音で埋め始める (ms)。
const int64_t kStarveMs = 200;
// 映像がこれだけ来なかったら、最後の絵を出し直す (ms)。
const int64_t kVideoRepeatMs = 1000;
// 差し込み待ちの映像の上限。溢れたら古いものから捨てる。音が途切れて
// 音と映像を時刻順に混ぜて書くために、音を溜めておく長さ。映像は音より遅れて
// 来る（装置のバッファ＋呼ぶ側の「早める」補正＋読み出しの間隔。Android で
// 43 + 200 + 33ms ほど）。そのぶん受信側の遅れが延びる。
const int64_t kReorderMs = 300;
// 無音で埋め始めるまで（kStarveMs）の間も映像は来るので、そのぶんは持てること。
const size_t kMaxQueuedFrames = 10;
// 呼ぶ側の時刻がこれ以上先を指していたら、「いま」とみなす (ms)。
const int64_t kMaxVideoLeadMs = 1000;

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

int64_t FramesToMs(uint64_t frames) {
	return (int64_t)(frames * 1000 / StreamEncoder::kSampleRate);
}

// Opus の CodecPrivate（RFC 7845 の "OpusHead"）。
std::string OpusHead(int channels, int preSkip, int inputRate) {
	std::string h("OpusHead", 8);
	h.push_back(1);  // version
	h.push_back((char)channels);
	h.push_back((char)(preSkip & 0xff));
	h.push_back((char)((preSkip >> 8) & 0xff));
	for (int i = 0; i < 4; i++) h.push_back((char)((inputRate >> (8 * i)) & 0xff));
	h.push_back(0);  // output gain
	h.push_back(0);
	h.push_back(0);  // channel mapping family
	return h;
}

}  // namespace

struct StreamEncoder::Codecs {
	vpx_codec_ctx_t vpx;
	bool vpxOpen;
	OpusEncoder *opus;

	Codecs() : vpxOpen(false), opus(0) { memset(&vpx, 0, sizeof(vpx)); }
	~Codecs() {
		if (vpxOpen) vpx_codec_destroy(&vpx);
		if (opus != 0) opus_encoder_destroy(opus);
	}
};

StreamEncoder::StreamEncoder()
    : codecs_(0), running_(false), quit_(false), submittedFrames_(0), lastSubmitWallMs_(0),
      lastWantSlot_(-1), encodedFrames_(0), startWallMs_(0), lastVideoMs_(-1), lastVideoSlot_(-1),
      haveLastImage_(false) {}

StreamEncoder::~StreamEncoder() {
	Stop();
}

bool StreamEncoder::Start(const StreamConfig &cfgIn, std::string *err) {
	Stop();
	cfg_ = cfgIn;
	cfg_.width = std::max(16, cfg_.width & ~1);
	cfg_.height = std::max(16, cfg_.height & ~1);
	cfg_.fps = std::min(60, std::max(1, cfg_.fps));
	cfg_.videoKbps = std::max(100, cfg_.videoKbps);
	cfg_.audioKbps = std::min(510, std::max(32, cfg_.audioKbps));

	Codecs *c = new Codecs;

	// VP8。遅れを増やさないよう先読み無し（lag 0）、実時間モード、CBR。
	vpx_codec_enc_cfg_t vc;
	if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &vc, 0) != VPX_CODEC_OK) {
		if (err) *err = "vpx_codec_enc_config_default failed";
		delete c;
		return false;
	}
	vc.g_w = (unsigned)cfg_.width;
	vc.g_h = (unsigned)cfg_.height;
	vc.g_timebase.num = 1;
	vc.g_timebase.den = 1000;  // ms
	vc.g_lag_in_frames = 0;
	// 既定は 1 本。VP8 は複数のスレッドで行を分けると、行のそろい待ちを
	// 回りながら待つ（Pixel 7a の 720p で、2 本にすると CPU のサイクルが 1.5 倍、
	// 4 本で 2 倍以上になり、1 枚の時間は縮まなかった。docs/design.md）。
	vc.g_threads = cfg_.threads > 0 ? (unsigned)cfg_.threads : 1;
	vc.rc_end_usage = VPX_CBR;
	vc.rc_target_bitrate = (unsigned)cfg_.videoKbps;
	vc.rc_min_quantizer = 4;
	vc.rc_max_quantizer = 56;
	vc.rc_buf_sz = 1000;
	vc.rc_buf_initial_sz = 500;
	vc.rc_buf_optimal_sz = 600;
	vc.kf_mode = VPX_KF_AUTO;
	vc.kf_min_dist = 0;
	vc.kf_max_dist = (unsigned)(cfg_.fps * 2);  // 2 秒ごとにキーフレーム
	if (vpx_codec_enc_init(&c->vpx, vpx_codec_vp8_cx(), &vc, 0) != VPX_CODEC_OK) {
		if (err) *err = std::string("vpx_codec_enc_init failed: ") + vpx_codec_error(&c->vpx);
		delete c;
		return false;
	}
	c->vpxOpen = true;
	// 速さ 8（実時間向けの設定の中ほど）。16 にしても Pixel 7a で 1 割しか
	// 軽くならず、画質が落ちるので 8 のまま。動かないところを飛ばす
	// VP8E_SET_STATIC_THRESHOLD と VP8E_SET_SCREEN_CONTENT_MODE も試したが、
	// 軽くならなかった（docs/design.md）。
	vpx_codec_control(&c->vpx, VP8E_SET_CPUUSED, 8);
	vpx_codec_control(&c->vpx, VP8E_SET_NOISE_SENSITIVITY, 0);

	// Opus。
	int e = 0;
	c->opus = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &e);
	if (c->opus == 0 || e != OPUS_OK) {
		if (err) *err = std::string("opus_encoder_create failed: ") + opus_strerror(e);
		delete c;
		return false;
	}
	opus_encoder_ctl(c->opus, OPUS_SET_BITRATE(cfg_.audioKbps * 1000));
	opus_int32 lookahead = 0;
	opus_encoder_ctl(c->opus, OPUS_GET_LOOKAHEAD(&lookahead));

	codecs_ = c;
	writer_ = WebmWriter();
	writer_.SetMinBytesPerSec((int64_t)std::max(0, cfg_.minKbps) * 1000 / 8);
	source_ = std::make_shared<LiveSource>(WebmWriter::Header(
	    cfg_.width, cfg_.height, OpusHead(kChannels, (int)lookahead, kSampleRate), (int)lookahead,
	    kChannels, kSampleRate));

	pendingAudio_.clear();
	frames_.clear();
	outAudio_.clear();
	outVideo_.clear();
	submittedFrames_ = 0;
	encodedFrames_ = 0;
	lastVideoMs_ = -1;  // まだ 1 枚も入れていない
	lastVideoSlot_ = -1;
	haveLastImage_ = false;
	stats_ = StreamStats();
	startWallMs_ = NowMs();
	lastSubmitWallMs_ = 0;
	lastWantSlot_ = -1;
	i420_.assign((size_t)cfg_.width * cfg_.height * 3 / 2, 0);

	quit_ = false;
	running_ = true;
	thread_ = std::thread(&StreamEncoder::Run, this);
	return true;
}

void StreamEncoder::Stop() {
	if (thread_.joinable()) {
		quit_ = true;
		cv_.notify_all();
		thread_.join();
		WriteOrdered(std::numeric_limits<int64_t>::max());
		writer_.Flush();
		FlushClusters();
	}
	if (source_) source_->Close();
	delete codecs_;
	codecs_ = 0;
	running_ = false;
}

void StreamEncoder::SubmitAudio(const int16_t *pcm, int frames) {
	if (frames <= 0 || !running_) return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pendingAudio_.insert(pendingAudio_.end(), pcm, pcm + (size_t)frames * kChannels);
		submittedFrames_ += (uint64_t)frames;
		lastSubmitWallMs_ = NowMs();
	}
	cv_.notify_all();
}

// 映像の時刻は fps の格子（枠）に載せる。枠 n の時刻は n * 1000 / fps (ms)。
// 渡された時刻のままだと、呼ぶ側の周期の揺れがそのまま入り（30fps で 20〜47ms 間隔）、
// 受信側（TV）で映像が音から少しずつ遅れては合わせ直す動きになった（docs/design.md）。
int64_t StreamEncoder::SlotOf(int64_t ms) const {
	return ms * cfg_.fps / 1000;
}

int64_t StreamEncoder::SlotMs(int64_t slot) const {
	return slot * 1000 / cfg_.fps;
}

// いまの流れの時刻の見積もり（鍵を持って呼ぶ）。渡された音声の長さに、最後に
// 渡されてからの経過を足す（kStarveMs まで）。音声はまとめて来る（Android の
// AAudio は 2048 フレーム＝43ms ずつ）ので、長さだけで枠を数えると、映像を
// 受け取れるのが音声の来た直後だけになり、30fps に届かなかった。
int64_t StreamEncoder::StreamNowMsLocked() const {
	const int64_t audioMs = FramesToMs(submittedFrames_);
	if (lastSubmitWallMs_ == 0) return audioMs;
	const int64_t since = NowMs() - lastSubmitWallMs_;
	return audioMs + std::max<int64_t>(0, std::min<int64_t>(since, kStarveMs));
}

// 流れの時計が次の枠に入ったら受け取る。
bool StreamEncoder::WantVideoFrame() const {
	if (!running_) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	return SlotOf(StreamNowMsLocked()) > lastWantSlot_;
}

int64_t StreamEncoder::submittedAudioMs() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return FramesToMs(submittedFrames_);
}

void StreamEncoder::SubmitVideoRGBA(const void *pixels, int width, int height, int pitch,
                                    int64_t ptsMs) {
	if (!running_ || pixels == 0 || width <= 0 || height <= 0) return;
	std::unique_ptr<Frame> f(new Frame);
	f->width = width;
	f->height = height;
	f->pitch = width * 4;
	f->rgba.resize((size_t)f->pitch * height);
	for (int y = 0; y < height; y++) {
		memcpy(&f->rgba[(size_t)y * f->pitch], (const uint8_t *)pixels + (size_t)y * pitch,
		       (size_t)f->pitch);
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const int64_t audioNow = FramesToMs(submittedFrames_);
	lastWantSlot_ = std::max(lastWantSlot_, SlotOf(StreamNowMsLocked()));

	if (ptsMs < 0 || ptsMs > audioNow + kMaxVideoLeadMs) ptsMs = audioNow;
	if (!frames_.empty() && ptsMs < frames_.back()->ptsMs) ptsMs = frames_.back()->ptsMs;
	f->ptsMs = ptsMs;
	if (frames_.size() >= kMaxQueuedFrames) {
		frames_.pop_front();
		stats_.droppedFrames++;
	}
	frames_.push_back(std::move(f));
}

StreamStats StreamEncoder::stats() const {
	StreamStats s;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		s = stats_;
	}
	if (source_) {
		s.clients = source_->clients();
		s.bytesSent = source_->bytesSent();
		s.clientBaseMs = source_->clientBaseMs();
	}
	return s;
}

void StreamEncoder::Run() {
	// 最初に黒い絵を 1 枚入れておく（映像のトラックが空のままだと、受信側が
	// 映像を待って再生を始めない）。
	{
		const size_t ySize = (size_t)cfg_.width * cfg_.height;
		memset(&i420_[0], 16, ySize);
		memset(&i420_[ySize], 128, ySize / 2);
		haveLastImage_ = true;
		EncodeVideo(0, 0, true);
	}

	std::vector<int16_t> chunk((size_t)kOpusFrame * kChannels);
	while (!quit_) {
		bool haveChunk = false;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
				return quit_.load() || pendingAudio_.size() >= (size_t)kOpusFrame * kChannels;
			});
			if (quit_) break;

			// 音声が途絶えていたら、実時間に追いつくまで無音を足す。
			const int64_t now = NowMs();
			const uint64_t expected = (uint64_t)(now - startWallMs_) * kSampleRate / 1000;
			if (now - lastSubmitWallMs_ > kStarveMs && submittedFrames_ < expected) {
				const uint64_t add = expected - submittedFrames_;
				pendingAudio_.resize(pendingAudio_.size() + (size_t)add * kChannels, 0);
				submittedFrames_ += add;
				stats_.silenceMs += FramesToMs(add);
			}

			if (pendingAudio_.size() >= chunk.size()) {
				std::copy(pendingAudio_.begin(), pendingAudio_.begin() + (long)chunk.size(),
				          chunk.begin());
				pendingAudio_.erase(pendingAudio_.begin(), pendingAudio_.begin() + (long)chunk.size());
				haveChunk = true;
			}
		}
		if (!haveChunk) continue;

		const int64_t audioMs = FramesToMs(encodedFrames_);
		// 受信側がつないできたら、次の映像をキーフレームにする（LiveSource）。
		bool forceKey = source_->TakeKeyRequest();

		// この音声より前の時刻の映像を、先に流れへ入れる。
		for (;;) {
			std::unique_ptr<Frame> f;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (frames_.empty() || frames_.front()->ptsMs > audioMs) break;
				f = std::move(frames_.front());
				frames_.pop_front();
			}
			EncodeVideo(f.get(), f->ptsMs, forceKey);
			forceKey = false;
		}
		// 映像が来ていなければ、最後の絵を出し直す（キーフレームを頼まれて
		// いればすぐに）。
		if (haveLastImage_ && (forceKey || audioMs - lastVideoMs_ >= kVideoRepeatMs)) {
			EncodeVideo(0, audioMs, forceKey);
		}

		EncodeAudioChunk(&chunk[0]);
		WriteOrdered(FramesToMs(encodedFrames_) - kReorderMs);
		FlushClusters();
	}
}

void StreamEncoder::EncodeAudioChunk(const int16_t *pcm) {
	unsigned char out[4000];
	const int n = opus_encode(codecs_->opus, pcm, kOpusFrame, out, sizeof(out));
	const int64_t t = FramesToMs(encodedFrames_);
	encodedFrames_ += kOpusFrame;
	if (n > 0) QueuePacket(WebmWriter::kAudioTrack, t, true, out, (size_t)n);
	std::lock_guard<std::mutex> lock(mutex_);
	stats_.audioMs = FramesToMs(encodedFrames_);
}

void StreamEncoder::EncodeVideo(const Frame *f, int64_t ptsMs, bool forceKey) {
	const int64_t t0 = NowMs();
	// 映像の時刻は一番近い枠に載せる。同じ時刻が 2 枚あると受信側が扱いに困る。
	// **渡された絵の枠がもう埋まっていたら、その絵は捨てる。** 次の枠へ送ると、
	// 後ろの絵も押し出されて映像の時刻が実際より先へずれ続け、TV で映像が
	// 音からどんどん遅れていった（絵の時刻が音声のコールバックごとにしか
	// 進まないアプリでは、30fps で読むと同じ時刻の絵が続く。docs/design.md）。
	// キーフレームを頼まれているときと、最後の絵の出し直し（f が NULL）だけは
	// 次の枠へ送る。
	if (ptsMs < 0) ptsMs = 0;
	int64_t slot = (ptsMs * cfg_.fps + 500) / 1000;
	if (slot <= lastVideoSlot_) {
		if (f != 0 && !forceKey) {
			std::lock_guard<std::mutex> lock(mutex_);
			stats_.sameTimeFrames++;
			return;
		}
		slot = lastVideoSlot_ + 1;
	}
	lastVideoSlot_ = slot;
	ptsMs = SlotMs(slot);
	if (f != 0) {
		ConvertToI420(*f);
		haveLastImage_ = true;
	}

	vpx_image_t img;
	vpx_img_wrap(&img, VPX_IMG_FMT_I420, (unsigned)cfg_.width, (unsigned)cfg_.height, 1, &i420_[0]);
	const vpx_enc_frame_flags_t flags = forceKey ? VPX_EFLAG_FORCE_KF : 0;
	if (vpx_codec_encode(&codecs_->vpx, &img, (vpx_codec_pts_t)ptsMs,
	                     (unsigned long)(1000 / cfg_.fps), flags, VPX_DL_REALTIME) != VPX_CODEC_OK) {
		return;
	}
	vpx_codec_iter_t iter = 0;
	const vpx_codec_cx_pkt_t *pkt;
	while ((pkt = vpx_codec_get_cx_data(&codecs_->vpx, &iter)) != 0) {
		if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
		const bool key = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
		QueuePacket(WebmWriter::kVideoTrack, ptsMs, key, pkt->data.frame.buf,
		            pkt->data.frame.sz);
	}
	lastVideoMs_ = ptsMs;

	const double ms = (double)(NowMs() - t0);
	std::lock_guard<std::mutex> lock(mutex_);
	stats_.videoFrames++;
	stats_.videoMs = ptsMs;
	stats_.encodeMsAvg = (stats_.videoFrames == 1) ? ms : stats_.encodeMsAvg * 0.95 + ms * 0.05;
}

// RGBA を送る大きさの I420 にする。縦横比を保って収め、余りは黒。
// 大きさを変えるのと色の変換は libyuv（NEON / SSE を使う）。
void StreamEncoder::ConvertToI420(const Frame &f) {
	const int dw = cfg_.width, dh = cfg_.height;
	const double scale = std::min((double)dw / f.width, (double)dh / f.height);
	int fw = std::min(dw, std::max(2, ((int)(f.width * scale + 0.5)) & ~1));
	int fh = std::min(dh, std::max(2, ((int)(f.height * scale + 0.5)) & ~1));
	const int ox = ((dw - fw) / 2) & ~1;
	const int oy = ((dh - fh) / 2) & ~1;

	uint8_t *yp = &i420_[0];
	uint8_t *up = yp + (size_t)dw * dh;
	uint8_t *vp = up + (size_t)(dw / 2) * (dh / 2);
	if (fw != dw || fh != dh) {
		memset(yp, 16, (size_t)dw * dh);
		memset(up, 128, (size_t)(dw / 2) * (dh / 2) * 2);
	}

	// 収める大きさへ縮める（同じ大きさならそのまま読む）。libyuv の ARGB の
	// 関数は 4 バイトの画素をまとめて扱うだけなので、並びが RGBA でもよい。
	// 縮めるときは面積の平均（kFilterBox）。大きく縮めても文字がちらつきにくい。
	const uint8_t *src = &f.rgba[0];
	int stride = f.pitch;
	if (fw != f.width || fh != f.height) {
		scaled_.resize((size_t)fw * fh * 4);
		libyuv::ARGBScale(&f.rgba[0], f.pitch, f.width, f.height, &scaled_[0], fw * 4, fw, fh,
		                  libyuv::kFilterBox);
		src = &scaled_[0];
		stride = fw * 4;
	}

	// BT.601 の限定範囲（受信側の既定）。libyuv の "ABGR" はメモリ上で
	// R, G, B, A の順（リトルエンディアンの 32 ビットで ABGR）。
	libyuv::ABGRToI420(src, stride, yp + (size_t)oy * dw + ox, dw,
	                   up + (size_t)(oy / 2) * (dw / 2) + ox / 2, dw / 2,
	                   vp + (size_t)(oy / 2) * (dw / 2) + ox / 2, dw / 2, fw, fh);
}

void StreamEncoder::QueuePacket(int track, int64_t timeMs, bool key, const void *data,
                                size_t size) {
	Packet p;
	p.track = track;
	p.timeMs = timeMs;
	p.key = key;
	p.data.assign((const char *)data, size);
	(track == WebmWriter::kVideoTrack ? outVideo_ : outAudio_).push_back(p);
}

// limitMs までの塊を、音と映像を混ぜて時刻順に書く（同じ時刻なら映像を先に。
// キーフレームで Cluster を切るので、その時刻の音は新しい Cluster へ入る）。
// それぞれの中はもともと時刻順。
void StreamEncoder::WriteOrdered(int64_t limitMs) {
	for (;;) {
		std::deque<Packet> *q = 0;
		if (!outVideo_.empty() &&
		    (outAudio_.empty() || outVideo_.front().timeMs <= outAudio_.front().timeMs)) {
			q = &outVideo_;
		} else if (!outAudio_.empty()) {
			q = &outAudio_;
		}
		if (q == 0 || q->front().timeMs > limitMs) break;
		const Packet &p = q->front();
		writer_.AddBlock(p.track, p.timeMs, p.key, p.data.data(), p.data.size());
		q->pop_front();
	}
}

void StreamEncoder::FlushClusters() {
	std::string data;
	int64_t t = 0;
	bool key = false;
	while (writer_.TakeCluster(&data, &t, &key)) source_->AddCluster(data, t, key);
}

}  // namespace sdlcastg
