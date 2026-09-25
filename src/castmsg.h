// sdlcastg - Cast V2 の CastMessage（protobuf）の符号化と復号
//
// 線の上では「4 バイトの長さ（ビッグエンディアン）+ protobuf の本体」。
// 本体は次の 7 項目だけなので、protobuf のライブラリは使わず手で書く。
//
//   1 protocol_version  enum    0 (CASTV2_1_0)
//   2 source_id         string  "sender-0" など
//   3 destination_id    string  "receiver-0" か、受信アプリの transportId
//   4 namespace         string  "urn:x-cast:com.google.cast.tp.heartbeat" など
//   5 payload_type      enum    0 = STRING / 1 = BINARY
//   6 payload_utf8      string  JSON
//   7 payload_binary    bytes   （使わない。読み飛ばす）

#ifndef SDLCASTG_CASTMSG_H
#define SDLCASTG_CASTMSG_H

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace sdlcastg {

struct CastMessage {
	std::string source;
	std::string destination;
	std::string ns;
	std::string payload;  // payload_utf8。BINARY のときは空
	bool binary;

	CastMessage() : binary(false) {}
	CastMessage(const std::string &src, const std::string &dst, const std::string &n,
	            const std::string &p)
	    : source(src), destination(dst), ns(n), payload(p), binary(false) {}
};

// 長さの前置きまで含めた、線に載せるバイト列。
std::string EncodeCastMessage(const CastMessage &m);

// 本体（長さの前置きを除いたもの）を読む。
bool DecodeCastMessage(const uint8_t *data, size_t size, CastMessage *out);

// 1 つのメッセージの本体の上限。Cast V2 の決まりは 64KiB。
const size_t kMaxCastMessageSize = 64 * 1024;

// 名前空間。
extern const char kNsConnection[];
extern const char kNsHeartbeat[];
extern const char kNsReceiver[];
extern const char kNsMedia[];

}  // namespace sdlcastg

#endif  // SDLCASTG_CASTMSG_H
