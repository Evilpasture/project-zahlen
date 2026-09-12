// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/HTTP/HTTPServer.hpp
//
// A loopback HTTP/1.1 server for tests that fetch: this extra's own suite in
// tests/extras/TestHTTP.cpp, and any consumer's test that would otherwise need
// a host out on the internet to assert against. No engine header and nothing
// third-party -- the platform's sockets and the standard library are the whole
// dependency, and both of those live in HTTPServer.cpp.
//
// This header names no platform type and includes no socket header, the way
// HTTP.hpp names no curl type and includes no curl header. A socket crosses this
// boundary as a std::intptr_t, with kNoListener standing for "there is none",
// and HTTPServer.cpp is the only translation unit that knows whether that is a
// SOCKET or an fd. Including this header is therefore not enough on its own: a
// target links zahlen_http, which on Windows is also what brings ws2_32 along.
//
// It binds an ephemeral port on 127.0.0.1 and never on an interface, answers
// every request with "Connection: close" so a handler never has to think about
// a second request on the same socket, and runs one std::jthread per connection
// behind one acceptor. Construction cannot fail the way a test would want to
// hear about, so IsListening() is the check: a server that did not come up is a
// skipped test, not a failed one.
//
// Why it lives here rather than in the suite that needed it first: a fetcher can
// only be tested against routes whose answers are known in advance, and a second
// test suite that needs those answers would otherwise write a second server and
// a second idea of HTTP/1.1. The routes are the behaviours HTTP.hpp promises,
// spelled out as answers.
//
// Routes are fixed and small, each one standing for a behaviour the API
// promises: /ok and /binary for bodies, /notfound and /servererror for the rule
// that a status is data, /redirect and /loop and /tofile for redirect handling,
// /head for the one method that is an option rather than a verb, /spacing for
// header parsing, /gzip for the decoded body, /slow for a peer that never
// answers, and /echo for what the client actually sent. Anything else is a 404.

#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ZHLN::HTTP {

/// Not held const at the call sites, which looks like an omission and is not:
/// the handlers this spawns write its request counter and its handler list, and
/// a const object whose own threads modify it is undefined behaviour however
/// harmless it looks in practice.
class LoopbackServer {
  public:
    /// What /gzip sends: a real gzip stream of the twelve bytes "decoded body",
    /// header and CRC included, so a test of that route is about libcurl decoding
    /// a document and not about this server compressing one. Public because the
    /// Content-Length it arrives with is this array's size, and a test asserts
    /// that the two disagree with the decoded body.
    static constexpr std::array<uint8_t, 32> kGzipOfDecodedBody {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x4B,
                                                                 0x49, 0x4D, 0xCE, 0x4F, 0x49, 0x4D, 0x51, 0x48, 0xCA, 0x4F, 0xA9,
                                                                 0x04, 0x00, 0x81, 0x21, 0x6F, 0xED, 0x0C, 0x00, 0x00, 0x00};

    LoopbackServer();
    ~LoopbackServer();

    LoopbackServer(const LoopbackServer&)            = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] auto IsListening() const noexcept -> bool {
        return _listener != kNoListener;
    }

    /// The URL of a route on this server, port included.
    [[nodiscard]] auto Url(std::string_view path) const -> std::string;

    /// Connections accepted so far, wake-up connection included. The smuggling
    /// test reads it before and after a refused request: what the client declined
    /// to send must not have reached anyone.
    [[nodiscard]] auto Requests() const noexcept -> uint64_t {
        return _requests.load();
    }

  private:
    /// What _listener holds when there is no socket: -1, which is a bad fd on
    /// POSIX and INVALID_SOCKET on Windows read as a signed integer of the same
    /// width. The constructor in HTTPServer.cpp asserts the two agree at compile
    /// time, which is what lets IsListening() above be one spelling on both.
    static constexpr std::intptr_t kNoListener = -1;

    void AcceptLoop();

    /// Reads one request, answers it, closes. A handler owns its socket from
    /// here on.
    void Handle(std::intptr_t connection);

    static void SendAll(std::intptr_t connection, const std::string& answer);

    std::string Route(std::string_view requestLine, std::span<const std::string> headerLines, const std::string& body) const;

    using HeaderPair = std::pair<std::string_view, std::string_view>;

    /// An initializer_list rather than a span: a dynamic-extent span cannot be
    /// built from a braced list (C++20 removed that constructor, because the list
    /// it would borrow from dies at the end of the full expression), and every
    /// call site is a literal list of pairs.
    static std::string Answer(int status, std::string_view reason, std::initializer_list<HeaderPair> headers, std::string_view body);

    std::intptr_t             _listener = kNoListener;
    uint16_t                  _port     = 0;
    std::atomic<bool>         _stop {false};
    std::atomic<uint64_t>     _requests {0};
    std::jthread              _acceptor;
    std::mutex                _handlersMutex;
    std::vector<std::jthread> _handlers;
};

} // namespace ZHLN::HTTP
