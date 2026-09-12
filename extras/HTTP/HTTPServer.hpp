// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/HTTP/HTTPServer.hpp
//
// A loopback HTTP/1.1 server for tests that fetch: this extra's own suite in
// tests/extras/TestHTTP.cpp, and any consumer's test that would otherwise need
// a host out on the internet to assert against. Header-only, no object code in
// zahlen_http, no engine header, and nothing third-party -- the platform's
// sockets and the standard library are the whole dependency.
//
// It binds an ephemeral port on 127.0.0.1 and never on an interface, answers
// every request with "Connection: close" so a handler never has to think about
// a second request on the same socket, and runs one std::jthread per connection
// behind one acceptor. Construction cannot fail the way a test would want to
// hear about, so IsListening() is the check: a server that did not come up is a
// skipped test, not a failed one.
//
// Why it lives here rather than in the suite that needed it first: a fetcher
// can only be tested against routes whose answers are known in advance, and a
// second test suite that needs one would otherwise write a second server and
// get its own ideas about HTTP/1.1. The routes below are the behaviours
// HTTP.hpp promises, spelled out as answers.
//
// Routes are fixed and small, each one standing for a behaviour the API
// promises: /ok and /binary for bodies, /notfound and /servererror for the rule
// that a status is data, /redirect and /loop and /tofile for redirect handling,
// /head for the one method that is an option rather than a verb, /spacing for
// header parsing, /gzip for the decoded body, /slow for a peer that never
// answers, and /echo for what the client actually sent. Anything else is a 404.
//
// Windows: this header is winsock, and winsock is a library. zahlen_http links
// ws2_32 as INTERFACE for that reason, so a target that links the extra gets
// it; one that includes this header without linking zahlen_http has to add
// ws2_32 itself.

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ZHLN::HTTP {

namespace Detail {

#if defined(_WIN32)
using SocketHandle                      = SOCKET;
inline constexpr SocketHandle kNoSocket = INVALID_SOCKET;

/// getsockname's address length is an int here and a socklen_t elsewhere.
using SocketLength = int;

/// WSAStartup is per-process and refcounted, so one is held for as long as any
/// socket exists.
struct WinsockScope {
    WSADATA data {};

    WinsockScope() {
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockScope() {
        WSACleanup();
    }

    WinsockScope(const WinsockScope&)            = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
};

inline auto CloseSocket(SocketHandle socket) noexcept -> void {
    if (socket != kNoSocket) {
        closesocket(socket);
    }
}
#else
using SocketHandle                      = int;
inline constexpr SocketHandle kNoSocket = -1;
using SocketLength                      = socklen_t;

struct WinsockScope {};

inline auto CloseSocket(SocketHandle socket) noexcept -> void {
    if (socket != kNoSocket) {
        close(socket);
    }
}
#endif

/// recv and send take an int length on Windows and a size_t one on POSIX. The
/// casts live here so the server reads the same on both.
inline auto Receive(SocketHandle socket, char* buffer, size_t capacity) -> ptrdiff_t {
#if defined(_WIN32)
    return recv(socket, buffer, static_cast<int>(capacity), 0);
#else
    return recv(socket, buffer, capacity, 0);
#endif
}

inline auto Transmit(SocketHandle socket, const char* buffer, size_t length) -> ptrdiff_t {
#if defined(_WIN32)
    return send(socket, buffer, static_cast<int>(length), 0);
#else
    return send(socket, buffer, length, 0);
#endif
}

} // namespace Detail

/// Not held const at the call sites, which looks like an omission and is not:
/// the handlers this spawns write its request counter and its handler list, and
/// a const object whose own threads modify it is undefined behaviour however
/// harmless it looks in practice.
class LoopbackServer {
  public:
    /// What /gzip sends: a real gzip stream of the twelve bytes "decoded body",
    /// header and CRC included, so a test of that route is about libcurl decoding a
    /// document and not about this server compressing one. Public because the
    /// Content-Length it arrives with is this array's size, and a test asserts the
    /// two disagree with the decoded body.
    static constexpr std::array<uint8_t, 32> kGzipOfDecodedBody {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x4B,
                                                                 0x49, 0x4D, 0xCE, 0x4F, 0x49, 0x4D, 0x51, 0x48, 0xCA, 0x4F, 0xA9,
                                                                 0x04, 0x00, 0x81, 0x21, 0x6F, 0xED, 0x0C, 0x00, 0x00, 0x00};

    LoopbackServer() {
#if defined(_WIN32)
        static Detail::WinsockScope winsock;
        (void) winsock;
#endif
        _listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_listener == Detail::kNoSocket) {
            return;
        }

        // TIME_WAIT from a previous run of the suite would otherwise make the
        // bind fail, and a bind failure is a failed test rather than a skipped
        // one.
        const int reuse = 1;
        setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port   = htons(0); // an ephemeral port: two suites can run at once
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (bind(_listener, reinterpret_cast<const sockaddr*>(&address), static_cast<Detail::SocketLength>(sizeof(address))) != 0) {
            Detail::CloseSocket(_listener);
            _listener = Detail::kNoSocket;
            return;
        }

        sockaddr_in          bound {};
        Detail::SocketLength boundLength = sizeof(bound);
        if (getsockname(_listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
            Detail::CloseSocket(_listener);
            _listener = Detail::kNoSocket;
            return;
        }
        _port = ntohs(bound.sin_port);

        if (listen(_listener, 64) != 0) {
            Detail::CloseSocket(_listener);
            _listener = Detail::kNoSocket;
            return;
        }

        _acceptor = std::jthread([this](std::stop_token) { AcceptLoop(); });
    }

    ~LoopbackServer() {
        _stop.store(true);

        // The accept loop is blocked in accept(), which no flag reaches, so it
        // gets a connection to wake it: one to itself, closed straight away.
        if (_listener != Detail::kNoSocket) {
            const Detail::SocketHandle wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (wake != Detail::kNoSocket) {
                sockaddr_in address {};
                address.sin_family = AF_INET;
                address.sin_port   = htons(_port);
                inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
                connect(wake, reinterpret_cast<const sockaddr*>(&address), static_cast<Detail::SocketLength>(sizeof(address)));
                Detail::CloseSocket(wake);
            }
        }

        // The acceptor first, so no new handler appears while the list is being
        // joined, then the handlers, then the socket they were accepted from.
        if (_acceptor.joinable()) {
            _acceptor.join();
        }
        {
            const std::lock_guard lock(_handlersMutex);
            for (auto& handler: _handlers) {
                if (handler.joinable()) {
                    handler.join();
                }
            }
            _handlers.clear();
        }
        Detail::CloseSocket(_listener);
        _listener = Detail::kNoSocket;
    }

    LoopbackServer(const LoopbackServer&)            = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] auto IsListening() const noexcept -> bool {
        return _listener != Detail::kNoSocket;
    }

    /// The URL of a route on this server, port included.
    [[nodiscard]] auto Url(std::string_view path) const -> std::string {
        return std::format("http://127.0.0.1:{}{}", _port, path);
    }

    /// Connections accepted so far, wake-up connection included. The smuggling
    /// test reads it before and after a refused request: what the client declined
    /// to send must not have reached anyone.
    [[nodiscard]] auto Requests() const noexcept -> uint64_t {
        return _requests.load();
    }

  private:
    void AcceptLoop() {
        while (!_stop.load()) {
            const Detail::SocketHandle connection = accept(_listener, nullptr, nullptr);
            if (connection == Detail::kNoSocket) {
                if (_stop.load()) {
                    return;
                }
                continue;
            }
            const std::lock_guard lock(_handlersMutex);
            _handlers.emplace_back([this, connection](std::stop_token) { Handle(connection); });
        }
    }

    /// Reads one request, answers it, closes. A handler owns its socket from
    /// here on.
    void Handle(Detail::SocketHandle connection) {
        _requests.fetch_add(1);

        std::string received;
        char        buffer[8192];
        size_t      headerEnd = std::string::npos;
        while ((headerEnd = received.find("\r\n\r\n")) == std::string::npos) {
            const auto count = Detail::Receive(connection, buffer, sizeof(buffer));
            if (count <= 0) {
                Detail::CloseSocket(connection);
                return;
            }
            received.append(buffer, static_cast<size_t>(count));
        }

        const std::string head = received.substr(0, headerEnd);
        std::string       body = received.substr(headerEnd + 4);

        std::vector<std::string> lines;
        size_t                   cursor = 0;
        while (cursor < head.size()) {
            const auto end = head.find("\r\n", cursor);
            lines.push_back(head.substr(cursor, (end == std::string::npos ? head.size() : end) - cursor));
            cursor = (end == std::string::npos) ? head.size() : end + 2;
        }

        const std::string requestLine = lines.empty() ? std::string {} : lines.front();
        size_t            expected    = 0;
        for (size_t i = 1; i < lines.size(); ++i) {
            const auto colon = lines[i].find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = lines[i].substr(0, colon);
            for (auto& character: name) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            if (name == "content-length") {
                expected = static_cast<size_t>(std::strtoul(lines[i].c_str() + colon + 1, nullptr, 10));
            }
        }
        while (body.size() < expected) {
            const auto count = Detail::Receive(connection, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            body.append(buffer, static_cast<size_t>(count));
        }

        const std::string answer = Route(requestLine, std::span(lines).subspan(lines.empty() ? 0 : 1), body);
        SendAll(connection, answer);
        Detail::CloseSocket(connection);
    }

    static void SendAll(Detail::SocketHandle connection, const std::string& answer) {
        size_t sent = 0;
        while (sent < answer.size()) {
            const auto count = Detail::Transmit(connection, answer.data() + sent, answer.size() - sent);
            if (count <= 0) {
                return;
            }
            sent += static_cast<size_t>(count);
        }
    }

    std::string Route(std::string_view requestLine, std::span<const std::string> headerLines, const std::string& body) const {
        std::string path;
        {
            const auto first = requestLine.find(' ');
            const auto last  = (first == std::string_view::npos) ? first : requestLine.find(' ', first + 1);
            if (first != std::string_view::npos && last != std::string_view::npos) {
                path = std::string(requestLine.substr(first + 1, last - first - 1));
            }
        }

        if (path == "/ok") {
            return Answer(200, "OK", {{"X-Test", "alpha"}, {"Content-Type", "text/plain"}}, "hello");
        }
        if (path == "/binary") {
            std::string payload;
            payload.resize(256);
            for (size_t i = 0; i < 256; ++i) {
                payload[i] = static_cast<char>(i);
            }
            return Answer(200, "OK", {{"Content-Type", "application/octet-stream"}}, payload);
        }
        if (path == "/notfound") {
            return Answer(404, "Not Found", {{"X-Test", "missing"}}, "gone");
        }
        if (path == "/servererror") {
            return Answer(500, "Internal Server Error", {}, "boom");
        }
        if (path == "/redirect") {
            return Answer(302, "Found", {{"Location", "/ok"}}, "");
        }
        if (path == "/loop") {
            return Answer(302, "Found", {{"Location", "/loop"}}, "");
        }
        if (path == "/tofile") {
            // Where a redirect is allowed to go is the client's decision, not the
            // server's, and this is the server trying to make it one.
            return Answer(302, "Found", {{"Location", "file:///etc/passwd"}}, "");
        }
        if (path == "/head") {
            // A HEAD answer may name a length it does not send: the headers are
            // the response, and there is no body to wait for.
            std::string answer = "HTTP/1.1 200 OK\r\nX-Test: head\r\nContent-Length: 5\r\nConnection: close\r\n\r\n";
            return answer;
        }
        if (path == "/spacing") {
            return Answer(200, "OK", {{"X-Space", "    padded   "}, {"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}, {"X-Empty", "   "}}, "spaced");
        }
        if (path == "/gzip") {
            const std::string payload(reinterpret_cast<const char*>(kGzipOfDecodedBody.data()), kGzipOfDecodedBody.size());
            return Answer(200, "OK", {{"Content-Encoding", "gzip"}, {"Content-Type", "text/plain"}}, payload);
        }
        if (path == "/slow") {
            // Never answers. The client's timeout ends the transfer and the stop
            // flag ends this handler, so neither waits on the other -- a sleep
            // here would hold up the suite for as long as it slept.
            while (!_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return {};
        }
        if (path == "/echo") {
            std::string payload(requestLine);
            for (const auto& line: headerLines) {
                payload += "\r\n";
                payload += line;
            }
            payload += "\r\n\r\n";
            payload += body;
            return Answer(200, "OK", {{"Content-Type", "text/plain"}}, payload);
        }
        return Answer(404, "Not Found", {}, "no such route");
    }

    using HeaderPair = std::pair<std::string_view, std::string_view>;

    /// An initializer_list rather than a span: a dynamic-extent span cannot be
    /// built from a braced list (C++20 removed that constructor, because the list
    /// it would borrow from dies at the end of the full expression), and every
    /// call site here is a literal list of pairs.
    static std::string Answer(int status, std::string_view reason, std::initializer_list<HeaderPair> headers, std::string_view body) {
        std::string answer = std::format("HTTP/1.1 {} {}\r\n", status, reason);
        for (const auto& [name, value]: headers) {
            answer += std::format("{}: {}\r\n", name, value);
        }
        answer += std::format("Content-Length: {}\r\n", body.size());
        answer += "Connection: close\r\n\r\n";
        answer += body;
        return answer;
    }

    Detail::SocketHandle      _listener = Detail::kNoSocket;
    uint16_t                  _port     = 0;
    std::atomic<bool>         _stop {false};
    std::atomic<uint64_t>     _requests {0};
    std::jthread              _acceptor;
    std::mutex                _handlersMutex;
    std::vector<std::jthread> _handlers;
};

} // namespace ZHLN::HTTP
