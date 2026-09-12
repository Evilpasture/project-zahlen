// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/HTTP/HTTP.hpp
//
// Fetching things over HTTP: a synchronous client over libcurl's easy
// interface. This is an extra, not core -- talking to a network means linking
// libcurl (and whatever TLS backend it was built against), which the engine
// needs nothing from, so the whole directory is optional. extras/HTTP/CMakeLists.txt
// looks for libcurl and, when it is not installed, warns and defines no target
// at all; consumers guard on `if(TARGET zahlen_http)` exactly the way they
// guard on zahlen_svg.
//
// curl/curl.h is never included from this header, and nothing that includes
// this header needs libcurl's include path: the C types, the option constants
// and the transfer codes all live in HTTP.cpp, which is also what keeps a
// libcurl upgrade from recompiling the world.
//
// What is an error and what is not:
//
//   HTTPError is for the transfer -- a host that did not resolve, a socket that
//   did not connect, a timeout, a redirect loop, a TLS handshake. An HTTP
//   status is not a transfer failure, so a 404 and a 500 come back as an
//   engaged Response with that statusCode and whatever body the server sent.
//   Callers that want "only 2xx" write that check once, where they know what
//   they are fetching; this layer does not guess it for them.
//
// Threading: Fetch is safe to call from several threads at once. Each call owns
// its easy handle, libcurl's one-time global initialisation happens behind a
// function-local static, and CURLOPT_NOSIGNAL is set -- without it curl's DNS
// timeout installs a process-wide SIGALRM handler, which is a race against
// every other thread in the engine.
//
// TLS verification is always on and is not a knob here. A fetch that needs it
// off is a fetch that has decided it does not care what it is talking to, and
// that decision does not belong in an asset loader's defaults.
//
// Bodies arrive decoded: libcurl is asked for every content encoding it was
// built with and decompresses transparently, so Response::body is the document
// and not the wire bytes. That is also why Content-Length in
// Response::headers can disagree with body.size().

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::HTTP {

/// Hard ceiling on a response body, in bytes (256 MiB). Not a libcurl limit --
/// a guard, because curl hands the body to a callback that appends to a
/// std::vector and a URL that turns out to be a 40 GiB file would otherwise ask
/// for exactly that. Going over it aborts the transfer as TransferFailed.
inline constexpr uint64_t kMaxBodyBytes = 268435456ULL;

/// How many redirects a request may follow before libcurl is told to stop,
/// which arrives as HTTPError::TooManyRedirects.
inline constexpr uint32_t kMaxRedirects = 20;

enum class HTTPError : uint8_t {
    ClientInitFailed   ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize CURL easy session"> {}) = 1,
    ConnectionFailed   ZHLN_ANNOTATION(ZHLN::Description<"Failed to connect to host"> {}),
    Timeout            ZHLN_ANNOTATION(ZHLN::Description<"HTTP request timed out"> {}),
    TransferFailed     ZHLN_ANNOTATION(ZHLN::Description<"HTTP data transfer error"> {}),
    InvalidUrl         ZHLN_ANNOTATION(ZHLN::Description<"Supplied URL is invalid or malformed"> {}),
    TooManyRedirects   ZHLN_ANNOTATION(ZHLN::Description<"Exceeded maximum redirect limit"> {}),
    SslHandshakeFailed ZHLN_ANNOTATION(ZHLN::Description<"TLS/SSL certificate or handshake error"> {}),
    InternalError      ZHLN_ANNOTATION(ZHLN::Description<"Internal HTTP client error"> {})
};

/// One header line, split at the first colon. Names arrive exactly as the peer
/// sent them, so lookups against them want a case-insensitive compare: HTTP
/// field names are case-insensitive and this layer does not fold them, because
/// folding would lose what the server actually said.
struct Header {
    std::string name;
    std::string value;
};

/// What to ask for. The defaults are a plain GET with a 30 second budget that
/// follows redirects, which is the whole of the common case.
///
/// A method that carries a body (POST, PUT, PATCH) gets a Content-Length even
/// when @p body is empty, because a request line that promises a body and sends
/// no length leaves the server waiting for one.
///
/// @p timeoutSeconds is the budget for the whole transfer, not for the connect;
/// 0 means no limit, which is how libcurl reads it. @p method is the method for
/// every request in a redirect chain: an explicit verb stays explicit, so a 303
/// after a POST is answered with another POST rather than with the GET a browser
/// would send. Leaving followRedirects off is how a caller decides that itself.
///
/// A URL, method or header carrying CR, LF or NUL is refused as
/// HTTPError::InvalidUrl before libcurl sees it. That is not pedantry: libcurl
/// sends what it is handed, so a newline inside a header value is a second
/// header on the wire and a newline inside the method is a second request line
/// -- request smuggling, opened by a string the caller built out of something it
/// did not write itself. The method and every header name must also be an RFC
/// 9110 token.
struct Request {
    std::string          url;
    std::string          method          = "GET";
    std::vector<Header>  headers         = {};
    std::vector<uint8_t> body            = {};
    uint32_t             timeoutSeconds  = 30;
    bool                 followRedirects = true;
};

/// What came back. Engaged whenever the transfer completed, so statusCode is
/// the thing to branch on: 404 is a Response, not an HTTPError.
struct Response {
    int32_t              statusCode = 0;
    std::vector<Header>  headers;
    std::vector<uint8_t> body;

    /// The body as text, without a copy. The view borrows from @p body, so it
    /// dies with the Response; a body that is not valid UTF-8 is still returned
    /// as-is, because "what the server sent" is the answer this layer owes.
    [[nodiscard]] auto Text() const noexcept -> std::string_view {
        return {reinterpret_cast<const char*>(body.data()), body.size()};
    }
};

/// Runs @p request to completion on the calling thread and returns the
/// response, or the HTTPError that stopped the transfer. Blocking: an engine
/// that must not stall a frame calls this from a worker and keeps the
/// std::expected, which is plain data and moves across threads freely.
[[nodiscard]] auto Fetch(const Request& request) noexcept -> std::expected<Response, Error>;

/// A GET with nothing else set.
[[nodiscard]] auto Get(std::string_view url, uint32_t timeoutSeconds = 30) noexcept -> std::expected<Response, Error>;

/// A POST of @p body with a Content-Type, which is the shape of every JSON API
/// call. @p body is text in, bytes on the wire: nothing is encoded or escaped
/// here, so serialising a document is the caller's job (extras/json).
[[nodiscard]] auto Post(std::string_view url, std::string_view body, std::string_view contentType = "application/json", uint32_t timeoutSeconds = 30) noexcept
    -> std::expected<Response, Error>;

} // namespace ZHLN::HTTP
