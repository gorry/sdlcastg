// sdlcastg - 受信側に中身を取りに来てもらうための、最小の HTTP サーバー
//
// Default Media Receiver は URL を受け取って、自分で取りに来る。そのために
// 送る側で待ち受ける。扱うのは GET と HEAD だけ。
//
// 中身は HttpSource で差し替える:
//   ファイル … 大きさが分かる。Range に応える（206）。
//   ライブ   … 大きさが分からない（size() < 0）。Range は無視して 200 で
//              流しっぱなしにする（2026-09-25 に Chromecast 内蔵の TV で確かめた作法。
//              受信側は Range: bytes=0- を付けた GET を 1 回だけ送ってくる）。

#ifndef SDLCASTG_HTTPSERVER_H
#define SDLCASTG_HTTPSERVER_H

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace sdlcastg {

// 1 回の応答で読む中身。Read は止まってよい（ライブなら次が来るまで待つ）。
class HttpStream {
public:
	virtual ~HttpStream() {}
	// 読めたバイト数。0 なら終わり、負なら誤り。
	virtual int Read(void *buf, int size) = 0;
};

class HttpSource {
public:
	virtual ~HttpSource() {}
	virtual std::string contentType() const = 0;
	// 全体の大きさ。負ならライブ（大きさが決まらない）。
	virtual int64_t size() const = 0;
	// offset から読む HttpStream を作る。失敗したら 0。
	virtual HttpStream *Open(int64_t offset) = 0;
};

// ファイルをそのまま配る。
class FileSource : public HttpSource {
public:
	FileSource(const std::string &path, const std::string &contentType);
	bool valid() const { return size_ >= 0; }
	std::string contentType() const { return contentType_; }
	int64_t size() const { return size_; }
	HttpStream *Open(int64_t offset);

private:
	std::string path_;
	std::string contentType_;
	int64_t size_;
};

class HttpServer {
public:
	HttpServer();
	~HttpServer();

	// port が 0 なら空いているものを使う。
	bool Start(int port, std::string *err);
	void Stop();
	bool running() const { return running_.load(); }
	int port() const { return port_; }

	// path（"/media/a.webm" の形）で source を配る。同じ path は差し替え。
	void Serve(const std::string &path, std::shared_ptr<HttpSource> source);
	void Unserve(const std::string &path);

	// 受けた要求の数（見本プログラムの表示用）。
	int requestCount() const { return requests_.load(); }

private:
	void AcceptLoop();
	void Handle(net::Socket s, std::shared_ptr<std::atomic<bool> > done);

	net::Socket listener_;
	int port_;
	std::thread acceptThread_;
	std::atomic<bool> running_;
	std::atomic<bool> quit_;
	std::atomic<int> requests_;

	std::mutex mutex_;
	std::map<std::string, std::shared_ptr<HttpSource> > sources_;
	// 接続ごとの作業スレッド。終わったものは次の accept のときに片付ける。
	struct Worker {
		std::thread thread;
		std::shared_ptr<std::atomic<bool> > done;
	};
	std::vector<Worker> workers_;
	std::vector<net::Socket> clients_;  // Stop で切るため
};

// 拡張子から Content-Type を決める（分からなければ application/octet-stream）。
std::string GuessContentType(const std::string &path);

}  // namespace sdlcastg

#endif  // SDLCASTG_HTTPSERVER_H
