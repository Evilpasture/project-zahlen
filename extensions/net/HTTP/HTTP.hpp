// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A synchronous HTTP client over libcurl's easy interface. Optional: the
// directory links libcurl only when it is installed, and consumers guard on
// `if(TARGET zahlen_http)`.
//
// curl/curl.h is never included here -- the C types, option constants and
// transfer codes live in HTTP.cpp, which also keeps a libcurl upgrade from
// recompiling the world.
//
// Error is for the transfer (no DNS, no connect, timeout, redirect loop,
// TLS handshake). An HTTP status is not a transfer failure: 404 and 500 come
// back as an engaged Response with that statusCode and the server's body.
// MalformedRequest and InvalidURL refuse a request this client will not put on
// the wire; neither is retryable.
//
// Fetch is safe to call from several threads at once -- each call owns its easy
// handle, and CURLOPT_NOSIGNAL keeps curl's DNS timeout from installing a
// process-wide SIGALRM handler.
//
// TLS verification is always on and is not a knob here.
//
// Bodies arrive decoded: libcurl is asked for every content encoding it was
// built with and decompresses transparently, so Response::body is the document
// and not the wire bytes. That is also why Content-Length in
// Response::headers can disagree with body.size().

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::HTTP {

// Hard ceiling on a response body, in bytes (256 MiB). Not a libcurl limit --
// a guard, because curl hands the body to a callback that appends to a
// std::vector and a URL that turns out to be a 40 GiB file would otherwise ask
// for exactly that. Going over it aborts the transfer as TransferFailed.
inline constexpr uint64_t kMaxBodyBytes = 268435456ULL;

// How many redirects a request may follow before libcurl is told to stop,
// which arrives as Error::TooManyRedirects.
inline constexpr uint32_t kMaxRedirects = 20;

// What stopped a transfer, grouped by the one thing a caller usually wants to
// know: whether trying again could possibly help. ConnectionFailed and Timeout
// are the network's doing and may succeed next time; InvalidURL and
// MalformedRequest are the caller's, and fail identically until the request
// changes; the rest are the client's or the peer's, and repeating the same
// request repeats them.
enum class Error : uint8_t {
    ClientInitFailed   ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize CURL easy session"> {}) = 1,
    ConnectionFailed   ZHLN_ANNOTATION(ZHLN::Description<"Failed to connect to host"> {}),
    Timeout            ZHLN_ANNOTATION(ZHLN::Description<"HTTP request timed out"> {}),
    TransferFailed     ZHLN_ANNOTATION(ZHLN::Description<"HTTP data transfer error"> {}),
    InvalidURL         ZHLN_ANNOTATION(ZHLN::Description<"Supplied URL is invalid or malformed"> {}),
    MalformedRequest   ZHLN_ANNOTATION(ZHLN::Description<"Request method or header is malformed"> {}),
    TooManyRedirects   ZHLN_ANNOTATION(ZHLN::Description<"Exceeded maximum redirect limit"> {}),
    SSLHandshakeFailed ZHLN_ANNOTATION(ZHLN::Description<"TLS/SSL certificate or handshake error"> {}),
    InternalError      ZHLN_ANNOTATION(ZHLN::Description<"Internal HTTP client error"> {})
};

// One header line, split at the first colon. Names arrive exactly as the peer
// sent them: HTTP field names are case-insensitive, and folding them on the way
// in would lose what the server actually said. Lookups go through
// SameFieldName, or through Response::FindHeader, which uses it.
struct Header {
    std::string name;
    std::string value;
};

// Whether two HTTP field names are the same name. ASCII only, because a field
// name that is not ASCII is not one HTTP defines, and folding anything else is
// a locale question this layer has no business answering.
[[nodiscard]] inline auto SameFieldName(std::string_view lhs, std::string_view rhs) noexcept -> bool {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto a = std::tolower(static_cast<unsigned char>(lhs[i]));
        const auto b = std::tolower(static_cast<unsigned char>(rhs[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

// What to ask for. The defaults are a plain GET with a 30 second budget that
// follows redirects, which is the whole of the common case.
//
// A method that carries a body (POST, PUT, PATCH) gets a Content-Length even
// when @p body is empty, because a request line that promises a body and sends
// no length leaves the server waiting for one.
//
// @p timeoutSeconds is the budget for the whole transfer, not for the connect;
// 0 means no limit, which is how libcurl reads it. @p method is the method for
// every request in a redirect chain: an explicit verb stays explicit, so a 303
// after a POST is answered with another POST rather than with the GET a browser
// would send. Leaving followRedirects off is how a caller decides that itself.
//
// A URL, method or header carrying CR, LF or NUL is refused before libcurl sees
// it -- the URL as Error::InvalidURL, the method and the headers as
// Error::MalformedRequest. That is not pedantry: libcurl sends what it is
// handed, so a newline inside a header value is a second header on the wire and
// a newline inside the method is a second request line -- request smuggling,
// opened by a string the caller built out of something it did not write itself.
// The method and every header name must also be an RFC 9110 token.
struct Request {
    std::string          url;
    std::string          method          = "GET";
    std::vector<Header>  headers         = {};
    std::vector<uint8_t> body            = {};
    uint32_t             timeoutSeconds  = 30;
    bool                 followRedirects = true;
    // When non-null, libcurl polls this each progress tick and aborts with
    // CURLE_ABORTED_BY_CALLBACK (mapped to Error::TransferFailed) as soon
    // as it is true. The pointed-to atomic must outlive the Fetch call.
    const ZHLN::Atomic<bool>* cancelSignal = nullptr;
};

// What came back. Engaged whenever the transfer completed, so statusCode is
// the thing to branch on: 404 is a Response, not an Error.
struct Response {
    int32_t              statusCode = 0;
    std::vector<Header>  headers;
    std::vector<uint8_t> body;
    // Validators from a redirected response belong to its final resource,
    // not necessarily the requested URL. Consumers must not forward them
    // blindly when a redirect target can change.
    bool redirected = false;

    // The body as text, without a copy. The view borrows from @p body, so it
    // dies with the Response; a body that is not valid UTF-8 is still returned
    // as-is, because "what the server sent" is the answer this layer owes.
    [[nodiscard]] auto Text() const noexcept -> std::string_view {
        return {reinterpret_cast<const char*>(body.data()), body.size()};
    }

    // The value of the first header called @p name, compared the way HTTP says
    // to, or nothing if there is none. The view borrows from @p headers. A name
    // that arrives more than once -- Set-Cookie does -- is the caller's to walk
    // in @p headers, which is the whole record in the order it came.
    [[nodiscard]] auto FindHeader(std::string_view name) const noexcept -> std::optional<std::string_view> {
        for (const auto& header: headers) {
            if (SameFieldName(header.name, name)) {
                return header.value;
            }
        }
        return std::nullopt;
    }
};

// Runs @p request to completion on the calling thread and returns the
// response, or the Error that stopped the transfer. Blocking: an engine
// that must not stall a frame calls this from a worker and keeps the
// std::expected, which is plain data and moves across threads freely.
[[nodiscard]] auto Fetch(const Request& request) noexcept -> std::expected<Response, ErrorCode>;

// Rich fetch failure: the ErrorCode kind plus the transfer's own detail
// (host, certificate, curl text). Plain data -- no logger needed here.
struct FetchFailure {
    ErrorCode   code;
    std::string detail;
};

// Same transfer as Fetch, but a failure carries which host/field/cert
// stopped it, for the caller to log.
[[nodiscard]] auto FetchDetailed(const Request& request) noexcept -> std::expected<Response, FetchFailure>;

// A GET with nothing else set.
[[nodiscard]] auto Get(std::string_view url, uint32_t timeoutSeconds = 30) noexcept -> std::expected<Response, ErrorCode>;

// A POST of @p body with a Content-Type, which is the shape of every JSON API
// call. @p body is text in, bytes on the wire: nothing is encoded or escaped
// here, so serialising a document is the caller's job (plugins/json).
[[nodiscard]] auto Post(std::string_view url, std::string_view body, std::string_view contentType = "application/json", uint32_t timeoutSeconds = 30) noexcept
    -> std::expected<Response, ErrorCode>;

} // namespace ZHLN::HTTP
