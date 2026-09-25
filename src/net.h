// sdlcastg - ソケットの薄い包み（Windows の Winsock と POSIX の違いを吸収する）
//
// ライブラリの中でだけ使う。IPv4 だけを扱う（Cast の受信側は IPv4 で
// 見つかり、手元の LAN もそれで足りる）。

#ifndef SDLCASTG_NET_H
#define SDLCASTG_NET_H

#include <stdint.h>

#include <string>
#include <vector>

namespace sdlcastg {
namespace net {

#ifdef _WIN32
typedef uintptr_t Socket;
#else
typedef int Socket;
#endif
extern const Socket kInvalidSocket;

// Winsock の初期化（何度呼んでもよい。POSIX では何もしない）。
bool Init();
void Quit();

void Close(Socket s);

// 直近のソケットのエラーを文字列で（ログ用。英語のまま）。
std::string LastError();

// TCP で host:port へつなぐ。timeoutMs を過ぎたら諦める。
Socket ConnectTcp(const std::string &host, int port, int timeoutMs, std::string *err);

// 送受信の待ち時間の上限 (ms)。0 なら待ち続ける。
void SetTimeouts(Socket s, int recvMs, int sendMs);

// s の手元側のアドレス（"192.168.1.10" の形）。
std::string LocalAddress(Socket s);

// host へ向かうときに使われる手元のアドレス。UDP で connect して
// getsockname するだけ（何も送らない）。複数の NIC があっても正しい口が選べる。
std::string LocalAddressToward(const std::string &host);

// "a.b.c.d" の形か。
bool IsIPv4(const std::string &s);

// 手元の IPv4 の口（ループバックを除く）。取れなければ空。
std::vector<std::string> LocalIPv4Addresses();

// 全部送る。途中で失敗したら false。
// send() と同じだが、相手が切ったソケットに書いても SIGPIPE を出さない
// （POSIX では出るとプロセスごと落ちる。受信側が切っただけでアプリが終わる）。
int Send(Socket s, const void *data, int size);
bool SendAll(Socket s, const void *data, size_t size);

}  // namespace net
}  // namespace sdlcastg

#endif  // SDLCASTG_NET_H
