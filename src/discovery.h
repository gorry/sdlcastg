// sdlcastg - Cast の受信側を探す（mDNS / DNS-SD の _googlecast._tcp）
//
// 224.0.0.251:5353 へ PTR の問い合わせを投げ、返ってきた PTR / SRV / TXT / A を
// まとめて 1 台ずつの情報にする。TXT の中身は id=（固有の ID）、fn=（名前）、
// md=（機種）など。
//
// **問い合わせには QU ビット（「ユニキャストで返して」）を立てる。** 立てないと
// 受信側は 224.0.0.251:5353 へ返し、そこで待っていないこちらには届かない
// （2026-09-25 に Chromecast 内蔵の TV で確かめた。docs/design.md）。念のため 5353 番でも
// 待つ（ほかのアプリが使っていて取れなければ諦める）。
//
// 手元に複数の口（NIC）があるときは、それぞれの口から問い合わせる。

#ifndef SDLCASTG_DISCOVERY_H
#define SDLCASTG_DISCOVERY_H

#include <stdint.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sdlcastg {

struct CastDevice {
	std::string id;       // TXT の id=（固有。なければ名前で代える）
	std::string name;     // fn=
	std::string model;    // md=
	std::string address;  // IPv4
	int port;             // ふつう 8009
	int capabilities;     // ca=（能力のビット。-1 なら分からない）
	int64_t lastSeenMs;

	CastDevice() : port(8009), capabilities(-1), lastSeenMs(0) {}
};

class Discovery {
public:
	Discovery();
	~Discovery();

	bool Start(std::string *err);
	void Stop();
	bool running() const { return running_.load(); }

	// 見つかったもの（見つけた順）。
	std::vector<CastDevice> devices() const;

	// 1 台に直に問い合わせる（ユニキャスト）。IP が分かっているときに、
	// 名前や機種を知るため。timeoutMs で諦める。
	static bool Probe(const std::string &address, int timeoutMs, CastDevice *out);

private:
	void Run();

	std::thread thread_;
	std::atomic<bool> running_;
	std::atomic<bool> quit_;
	mutable std::mutex mutex_;
	std::vector<CastDevice> devices_;
};

}  // namespace sdlcastg

#endif  // SDLCASTG_DISCOVERY_H
