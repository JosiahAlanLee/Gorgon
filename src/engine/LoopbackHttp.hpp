// A minimal HTTP/1.0 client for HydraPy Studio on 127.0.0.1.
//
// Rack's network::requestJson logs every request and can't send Studio's token
// header, and the bridge polls about 30 times a second, so it has its own client.
// One request per connection (Studio's server closes after each response).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace gorgon {

struct HttpResponse {
	int status = 0;
	std::string body;
	std::string error;
};

class LoopbackHttp {
public:
	LoopbackHttp() {
#ifdef _WIN32
		WSADATA data;
		started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
	}
	~LoopbackHttp() {
#ifdef _WIN32
		if (started)
			WSACleanup();
#endif
	}
	LoopbackHttp(const LoopbackHttp&) = delete;
	LoopbackHttp& operator=(const LoopbackHttp&) = delete;

	// `body` empty sends a GET; otherwise a POST with a JSON body.
	HttpResponse request(int port, const std::string& path, const std::string& token,
	                     const std::string& body = "", int timeoutMs = 250) {
		HttpResponse out;
		if (port <= 0 || port > 65535) {
			out.error = "no port";
			return out;
		}
		Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID) {
			out.error = "socket failed";
			return out;
		}
		setTimeouts(s, timeoutMs);
		int one = 1;
		::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
		// macOS has no MSG_NOSIGNAL: without this, writing to a closed Studio would raise SIGPIPE in Rack.
		::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
		sockaddr_in addr;
		std::memset(&addr, 0, sizeof addr);
		addr.sin_family = AF_INET;
		addr.sin_port = htons(uint16_t(port));
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
			close(s);
			out.error = "Studio not reachable";
			return out;
		}
		std::string host = "127.0.0.1:" + std::to_string(port);
		std::string req = (body.empty() ? "GET " : "POST ") + path + " HTTP/1.0\r\n"
		                  "Host: " + host + "\r\n"
		                  "X-Hydrapy-Token: " + token + "\r\n"
		                  "Accept: application/json\r\n";
		if (!body.empty())
			req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
		req += "Connection: close\r\n\r\n" + body;
		if (!sendAll(s, req)) {
			close(s);
			out.error = "send failed";
			return out;
		}
		std::string raw;
		char buffer[8192];
		while (raw.size() < kMaxResponse) {
			int n = int(::recv(s, buffer, sizeof buffer, 0));
			if (n <= 0)
				break;
			raw.append(buffer, size_t(n));
		}
		close(s);
		size_t split = raw.find("\r\n\r\n");
		if (raw.compare(0, 5, "HTTP/") != 0 || split == std::string::npos) {
			out.error = raw.empty() ? "no response" : "malformed response";
			return out;
		}
		size_t space = raw.find(' ');
		out.status = std::atoi(raw.c_str() + space + 1);
		out.body = raw.substr(split + 4);
		return out;
	}

private:
#ifdef _WIN32
	typedef SOCKET Socket;
	static constexpr Socket INVALID = INVALID_SOCKET;
	static void close(Socket s) { ::closesocket(s); }
	static void setTimeouts(Socket s, int ms) {
		DWORD t = DWORD(ms);
		::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
		::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
	}
	bool started = false;
#else
	typedef int Socket;
	static constexpr Socket INVALID = -1;
	static void close(Socket s) { ::close(s); }
	static void setTimeouts(Socket s, int ms) {
		timeval t;
		t.tv_sec = ms / 1000;
		t.tv_usec = (ms % 1000) * 1000;
		::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
		::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
	}
#endif
	static constexpr size_t kMaxResponse = 4u << 20;

	static bool sendAll(Socket s, const std::string& data) {
		size_t sent = 0;
		while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
			int n = int(::send(s, data.data() + sent, int(data.size() - sent), MSG_NOSIGNAL));
#else
			int n = int(::send(s, data.data() + sent, int(data.size() - sent), 0));
#endif
			if (n <= 0)
				return false;
			sent += size_t(n);
		}
		return true;
	}
};

} // namespace gorgon
