// sdlcastg - ソケットの薄い包み

#include "net.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#if !defined(__ANDROID__) || __ANDROID_API__ >= 24
#include <ifaddrs.h>
#include <net/if.h>
#define SDLCASTG_HAVE_IFADDRS 1
#endif
#endif

namespace sdlcastg {
namespace net {

#ifdef _WIN32
const Socket kInvalidSocket = (Socket)INVALID_SOCKET;
#else
const Socket kInvalidSocket = -1;
#endif

namespace {

#ifdef _WIN32
int g_initCount = 0;
#endif

void SetNonBlocking(Socket s, bool on) {
#ifdef _WIN32
	u_long v = on ? 1 : 0;
	ioctlsocket((SOCKET)s, FIONBIO, &v);
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0) flags = 0;
	fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

std::string AddrToString(const sockaddr_in &sa) {
	char buf[INET_ADDRSTRLEN] = { 0 };
	inet_ntop(AF_INET, (void *)&sa.sin_addr, buf, sizeof(buf));
	return buf;
}

}  // namespace

bool Init() {
#ifdef _WIN32
	if (g_initCount++ > 0) return true;
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		g_initCount = 0;
		return false;
	}
#endif
	return true;
}

void Quit() {
#ifdef _WIN32
	if (g_initCount <= 0) return;
	if (--g_initCount == 0) WSACleanup();
#endif
}

void Close(Socket s) {
	if (s == kInvalidSocket) return;
#ifdef _WIN32
	closesocket((SOCKET)s);
#else
	close(s);
#endif
}

std::string LastError() {
#ifdef _WIN32
	const int e = WSAGetLastError();
#else
	const int e = errno;
#endif
	char buf[32];
	snprintf(buf, sizeof(buf), "socket error %d", e);
	return buf;
}

Socket ConnectTcp(const std::string &host, int port, int timeoutMs, std::string *err) {
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	char portStr[16];
	snprintf(portStr, sizeof(portStr), "%d", port);
	addrinfo *res = 0;
	if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || res == 0) {
		if (err) *err = "cannot resolve " + host;
		return kInvalidSocket;
	}

	Socket s = (Socket)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s == kInvalidSocket) {
		if (err) *err = "socket() failed: " + LastError();
		freeaddrinfo(res);
		return kInvalidSocket;
	}

	// 待ち時間を決めてつなぐために、いったんノンブロッキングにする。
	SetNonBlocking(s, true);
	int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
	freeaddrinfo(res);
	bool ok = (r == 0);
	if (!ok) {
#ifdef _WIN32
		const bool pending = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
		const bool pending = (errno == EINPROGRESS);
#endif
		if (pending) {
			fd_set wr, ex;
			FD_ZERO(&wr);
			FD_ZERO(&ex);
			FD_SET(s, &wr);
			FD_SET(s, &ex);
			timeval tv;
			tv.tv_sec = timeoutMs / 1000;
			tv.tv_usec = (timeoutMs % 1000) * 1000;
			r = select((int)s + 1, 0, &wr, &ex, &tv);
			if (r > 0 && FD_ISSET(s, &wr)) {
				int soerr = 0;
				socklen_t len = sizeof(soerr);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
				ok = (soerr == 0);
				if (!ok && err) {
					char buf[64];
					snprintf(buf, sizeof(buf), "connect failed (error %d)", soerr);
					*err = buf;
				}
			} else if (err) {
				*err = (r == 0) ? "connect timed out" : "connect failed";
			}
		} else if (err) {
			*err = "connect failed: " + LastError();
		}
	}
	if (!ok) {
		Close(s);
		return kInvalidSocket;
	}
	SetNonBlocking(s, false);

	// 小さなメッセージを溜めずに送る（心拍や操作の返事が遅れないように）。
	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	return s;
}

void SetTimeouts(Socket s, int recvMs, int sendMs) {
#ifdef _WIN32
	DWORD r = (DWORD)recvMs, w = (DWORD)sendMs;
	setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof(r));
	setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof(w));
#else
	timeval r, w;
	r.tv_sec = recvMs / 1000;
	r.tv_usec = (recvMs % 1000) * 1000;
	w.tv_sec = sendMs / 1000;
	w.tv_usec = (sendMs % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w));
#endif
}

std::string LocalAddress(Socket s) {
	sockaddr_in sa;
	socklen_t len = sizeof(sa);
	memset(&sa, 0, sizeof(sa));
	if (getsockname(s, (sockaddr *)&sa, &len) != 0) return std::string();
	return AddrToString(sa);
}

std::string LocalAddressToward(const std::string &host) {
	Socket s = (Socket)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == kInvalidSocket) return std::string();
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(9);  // discard。UDP の connect は何も送らない
	std::string out;
	if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) == 1 &&
	    connect(s, (sockaddr *)&sa, sizeof(sa)) == 0) {
		out = LocalAddress(s);
	}
	Close(s);
	return out;
}

bool IsIPv4(const std::string &s) {
	in_addr a;
	return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

std::vector<std::string> LocalIPv4Addresses() {
	std::vector<std::string> out;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buf(size);
	ULONG r = GetAdaptersAddresses(AF_INET,
	                               GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
	                                   GAA_FLAG_SKIP_DNS_SERVER,
	                               0, (IP_ADAPTER_ADDRESSES *)&buf[0], &size);
	if (r == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		r = GetAdaptersAddresses(AF_INET,
		                         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
		                             GAA_FLAG_SKIP_DNS_SERVER,
		                         0, (IP_ADAPTER_ADDRESSES *)&buf[0], &size);
	}
	if (r != NO_ERROR) return out;
	for (IP_ADAPTER_ADDRESSES *a = (IP_ADAPTER_ADDRESSES *)&buf[0]; a != 0; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp) continue;
		if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u != 0; u = u->Next) {
			if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
			out.push_back(AddrToString(*(sockaddr_in *)u->Address.lpSockaddr));
		}
	}
#elif defined(SDLCASTG_HAVE_IFADDRS)
	ifaddrs *list = 0;
	if (getifaddrs(&list) != 0) return out;
	for (ifaddrs *p = list; p != 0; p = p->ifa_next) {
		if (p->ifa_addr == 0 || p->ifa_addr->sa_family != AF_INET) continue;
		if ((p->ifa_flags & IFF_UP) == 0 || (p->ifa_flags & IFF_LOOPBACK) != 0) continue;
		out.push_back(AddrToString(*(sockaddr_in *)p->ifa_addr));
	}
	freeifaddrs(list);
#endif
	return out;
}

int Send(Socket s, const void *data, int size) {
#ifdef MSG_NOSIGNAL
	return (int)send(s, (const char *)data, size, MSG_NOSIGNAL);
#else
	return (int)send(s, (const char *)data, size, 0);  // Windows には SIGPIPE が無い
#endif
}

bool SendAll(Socket s, const void *data, size_t size) {
	const char *p = (const char *)data;
	while (size > 0) {
		const int chunk = (size > 65536) ? 65536 : (int)size;
		const int n = Send(s, p, chunk);
		if (n <= 0) return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

}  // namespace net
}  // namespace sdlcastg
