// sdlcastg - Cast V2 の通り道（TLS の上で CastMessage をやりとりする）
//
// 受信側の 8009 番へ TLS でつなぐ。受信側の証明書は自己署名なので検証しない
// （Cast の送る側はどれもそうしている。送る側の認証も要らない）。
//
// **入出力は 1 本のスレッドだけが行う。** mbedTLS の 1 つの接続を、読むスレッドと
// 書くスレッドから同時に触ることはできないため。Send は待ち行列に積むだけで、
// 入出力のスレッドが読み取りの合間（read_timeout ごと）に書き出す。
//
// 心拍もここで持つ: 受信側からの PING には PONG を返し、こちらからも 5 秒ごとに
// PING を送る。何も届かないまま kDeadMs を過ぎたら切れたとみなす。

#ifndef SDLCASTG_CHANNEL_H
#define SDLCASTG_CHANNEL_H

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "castmsg.h"
#include "net.h"

namespace sdlcastg {

class CastChannel {
public:
	// 届いたメッセージ（心拍を除く）。入出力のスレッドから呼ばれる。
	// 中で Send してよい（積むだけなので）。
	typedef std::function<void(const CastMessage &)> MessageHandler;
	// 切れた（相手が閉じた・心拍が途絶えた・通信の誤り）。入出力のスレッドから 1 回だけ。
	typedef std::function<void(const std::string &reason)> ClosedHandler;
	// 定期的に呼ぶ（状態の問い合わせなど）。入出力のスレッドから。
	typedef std::function<void()> TickHandler;

	CastChannel();
	~CastChannel();

	// Open の前に設定する。
	void SetHandlers(MessageHandler onMessage, ClosedHandler onClosed, TickHandler onTick,
	                 int tickMs);

	// つないで TLS の握手まで済ませ、入出力のスレッドを始める。
	bool Open(const std::string &host, int port, int timeoutMs, std::string *err);

	// 待ち行列を書き出してから閉じる。スレッドの終わりを待つ。
	// ハンドラの中からは呼ばないこと（自分の終わりを待つことになる）。
	void Close();

	void Send(const CastMessage &m);

	bool open() const { return running_.load(); }

	// 手元側のアドレス（受信側から見たこちらの住所。HTTP の URL に使う）。
	std::string localAddress() const { return localAddress_; }

private:
	struct Tls;

	void Run();
	bool FlushOutgoing();
	void HandleFrame(const std::string &body);

	Tls *tls_;
	std::thread thread_;
	std::atomic<bool> running_;
	std::atomic<bool> quit_;
	std::mutex mutex_;
	std::deque<std::string> outgoing_;  // 線に載せる形（長さの前置き込み）
	std::string localAddress_;

	MessageHandler onMessage_;
	ClosedHandler onClosed_;
	TickHandler onTick_;
	int tickMs_;
};

// 送る側・受ける側の既定の名前。
extern const char kSenderId[];    // "sender-0"
extern const char kReceiverId[];  // "receiver-0"

}  // namespace sdlcastg

#endif  // SDLCASTG_CHANNEL_H
