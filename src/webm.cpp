// sdlcastg - ライブ向けの WebM（Matroska）の書き出し

#include "webm.h"

#include <string.h>

namespace sdlcastg {

// クラスの中で値を与えた定数の定義（livesource.cpp と同じ理由）。
const int64_t WebmWriter::kMaxClusterMs;

namespace {

// 要素の ID（先頭のビットで長さが決まっているので、そのまま大きい順に書く）。
const uint32_t kEbml = 0x1A45DFA3;
const uint32_t kEbmlVersion = 0x4286;
const uint32_t kEbmlReadVersion = 0x42F7;
const uint32_t kEbmlMaxIdLength = 0x42F2;
const uint32_t kEbmlMaxSizeLength = 0x42F3;
const uint32_t kDocType = 0x4282;
const uint32_t kDocTypeVersion = 0x4287;
const uint32_t kDocTypeReadVersion = 0x4285;
const uint32_t kSegment = 0x18538067;
const uint32_t kInfo = 0x1549A966;
const uint32_t kTimecodeScale = 0x2AD7B1;
const uint32_t kMuxingApp = 0x4D80;
const uint32_t kWritingApp = 0x5741;
const uint32_t kTracks = 0x1654AE6B;
const uint32_t kTrackEntry = 0xAE;
const uint32_t kTrackNumber = 0xD7;
const uint32_t kTrackUid = 0x73C5;
const uint32_t kTrackType = 0x83;
const uint32_t kFlagLacing = 0x9C;
const uint32_t kCodecId = 0x86;
const uint32_t kCodecPrivate = 0x63A2;
const uint32_t kCodecDelay = 0x56AA;
const uint32_t kSeekPreRoll = 0x56BB;
const uint32_t kVideo = 0xE0;
const uint32_t kPixelWidth = 0xB0;
const uint32_t kPixelHeight = 0xBA;
const uint32_t kAudio = 0xE1;
const uint32_t kSamplingFrequency = 0xB5;
const uint32_t kChannels = 0x9F;
const uint32_t kCluster = 0x1F43B675;
const uint32_t kTimecode = 0xE7;
const uint32_t kSimpleBlock = 0xA3;
const uint32_t kVoid = 0xEC;

void PutId(std::string *out, uint32_t id) {
	if (id >= 0x1000000) out->push_back((char)(id >> 24));
	if (id >= 0x10000) out->push_back((char)(id >> 16));
	if (id >= 0x100) out->push_back((char)(id >> 8));
	out->push_back((char)id);
}

// 大きさ（可変長の整数）。短く書ける長さを選ぶ。
void PutSize(std::string *out, uint64_t n) {
	int len = 1;
	while (len < 8 && n >= ((uint64_t)1 << (7 * len)) - 1) len++;
	for (int i = len - 1; i >= 0; i--) {
		uint8_t b = (uint8_t)(n >> (8 * i));
		if (i == len - 1) b |= (uint8_t)(0x100 >> len);
		out->push_back((char)b);
	}
}

// 「大きさ不明」。流しっぱなしの Segment に使う。
void PutUnknownSize(std::string *out) {
	static const unsigned char kUnknown[8] = { 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	out->append((const char *)kUnknown, 8);
}

void PutElement(std::string *out, uint32_t id, const std::string &payload) {
	PutId(out, id);
	PutSize(out, payload.size());
	out->append(payload);
}

void PutUInt(std::string *out, uint32_t id, uint64_t v) {
	std::string p;
	int len = 1;
	while (len < 8 && (v >> (8 * len)) != 0) len++;
	for (int i = len - 1; i >= 0; i--) p.push_back((char)(v >> (8 * i)));
	PutElement(out, id, p);
}

void PutFloat(std::string *out, uint32_t id, double v) {
	uint64_t bits;
	memcpy(&bits, &v, sizeof(bits));
	std::string p;
	for (int i = 7; i >= 0; i--) p.push_back((char)(bits >> (8 * i)));
	PutElement(out, id, p);
}

void PutString(std::string *out, uint32_t id, const std::string &s) {
	PutElement(out, id, s);
}

}  // namespace

std::string WebmWriter::Header(int width, int height, const std::string &opusHead,
                               int opusPreSkip, int channels, int sampleRate) {
	std::string ebml;
	PutUInt(&ebml, kEbmlVersion, 1);
	PutUInt(&ebml, kEbmlReadVersion, 1);
	PutUInt(&ebml, kEbmlMaxIdLength, 4);
	PutUInt(&ebml, kEbmlMaxSizeLength, 8);
	PutString(&ebml, kDocType, "webm");
	PutUInt(&ebml, kDocTypeVersion, 4);
	PutUInt(&ebml, kDocTypeReadVersion, 2);

	std::string info;
	PutUInt(&info, kTimecodeScale, 1000000);  // 1ms
	PutString(&info, kMuxingApp, "sdlcastg");
	PutString(&info, kWritingApp, "sdlcastg");

	std::string video;
	{
		std::string v;
		PutUInt(&v, kPixelWidth, (uint64_t)width);
		PutUInt(&v, kPixelHeight, (uint64_t)height);
		PutUInt(&video, kTrackNumber, kVideoTrack);
		PutUInt(&video, kTrackUid, kVideoTrack);
		PutUInt(&video, kTrackType, 1);
		PutUInt(&video, kFlagLacing, 0);
		PutString(&video, kCodecId, "V_VP8");
		PutElement(&video, kVideo, v);
	}
	std::string audio;
	{
		std::string a;
		PutFloat(&a, kSamplingFrequency, (double)sampleRate);
		PutUInt(&a, kChannels, (uint64_t)channels);
		PutUInt(&audio, kTrackNumber, kAudioTrack);
		PutUInt(&audio, kTrackUid, kAudioTrack);
		PutUInt(&audio, kTrackType, 2);
		PutUInt(&audio, kFlagLacing, 0);
		PutString(&audio, kCodecId, "A_OPUS");
		PutElement(&audio, kCodecPrivate, opusHead);
		// 頭の読み飛ばし（pre-skip）を ns で。Opus の内部は常に 48kHz。
		PutUInt(&audio, kCodecDelay, (uint64_t)opusPreSkip * 1000000000ULL / 48000ULL);
		PutUInt(&audio, kSeekPreRoll, 80000000ULL);  // 80ms（Opus の決まり）
		PutElement(&audio, kAudio, a);
	}
	std::string tracks;
	PutElement(&tracks, kTrackEntry, video);
	PutElement(&tracks, kTrackEntry, audio);

	std::string out;
	PutElement(&out, kEbml, ebml);
	PutId(&out, kSegment);
	PutUnknownSize(&out);
	PutElement(&out, kInfo, info);
	PutElement(&out, kTracks, tracks);
	return out;
}

WebmWriter::WebmWriter() : clusterTime_(-1), clusterKey_(false), lastTime_(0), minBytesPerSec_(0) {}

void WebmWriter::AddBlock(int track, int64_t timeMs, bool keyframe, const void *data,
                          size_t size) {
	// 時刻は前の塊より前へ戻さない（呼ぶ側が時刻順に渡す。encoder.cpp の
	// WriteOrdered）。**ここで無理に揃えると、後から来た映像の時刻が音の時刻まで
	// 押し出され、映像が音より遅れて出た**（Android で 43ms。docs/design.md）。
	if (timeMs < lastTime_) timeMs = lastTime_;
	lastTime_ = timeMs;

	const bool videoKey = (track == kVideoTrack && keyframe);
	if (clusterTime_ >= 0 && (videoKey || timeMs - clusterTime_ >= kMaxClusterMs)) {
		CloseCluster();
	}
	if (clusterTime_ < 0) {
		clusterTime_ = timeMs;
		clusterKey_ = videoKey;
		body_.clear();
	}

	std::string block;
	block.push_back((char)(0x80 | track));  // トラック番号（1 バイトの可変長整数）
	const int16_t rel = (int16_t)(timeMs - clusterTime_);
	block.push_back((char)((uint16_t)rel >> 8));
	block.push_back((char)((uint16_t)rel & 0xff));
	block.push_back((char)(keyframe ? 0x80 : 0x00));
	block.append((const char *)data, size);
	PutElement(&body_, kSimpleBlock, block);
}

void WebmWriter::CloseCluster() {
	if (clusterTime_ < 0) return;
	// 流す量が下限に届かなければ、Void で詰める。
	if (minBytesPerSec_ > 0) {
		int64_t dur = lastTime_ - clusterTime_;
		if (dur < 20) dur = 20;
		const int64_t want = minBytesPerSec_ * dur / 1000;
		if ((int64_t)body_.size() + 16 < want) {
			PutElement(&body_, kVoid, std::string((size_t)(want - (int64_t)body_.size() - 16), (char)0));
		}
	}
	Ready r;
	r.data.swap(body_);
	r.timeMs = clusterTime_;
	r.key = clusterKey_;
	ready_.push_back(r);
	body_.clear();
	clusterTime_ = -1;
	clusterKey_ = false;
}

void WebmWriter::Flush() {
	CloseCluster();
}

std::string WebmWriter::ClusterBytes(int64_t timecodeMs, const std::string &blocks) {
	std::string body;
	PutUInt(&body, kTimecode, (uint64_t)(timecodeMs < 0 ? 0 : timecodeMs));
	body.append(blocks);
	std::string out;
	PutElement(&out, kCluster, body);
	return out;
}

bool WebmWriter::TakeCluster(std::string *out, int64_t *timeMs, bool *startsWithKeyframe) {
	if (ready_.empty()) return false;
	out->swap(ready_.front().data);
	*timeMs = ready_.front().timeMs;
	*startsWithKeyframe = ready_.front().key;
	ready_.pop_front();
	return true;
}

}  // namespace sdlcastg
