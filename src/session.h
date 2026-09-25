// sdlcastg - 受信側との 1 回のやりとり（受信アプリの起動・再生の指示・状態）
//
// 流れ:
//   Connect … receiver-0 へ CONNECT し、状態を尋ねる（TV の表示は変わらない）
//   Load    … 受信アプリ（既定は Default Media Receiver）が動いていなければ
//             LAUNCH し、動き出したら（RECEIVER_STATUS に transportId が
//             出たら）そこへ CONNECT して LOAD を送る
//   Stop    … 受信アプリを止める（TV はアプリの無い状態に戻る）
//
// 受信側からの知らせは CastChannel の入出力スレッドで受け、状態（Status）を
// 更新する。読む側は status() で写しを取る。

#ifndef SDLCASTG_SESSION_H
#define SDLCASTG_SESSION_H

#include <stdint.h>

#include <mutex>
#include <string>

#include "channel.h"

namespace sdlcastg {

// Default Media Receiver。登録なしで誰でも使える受信アプリ。
extern const char kDefaultMediaReceiver[];

struct LoadRequest {
	std::string url;
	std::string contentType;  // "video/webm" など
	bool live;                // streamType を LIVE にする（流しっぱなし）
	std::string title;

	LoadRequest() : live(false) {}
};

struct SessionStatus {
	enum State {
		kDisconnected = 0,
		kConnecting,  // つないで受信側の状態を待っている
		kConnected,   // つながった（受信アプリはまだ）
		kLaunching,   // 受信アプリを起こしている
		kLoading,     // LOAD を送った
		kBuffering,
		kPlaying,
		kPaused,
		kIdle,        // 再生が終わった（idleReason を見る）
		kError,
	};
	State state;
	std::string error;  // kError / kDisconnected の理由

	// 受信側
	std::string runningAppId;        // いま動いている受信アプリ（無ければ空）
	std::string runningAppName;
	float volume;                    // 0..1
	bool muted;

	// 自分が起こした受信アプリ
	std::string transportId;
	std::string sessionId;

	// 再生
	int mediaSessionId;              // 0 なら無し
	std::string playerState;         // IDLE / BUFFERING / PLAYING / PAUSED
	std::string prevPlayerState;     // その前の状態（何から IDLE になったかを見るため）
	std::string idleReason;          // FINISHED / ERROR / CANCELLED / INTERRUPTED
	// 秒。受信側が知らせてきた値に、再生中ならそれからの経過を足したもの
	// （問い合わせは 2 秒ごとなので、そのままだと最大 2 秒古い）。
	double currentTime;

	SessionStatus()
	    : state(kDisconnected), volume(0.0f), muted(false), mediaSessionId(0), currentTime(0.0) {}
};

const char *StateName(SessionStatus::State s);

class Session {
public:
	Session();
	~Session();

	bool Connect(const std::string &host, int port, std::string *err);

	// 受信アプリを起こして（動いていればそのまま）、req を再生させる。
	void Load(const LoadRequest &req, const std::string &appId = kDefaultMediaReceiver);

	// 受信アプリを止める。
	void Stop();

	// 受信側の音量 (0..1)。
	void SetVolume(float level);
	// 受信側の再生に SEEK を送る（秒。受信側にとっての時刻）。再生を続ける。
	void Seek(double seconds);

	// 別れの挨拶をしてから切る。Stop していなければ受信アプリは動いたまま残る。
	void Disconnect();

	SessionStatus status() const;
	std::string localAddress() const { return channel_.localAddress(); }
	std::string host() const { return host_; }

private:
	void OnMessage(const CastMessage &m);
	void OnClosed(const std::string &reason);
	void OnTick();
	void SendReceiver(const std::string &json);
	void SendMedia(const std::string &json);
	int NextRequestId();
	void SendLoadLocked();

	CastChannel channel_;
	std::string host_;

	mutable std::mutex mutex_;
	SessionStatus status_;
	std::string wantAppId_;
	LoadRequest pendingLoad_;
	bool loadPending_;
	bool appSeen_;            // 自分の受信アプリが一度でも動いているのを見た
	int requestId_;
	int loadRequestId_;  // 最後に送った LOAD の requestId
	// こちらの LOAD で始まった再生（mediaSessionId）。受信側は LOAD への返事の
	// MEDIA_STATUS に同じ requestId を付けてくるので、そこで知る。それまでの間と、
	// それより古い再生の知らせは無視する。前の再生（読み込み直す前のもの、前の
	// 接続で流していたもの）が遮られた IDLE / INTERRUPTED や、遮られる直前の
	// PLAYING を、こちらの再生の状態と取り違えて「受信側で終わった」と判断していた。
	int ourMediaSessionId_;
	bool awaitingLoadStatus_;   // LOAD への返事を待っている
	int64_t loadSentAtMs_;      // LOAD を送った時刻（返事が来なければ 10 秒で諦める）
	// 受信側が受け付ける操作（MEDIA_STATUS の supportedMediaCommands）。変わったら
	// ログに出す（TV のリモコンのどのキーが効くかの手掛かり）。-1 はまだ来ていない。
	long long supportedCommands_;
	int ticks_;
	int64_t currentTimeAtMs_;  // status_.currentTime を受け取った時刻（steady_clock）
};

}  // namespace sdlcastg

#endif  // SDLCASTG_SESSION_H
