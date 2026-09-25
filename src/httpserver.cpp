// sdlcastg - 受信側に中身を取りに来てもらうための、最小の HTTP サーバー

#include "httpserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace sdlcastg {

namespace {

const size_t kMaxRequestHeader = 16 * 1024;
const int kRecvTimeoutMs = 5000;
// 相手が読まなくなったら諦めるまでの長さ。ライブは受信側が数秒ぶん
// 溜めたところで読むのを緩めるので、短すぎないこと。
const int kSendTimeoutMs = 30000;

FILE *OpenUtf8(const std::string &path) {
#ifdef _WIN32
	const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, 0, 0);
	if (n <= 0) return 0;
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
	return _wfopen(w.c_str(), L"rb");
#else
	return fopen(path.c_str(), "rb");
#endif
}

bool Seek64(FILE *f, int64_t off, int whence) {
#ifdef _WIN32
	return _fseeki64(f, off, whence) == 0;
#else
	return fseeko(f, (off_t)off, whence) == 0;
#endif
}

int64_t Tell64(FILE *f) {
#ifdef _WIN32
	return _ftelli64(f);
#else
	return (int64_t)ftello(f);
#endif
}

class FileStream : public HttpStream {
public:
	explicit FileStream(FILE *f) : f_(f) {}
	~FileStream() { fclose(f_); }
	int Read(void *buf, int size) { return (int)fread(buf, 1, (size_t)size, f_); }

private:
	FILE *f_;
};

std::string Trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
	return s.substr(a, b - a);
}

std::string LowerAscii(std::string s) {
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
	}
	return s;
}

bool SendText(net::Socket s, const std::string &text) {
	return net::SendAll(s, text.data(), text.size());
}

void SendStatus(net::Socket s, const char *status) {
	SendText(s, std::string("HTTP/1.1 ") + status +
	                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

}  // namespace

// ---------------------------------------------------------------------------

FileSource::FileSource(const std::string &path, const std::string &contentType)
    : path_(path), contentType_(contentType), size_(-1) {
	FILE *f = OpenUtf8(path);
	if (f == 0) return;
	if (Seek64(f, 0, SEEK_END)) size_ = Tell64(f);
	fclose(f);
}

HttpStream *FileSource::Open(int64_t offset) {
	FILE *f = OpenUtf8(path_);
	if (f == 0) return 0;
	if (offset > 0 && !Seek64(f, offset, SEEK_SET)) {
		fclose(f);
		return 0;
	}
	return new FileStream(f);
}

std::string GuessContentType(const std::string &path) {
	const size_t dot = path.rfind('.');
	const std::string ext = (dot == std::string::npos) ? std::string() : LowerAscii(path.substr(dot));
	if (ext == ".webm") return "video/webm";
	if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
	if (ext == ".mkv") return "video/x-matroska";
	if (ext == ".mp3") return "audio/mpeg";
	if (ext == ".m4a" || ext == ".aac") return "audio/mp4";
	if (ext == ".ogg" || ext == ".opus") return "audio/ogg";
	if (ext == ".wav") return "audio/wav";
	if (ext == ".flac") return "audio/flac";
	if (ext == ".png") return "image/png";
	if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
	return "application/octet-stream";
}

// ---------------------------------------------------------------------------

HttpServer::HttpServer()
    : listener_(net::kInvalidSocket), port_(0), running_(false), quit_(false), requests_(0) {}

HttpServer::~HttpServer() {
	Stop();
}

bool HttpServer::Start(int port, std::string *err) {
	if (running_) return true;
	if (!net::Init()) {
		if (err) *err = "network init failed";
		return false;
	}
	net::Socket s = (net::Socket)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == net::kInvalidSocket) {
		if (err) *err = "socket() failed: " + net::LastError();
		net::Quit();
		return false;
	}
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (sockaddr *)&sa, sizeof(sa)) != 0 || listen(s, 8) != 0) {
		if (err) *err = "bind/listen failed: " + net::LastError();
		net::Close(s);
		net::Quit();
		return false;
	}
	socklen_t len = sizeof(sa);
	getsockname(s, (sockaddr *)&sa, &len);
	port_ = ntohs(sa.sin_port);
	listener_ = s;
	quit_ = false;
	running_ = true;
	acceptThread_ = std::thread(&HttpServer::AcceptLoop, this);
	return true;
}

void HttpServer::Stop() {
	if (!running_) return;
	quit_ = true;
	if (acceptThread_.joinable()) acceptThread_.join();
	net::Close(listener_);
	listener_ = net::kInvalidSocket;

	// 配っている最中の接続を切って、待っている送信を戻らせる。
	std::vector<Worker> workers;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (size_t i = 0; i < clients_.size(); i++) {
#ifdef _WIN32
			shutdown(clients_[i], SD_BOTH);
#else
			shutdown(clients_[i], SHUT_RDWR);
#endif
		}
		workers.swap(workers_);
	}
	for (size_t i = 0; i < workers.size(); i++) {
		if (workers[i].thread.joinable()) workers[i].thread.join();
	}
	running_ = false;
	net::Quit();
}

void HttpServer::Serve(const std::string &path, std::shared_ptr<HttpSource> source) {
	std::lock_guard<std::mutex> lock(mutex_);
	sources_[path] = source;
}

void HttpServer::Unserve(const std::string &path) {
	std::lock_guard<std::mutex> lock(mutex_);
	sources_.erase(path);
}

void HttpServer::AcceptLoop() {
	while (!quit_) {
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(listener_, &rd);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000;
		if (select((int)listener_ + 1, &rd, 0, 0, &tv) <= 0) continue;
		const net::Socket c = (net::Socket)accept(listener_, 0, 0);
		if (c == net::kInvalidSocket) continue;
		net::SetTimeouts(c, kRecvTimeoutMs, kSendTimeoutMs);
		std::lock_guard<std::mutex> lock(mutex_);
		// 終わった作業スレッドを片付ける。
		for (size_t i = 0; i < workers_.size();) {
			if (workers_[i].done->load()) {
				workers_[i].thread.join();
				workers_.erase(workers_.begin() + (long)i);
			} else {
				i++;
			}
		}
		clients_.push_back(c);
		Worker w;
		w.done = std::make_shared<std::atomic<bool> >(false);
		w.thread = std::thread(&HttpServer::Handle, this, c, w.done);
		workers_.push_back(std::move(w));
	}
}

void HttpServer::Handle(net::Socket s, std::shared_ptr<std::atomic<bool> > done) {
	requests_++;

	// 要求の頭（空行まで）を読む。
	std::string head;
	char buf[2048];
	while (head.find("\r\n\r\n") == std::string::npos && head.size() < kMaxRequestHeader) {
		const int n = (int)recv(s, buf, sizeof(buf), 0);
		if (n <= 0) break;
		head.append(buf, (size_t)n);
	}

	std::string method, path, range;
	{
		const size_t eol = head.find("\r\n");
		const std::string line = head.substr(0, eol);
		const size_t sp1 = line.find(' ');
		const size_t sp2 = (sp1 == std::string::npos) ? sp1 : line.find(' ', sp1 + 1);
		if (sp2 != std::string::npos) {
			method = line.substr(0, sp1);
			path = line.substr(sp1 + 1, sp2 - sp1 - 1);
			const size_t q = path.find('?');
			if (q != std::string::npos) path.erase(q);
		}
		size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
		while (pos < head.size()) {
			const size_t e = head.find("\r\n", pos);
			if (e == std::string::npos || e == pos) break;
			const std::string h = head.substr(pos, e - pos);
			const size_t colon = h.find(':');
			if (colon != std::string::npos &&
			    LowerAscii(Trim(h.substr(0, colon))) == "range") {
				range = Trim(h.substr(colon + 1));
			}
			pos = e + 2;
		}
	}

	std::shared_ptr<HttpSource> src;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::map<std::string, std::shared_ptr<HttpSource> >::iterator it = sources_.find(path);
		if (it != sources_.end()) src = it->second;
	}

	if (method != "GET" && method != "HEAD") {
		SendStatus(s, "405 Method Not Allowed");
	} else if (!src) {
		SendStatus(s, "404 Not Found");
	} else {
		const int64_t total = src->size();
		const bool live = (total < 0);
		int64_t from = 0, to = total - 1;
		bool partial = false;
		bool bad = false;
		// Range: bytes=a-b（b は省略可）。ライブでは応えない。
		if (!live && range.compare(0, 6, "bytes=") == 0) {
			const std::string spec = range.substr(6);
			const size_t dash = spec.find('-');
			if (dash != std::string::npos && dash > 0) {
				from = strtoll(spec.substr(0, dash).c_str(), 0, 10);
				if (dash + 1 < spec.size()) to = strtoll(spec.substr(dash + 1).c_str(), 0, 10);
				if (to > total - 1) to = total - 1;
				if (from > to || from >= total) bad = true;
				partial = !bad;
			}
		}
		if (bad) {
			char h[128];
			snprintf(h, sizeof(h),
			         "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
			         "Content-Length: 0\r\nConnection: close\r\n\r\n",
			         (long long)total);
			SendText(s, h);
		} else {
			std::string h = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
			h += "Content-Type: " + src->contentType() + "\r\n";
			h += "Access-Control-Allow-Origin: *\r\n";
			h += "Cache-Control: no-cache\r\n";
			h += "Connection: close\r\n";
			char line[128];
			if (!live) {
				h += "Accept-Ranges: bytes\r\n";
				snprintf(line, sizeof(line), "Content-Length: %lld\r\n", (long long)(to - from + 1));
				h += line;
				if (partial) {
					snprintf(line, sizeof(line), "Content-Range: bytes %lld-%lld/%lld\r\n",
					         (long long)from, (long long)to, (long long)total);
					h += line;
				}
			}
			h += "\r\n";
			if (SendText(s, h) && method == "GET") {
				HttpStream *st = src->Open(from);
				if (st != 0) {
					static const int kChunk = 64 * 1024;
					std::vector<char> body(kChunk);
					int64_t left = live ? -1 : (to - from + 1);
					while (!quit_ && (live || left > 0)) {
						int want = kChunk;
						if (!live && left < want) want = (int)left;
						const int n = st->Read(&body[0], want);
						if (n <= 0) break;
						if (!net::SendAll(s, &body[0], (size_t)n)) break;
						if (!live) left -= n;
					}
					delete st;
				}
			}
		}
	}

	std::lock_guard<std::mutex> lock(mutex_);
	for (size_t i = 0; i < clients_.size(); i++) {
		if (clients_[i] == s) {
			clients_.erase(clients_.begin() + (long)i);
			break;
		}
	}
	net::Close(s);
	done->store(true);
}

}  // namespace sdlcastg
