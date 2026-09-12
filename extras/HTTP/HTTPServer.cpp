// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/HTTP/HTTPServer.cpp
//
// Every platform difference the loopback server has lives here, the way all of
// libcurl lives in HTTP.cpp: the handle type, recv and send taking an int length
// on Windows and a size_t one on POSIX, WSAStartup being per-process and
// refcounted, and close being closesocket. HTTPServer.hpp names none of it, so a
// consumer that includes it compiles the same everywhere, and never has to get
// winsock2.h in ahead of windows.h in its own include order.
//
// The class holds a socket as a std::intptr_t, because that is the one spelling
// both platforms fit into without this header knowing either: -1 is a bad fd on
// POSIX and INVALID_SOCKET on Windows read at the same width, and the
// constructor asserts the two agree. Listener() below is the crossing back.

#include <HTTP/HTTPServer.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
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
namespace {

#if defined(_WIN32)
using SocketHandle                      = SOCKET;
inline constexpr SocketHandle kNoSocket = INVALID_SOCKET;

/// getsockname's address length is an int here and a socklen_t elsewhere.
using SocketLength = int;

/// WSAStartup is per-process and refcounted, so one is held for as long as any
/// socket exists. The server's constructor keeps exactly one, as a static.
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

auto CloseSocket(SocketHandle socket) noexcept -> void {
    if (socket != kNoSocket) {
        closesocket(socket);
    }
}
#else
using SocketHandle                      = int;
inline constexpr SocketHandle kNoSocket = -1;
using SocketLength                      = socklen_t;

struct WinsockScope {};

auto CloseSocket(SocketHandle socket) noexcept -> void {
    if (socket != kNoSocket) {
        close(socket);
    }
}
#endif

/// recv and send take an int length on Windows and a size_t one on POSIX. The
/// casts live here so the server reads the same on both.
auto Receive(SocketHandle socket, char* buffer, size_t capacity) -> ptrdiff_t {
#if defined(_WIN32)
    return recv(socket, buffer, static_cast<int>(capacity), 0);
#else
    return recv(socket, buffer, capacity, 0);
#endif
}

auto Transmit(SocketHandle socket, const char* buffer, size_t length) -> ptrdiff_t {
#if defined(_WIN32)
    return send(socket, buffer, static_cast<int>(length), 0);
#else
    return send(socket, buffer, length, 0);
#endif
}

/// A socket the class holds, as this platform's handle. The header only ever
/// sees the integer.
auto Listener(std::intptr_t held) noexcept -> SocketHandle {
    return static_cast<SocketHandle>(held);
}

} // namespace

LoopbackServer::LoopbackServer() {
    // kNoListener is the header's spelling of "no socket": -1, which is a bad
    // fd on POSIX and INVALID_SOCKET on Windows read as a signed integer of the
    // same width. If those two ever disagree, IsListening() lies.
    static_assert(static_cast<std::intptr_t>(kNoSocket) == kNoListener, "the header's sentinel is this platform's");

#if defined(_WIN32)
    static WinsockScope winsock;
    (void) winsock;
#endif
    _listener = static_cast<std::intptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (_listener == kNoListener) {
        return;
    }

    // One handle for the rest of the constructor: it does not change again until
    // a failure below clears it.
    const SocketHandle listener = Listener(_listener);

    // TIME_WAIT from a previous run of the suite would otherwise make the
    // bind fail, and a bind failure is a failed test rather than a skipped
    // one.
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port   = htons(0); // an ephemeral port: two suites can run at once
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), static_cast<SocketLength>(sizeof(address))) != 0) {
        CloseSocket(listener);
        _listener = kNoListener;
        return;
    }

    sockaddr_in  bound {};
    SocketLength boundLength = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
        CloseSocket(listener);
        _listener = kNoListener;
        return;
    }
    _port = ntohs(bound.sin_port);

    if (listen(listener, 64) != 0) {
        CloseSocket(listener);
        _listener = kNoListener;
        return;
    }

    _acceptor = std::jthread([this](std::stop_token) { AcceptLoop(); });
}

LoopbackServer::~LoopbackServer() {
    _stop.store(true);

    // The accept loop is blocked in accept(), which no flag reaches, so it
    // gets a connection to wake it: one to itself, closed straight away.
    if (_listener != kNoListener) {
        const SocketHandle wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (wake != kNoSocket) {
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_port   = htons(_port);
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            connect(wake, reinterpret_cast<const sockaddr*>(&address), static_cast<SocketLength>(sizeof(address)));
            CloseSocket(wake);
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
    CloseSocket(Listener(_listener));
    _listener = kNoListener;
}

auto LoopbackServer::Url(std::string_view path) const -> std::string {
    return std::format("http://127.0.0.1:{}{}", _port, path);
}

void LoopbackServer::AcceptLoop() {
    while (!_stop.load()) {
        const SocketHandle accepted = accept(Listener(_listener), nullptr, nullptr);
        if (accepted == kNoSocket) {
            if (_stop.load()) {
                return;
            }
            continue;
        }
        // Handle takes a socket the way the header spells one, which is the
        // only spelling that crosses into it.
        const auto connection = static_cast<std::intptr_t>(accepted);

        const std::lock_guard lock(_handlersMutex);
        _handlers.emplace_back([this, connection](std::stop_token) { Handle(connection); });
    }
}

void LoopbackServer::Handle(std::intptr_t connection) {
    _requests.fetch_add(1);

    std::string received;
    char        buffer[8192];
    size_t      headerEnd = std::string::npos;
    while ((headerEnd = received.find("\r\n\r\n")) == std::string::npos) {
        const auto count = Receive(Listener(connection), buffer, sizeof(buffer));
        if (count <= 0) {
            CloseSocket(Listener(connection));
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
        const auto count = Receive(Listener(connection), buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        body.append(buffer, static_cast<size_t>(count));
    }

    const std::string answer = Route(requestLine, std::span(lines).subspan(lines.empty() ? 0 : 1), body);
    SendAll(connection, answer);
    CloseSocket(Listener(connection));
}

void LoopbackServer::SendAll(std::intptr_t connection, const std::string& answer) {
    size_t sent = 0;
    while (sent < answer.size()) {
        const auto count = Transmit(Listener(connection), answer.data() + sent, answer.size() - sent);
        if (count <= 0) {
            return;
        }
        sent += static_cast<size_t>(count);
    }
}

std::string LoopbackServer::Route(std::string_view requestLine, std::span<const std::string> headerLines, const std::string& body) const {
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

std::string LoopbackServer::Answer(int status, std::string_view reason, std::initializer_list<HeaderPair> headers, std::string_view body) {
    std::string answer = std::format("HTTP/1.1 {} {}\r\n", status, reason);
    for (const auto& [name, value]: headers) {
        answer += std::format("{}: {}\r\n", name, value);
    }
    answer += std::format("Content-Length: {}\r\n", body.size());
    answer += "Connection: close\r\n\r\n";
    answer += body;
    return answer;
}

} // namespace ZHLN::HTTP
