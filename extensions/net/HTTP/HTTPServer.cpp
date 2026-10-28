// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/net/HTTP/HTTPServer.cpp
//
// The loopback fixture uses zahlen_socket for every socket operation. This
// translation unit owns the listener and the accepted connection values; the
// POSIX/Win32 details and Winsock lifetime stay inside the socket extension.

#include <HTTP/HTTPServer.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ZHLN::HTTP {

LoopbackServer::LoopbackServer() {
    auto listener = Net::OpenSocket(Net::SocketType::Stream, Net::SocketAddressFamily::IPv4);
    if (!listener) {
        return;
    }
    _listener = std::move(*listener);

    auto reuseAddress = _listener.SetReuseAddress(true);
    if (!reuseAddress) {
        _listener.Close();
        return;
    }

    auto bound = _listener.Bind(Net::SocketEndpoint::IPv4Loopback(0));
    if (!bound) {
        _listener.Close();
        return;
    }

    auto endpoint = _listener.LocalEndpoint();
    if (!endpoint) {
        _listener.Close();
        return;
    }
    _port = endpoint->port;

    auto listening = _listener.Listen(64);
    if (!listening) {
        _listener.Close();
        return;
    }

    auto nonblocking = _listener.SetNonBlocking(true);
    if (!nonblocking) {
        _listener.Close();
        return;
    }

    _acceptor = std::jthread([this](std::stop_token) { AcceptLoop(); });
}

LoopbackServer::~LoopbackServer() {
    _stop.store(true);

    // AcceptLoop polls with a short timeout, so shutdown does not need a raw
    // wake-up connection or a platform-specific shutdown call.
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
    _listener.Close();
}

auto LoopbackServer::Url(std::string_view path) const -> std::string {
    return std::format("http://127.0.0.1:{}{}", _port, path);
}

void LoopbackServer::SetResource(std::string path, Resource resource) {
    const std::lock_guard lock(_resourcesMutex);
    _resources.insert_or_assign(std::move(path), std::move(resource));
}

void LoopbackServer::AcceptLoop() {
    while (!_stop.load()) {
        auto readiness = _listener.WaitFor(true, false, std::chrono::milliseconds(50));
        if (!readiness) {
            if (_stop.load()) {
                return;
            }
            continue;
        }
        if (readiness->error || readiness->closed) {
            return;
        }
        if (!readiness->readable) {
            continue;
        }

        auto accepted = Net::AcceptSocket(_listener);
        if (!accepted) {
            if (_stop.load()) {
                return;
            }
            continue;
        }
        auto blocking = accepted->SetNonBlocking(false);
        if (!blocking) {
            accepted->Close();
            continue;
        }

        const std::lock_guard lock(_handlersMutex);
        _handlers.emplace_back(
            [this, connection = std::move(*accepted)](std::stop_token) mutable {
                Handle(std::move(connection));
            }
        );
    }
}

void LoopbackServer::Handle(Net::Socket connection) {
    _requests.fetch_add(1);

    std::string      received;
    uint8_t          buffer[8192];
    size_t           headerEnd = std::string::npos;
    while ((headerEnd = received.find("\r\n\r\n")) == std::string::npos) {
        auto count = connection.Receive(std::span<uint8_t>(buffer));
        if (!count || count->state != Net::SocketIOState::Complete || count->bytes == 0) {
            connection.Close();
            return;
        }
        received.append(reinterpret_cast<const char*>(buffer), count->bytes);
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
        auto count = connection.Receive(std::span<uint8_t>(buffer));
        if (!count || count->state != Net::SocketIOState::Complete || count->bytes == 0) {
            break;
        }
        body.append(reinterpret_cast<const char*>(buffer), count->bytes);
    }

    const std::string answer = Route(requestLine, std::span(lines).subspan(lines.empty() ? 0 : 1), body);
    SendAll(connection, answer);
    connection.Close();
}

void LoopbackServer::SendAll(Net::Socket& connection, const std::string_view answer) {
    size_t sent = 0;
    while (sent < answer.size()) {
        const auto bytes = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(answer.data() + sent), answer.size() - sent
        );
        auto result = connection.Send(bytes);
        if (!result) {
            return;
        }
        if (result->state == Net::SocketIOState::WouldBlock) {
            auto readiness = connection.WaitFor(false, true, std::chrono::seconds(5));
            if (!readiness || !readiness->writable) {
                return;
            }
            continue;
        }
        if (result->state != Net::SocketIOState::Complete || result->bytes == 0) {
            return;
        }
        sent += result->bytes;
    }
}

std::string LoopbackServer::Route(std::string_view requestLine, std::span<const std::string> headerLines, std::string_view body) {
    std::string path;
    {
        const auto first = requestLine.find(' ');
        const auto last  = (first == std::string_view::npos) ? first : requestLine.find(' ', first + 1);
        if (first != std::string_view::npos && last != std::string_view::npos) {
            path = std::string(requestLine.substr(first + 1, last - first - 1));
        }
    }

    std::optional<Resource> resource;
    {
        const std::lock_guard lock(_resourcesMutex);
        if (const auto it = _resources.find(path); it != _resources.end())
            resource = it->second;
    }
    if (resource) {
        const auto field = [&](std::string_view wanted) -> std::string_view {
            for (const auto& line: headerLines) {
                const auto colon = line.find(':');
                if (colon != wanted.size())
                    continue;
                bool same = true;
                for (size_t i = 0; i < colon; ++i) {
                    same &= static_cast<char>(std::tolower(static_cast<unsigned char>(line[i]))) == wanted[i];
                }
                if (!same)
                    continue;
                std::string_view value = std::string_view(line).substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                return value;
            }
            return {};
        };
        const auto etag     = field("if-none-match");
        const auto modified = field("if-modified-since");
        if (!etag.empty() || !modified.empty())
            _conditionalRequests.fetch_add(1);
        const auto weakTag = [](std::string_view tag) {
            if (tag.starts_with("W/"))
                tag.remove_prefix(2);
            return tag;
        };
        const bool unchanged = !etag.empty() ? (!resource->etag.empty() && weakTag(etag) == weakTag(resource->etag)) :
                                               (!modified.empty() && modified == resource->lastModified);
        const int  status    = resource->status == 200 && unchanged ? 304 : resource->status;
        if (status == 304)
            _notModifiedResponses.fetch_add(1);
        std::vector<HeaderPair> headers;
        if (status != 304 || !resource->omitValidatorsOn304) {
            if (!resource->etag.empty())
                headers.emplace_back("ETag", resource->etag);
            if (!resource->lastModified.empty())
                headers.emplace_back("Last-Modified", resource->lastModified);
        }
        if (!resource->cacheControl.empty())
            headers.emplace_back("Cache-Control", resource->cacheControl);
        if (!resource->age.empty())
            headers.emplace_back("Age", resource->age);
        if (!resource->vary.empty())
            headers.emplace_back("Vary", resource->vary);
        if (!resource->location.empty())
            headers.emplace_back("Location", resource->location);
        // Run outside the resource lock: tests can replace a resource or evict
        // a cached body exactly while its conditional response is in flight.
        if (resource->beforeReply)
            resource->beforeReply();
        return AnswerFields(
            status, status == 304 ? "Not Modified" : "Fixture", headers, status == 304 ? std::string_view {} : std::string_view(resource->body)
        );
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
    return AnswerFields(status, reason, std::span<const HeaderPair>(headers.begin(), headers.size()), body);
}

std::string LoopbackServer::AnswerFields(int status, std::string_view reason, std::span<const HeaderPair> headers, std::string_view body) {
    std::string answer = std::format("HTTP/1.1 {} {}\r\n", status, reason);
    for (const auto& [name, value]: headers) {
        answer += std::format("{}: {}\r\n", name, value);
    }
    if (status != 304)
        answer += std::format("Content-Length: {}\r\n", body.size());
    answer += "Connection: close\r\n\r\n";
    answer += body;
    return answer;
}

} // namespace ZHLN::HTTP
