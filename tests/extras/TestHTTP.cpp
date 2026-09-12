// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestHTTP.cpp
//
// HTTP is an optional layer (extras/HTTP) with an optional third-party library
// behind it, so this suite is built only when extras/HTTP/CMakeLists.txt found
// libcurl and defined zahlen_http. No libcurl, no target, no test: see
// tests/extras/CMakeLists.txt.
//
// The suite talks to a server of its own, on 127.0.0.1, on an ephemeral port,
// in this process. Nothing here reaches a network, so the answers do not depend
// on a CI runner's egress rules, on a third-party host staying up, or on a
// certificate authority being reachable -- and a redirect loop, a silent server
// and a 404 are all things a real host would have to be persuaded to do.
//
// The server is not in this file, and not in tests/helpers either: it is
// extras/HTTP/HTTPServer.hpp, part of the extra. It is an assertion instrument
// rather than a fixture -- /echo answers with the request it received, which is
// the only way to see the half of this API that error paths cannot show: the
// method on the request line, the Content-Length a bodiless POST still has to
// carry, the caller's headers, and what libcurl was asked to accept -- and a
// second suite that needs those answers should not have to write a second idea
// of HTTP/1.1 to get them.
//
// Not covered here, and why:
//
//   * kMaxBodyBytes. Reaching a 256 MiB ceiling means moving 256 MiB over
//     loopback in a suite labelled Fast. The guard is four lines in the write
//     callback and refusing a chunk is how it fires.
//   * TLS. A handshake failure needs a certificate the peer rejects, which means
//     either shipping one or trusting a host on the internet. Both are worse
//     than the assertion is worth; the mapping from curl's TLS codes to
//     SSLHandshakeFailed is a switch that reads as what it is.
//   * The exact text libcurl puts in its error buffer. It is version-specific
//     and it goes to the log, not to the caller.

#include "TestsFramework.hpp"
#include <HTTP/HTTP.hpp>
#include <HTTP/HTTPServer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using ZHLN::HTTP::LoopbackServer;

auto Text(const ZHLN::HTTP::Response& response) -> std::string {
    return std::string(response.Text());
}

/// An exact-name search over the record as it arrived, which is what proves the
/// names were not folded on the way in: FindHeader finds a name case-insensitively,
/// and the vector still has to hold what the peer actually sent.
auto Find(const ZHLN::HTTP::Response& response, std::string_view name) -> const ZHLN::HTTP::Header* {
    for (const auto& header: response.headers) {
        if (header.name == name) {
            return &header;
        }
    }
    return nullptr;
}

auto Count(const ZHLN::HTTP::Response& response, std::string_view name) -> size_t {
    size_t found = 0;
    for (const auto& header: response.headers) {
        if (header.name == name) {
            ++found;
        }
    }
    return found;
}

/// The value by name, through the API's own case-insensitive lookup.
auto Value(const ZHLN::HTTP::Response& response, std::string_view name) -> std::string {
    return std::string(response.FindHeader(name).value_or(""));
}

/// What the client sent, as /echo reported it back.
struct Echo {
    std::string text;

    [[nodiscard]] auto RequestLine() const -> std::string {
        const auto end = text.find("\r\n");
        return (end == std::string::npos) ? text : text.substr(0, end);
    }

    [[nodiscard]] auto Has(std::string_view needle) const -> bool {
        return text.find(needle) != std::string::npos;
    }

    [[nodiscard]] auto Body() const -> std::string {
        const auto end = text.find("\r\n\r\n");
        return (end == std::string::npos) ? std::string {} : text.substr(end + 4);
    }
};

} // namespace

struct HTTPTestSuite {
    enum class HTTPTestError : uint8_t {
        ServerUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The suite's loopback server did not come up, so there was nothing to fetch."> {}) = 1,
        FetchFailed       ZHLN_ANNOTATION(ZHLN::Description<"extras/HTTP refused a request the suite expected it to carry."> {}),
    };

    struct Tests {
        // --- 1. The error enum is annotated, so a failure can be reported ---
        std::expected<void, ZHLN::Error> the_error_enum_carries_its_descriptions() {
            using ZHLN::HTTP::HTTPError;

            // ToString reads the ZHLN_ANNOTATION description, which is what a
            // caller puts in a log line. An enumerator that lost its annotation
            // would come back as the name or as nothing.
            ZHLN::Test::ExpectEq(std::string(ZHLN::ToString(HTTPError::Timeout)), std::string("HTTP request timed out"));
            ZHLN::Test::ExpectEq(std::string(ZHLN::ToString(HTTPError::TooManyRedirects)), std::string("Exceeded maximum redirect limit"));
            ZHLN::Test::ExpectEq(std::string(ZHLN::ToString(HTTPError::MalformedRequest)), std::string("Request method or header is malformed"));
            ZHLN::Test::ExpectFalse(ZHLN::ToString(HTTPError::SSLHandshakeFailed).empty());

            // ZHLN::Error rejects a zero value, so the enumerators have to start
            // at one for the std::expected<Response, Error> contract to hold.
            ZHLN::Test::ExpectEq(static_cast<int>(HTTPError::ClientInitFailed), 1);
            return {};
        }

        // --- 2. A plain GET: status, headers, body ---
        std::expected<void, ZHLN::Error> a_plain_get_returns_status_headers_and_body() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            auto response = ZHLN::HTTP::Get(server.Url("/ok"));
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                ZHLN::Println("    [HTTP] Get failed: {}", response.error().Message());
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            ZHLN::Test::ExpectEq(response->statusCode, 200);
            ZHLN::Test::ExpectEq(Text(*response), std::string("hello"));
            // Text() is a view over the body, not a copy of it.
            ZHLN::Test::ExpectEq(response->Text().size(), response->body.size());
            ZHLN::Test::ExpectEq(Value(*response, "X-Test"), std::string("alpha"));
            ZHLN::Test::ExpectEq(Value(*response, "Content-Type"), std::string("text/plain"));
            ZHLN::Test::ExpectTrue(Find(*response, "Content-Length") != nullptr);
            return {};
        }

        // --- 3. A body is bytes, not a string ---
        std::expected<void, ZHLN::Error> a_binary_body_survives_byte_for_byte() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            auto response = ZHLN::HTTP::Get(server.Url("/binary"));
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            // Every value 0..255, so a NUL in the middle would truncate a
            // client that went through a C string anywhere on the way.
            if (ZHLN::Test::ExpectEq(response->body.size(), size_t {256})) {
                bool exact = true;
                for (size_t i = 0; i < 256; ++i) {
                    exact = exact && response->body[i] == static_cast<uint8_t>(i);
                }
                ZHLN::Test::ExpectTrue(exact);
            }
            ZHLN::Test::ExpectEq(response->Text().size(), size_t {256});
            return {};
        }

        // --- 4. The rule the API is built on: a status is data ---
        std::expected<void, ZHLN::Error> an_http_error_status_is_a_response_not_a_failure() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            // 404 and 500 both come back engaged. A wrapper that turned them
            // into an Error would leave the caller no way to read the body a
            // server sent with them, which is where the explanation usually is.
            auto missing = ZHLN::HTTP::Get(server.Url("/notfound"));
            if (ZHLN::Test::ExpectTrue(missing.has_value())) {
                ZHLN::Test::ExpectEq(missing->statusCode, 404);
                ZHLN::Test::ExpectEq(Text(*missing), std::string("gone"));
                ZHLN::Test::ExpectEq(Value(*missing, "X-Test"), std::string("missing"));
            }

            auto broken = ZHLN::HTTP::Get(server.Url("/servererror"));
            if (ZHLN::Test::ExpectTrue(broken.has_value())) {
                ZHLN::Test::ExpectEq(broken->statusCode, 500);
                ZHLN::Test::ExpectEq(Text(*broken), std::string("boom"));
            }
            return {};
        }

        // --- 5. Post() sends the body and the content type it documents ---
        std::expected<void, ZHLN::Error> post_sends_its_body_and_content_type() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            constexpr std::string_view payload  = R"({"a":1})";
            auto                       response = ZHLN::HTTP::Post(server.Url("/echo"), payload);
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                ZHLN::Println("    [HTTP] Post failed: {}", response.error().Message());
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            const Echo echo {Text(*response)};
            ZHLN::Test::ExpectEq(echo.RequestLine(), std::string("POST /echo HTTP/1.1"));
            ZHLN::Test::ExpectTrue(echo.Has("Content-Type: application/json"));
            ZHLN::Test::ExpectTrue(echo.Has(std::format("Content-Length: {}", payload.size())));
            ZHLN::Test::ExpectEq(echo.Body(), std::string(payload));

            // A content type is a header like any other, so the caller's own
            // replaces the default rather than sitting beside it.
            auto form = ZHLN::HTTP::Post(server.Url("/echo"), "a=1&b=2", "application/x-www-form-urlencoded");
            if (ZHLN::Test::ExpectTrue(form.has_value())) {
                const Echo formEcho {Text(*form)};
                ZHLN::Test::ExpectTrue(formEcho.Has("Content-Type: application/x-www-form-urlencoded"));
                ZHLN::Test::ExpectEq(formEcho.Body(), std::string("a=1&b=2"));
            }
            return {};
        }

        // --- 6. A POST with nothing to send still says how much that is ---
        std::expected<void, ZHLN::Error> a_post_with_no_body_still_names_its_length() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            ZHLN::HTTP::Request request;
            request.url    = server.Url("/echo");
            request.method = "POST";

            auto response = ZHLN::HTTP::Fetch(request);
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                ZHLN::Println("    [HTTP] empty POST failed: {}", response.error().Message());
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            // Without "Content-Length: 0" the server is left reading a body that
            // is never sent, and what the caller gets is a timeout on a request
            // that was already complete.
            const Echo echo {Text(*response)};
            ZHLN::Test::ExpectEq(echo.RequestLine(), std::string("POST /echo HTTP/1.1"));
            ZHLN::Test::ExpectTrue(echo.Has("Content-Length: 0"));
            ZHLN::Test::ExpectTrue(echo.Body().empty());
            return {};
        }

        // --- 7. The method reaches the wire as it was written ---
        std::expected<void, ZHLN::Error> the_method_reaches_the_wire_verbatim() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            // A body with a verb that is not POST: libcurl's POSTFIELDS sets the
            // request up to carry a payload while CUSTOMREQUEST decides the word
            // on the request line, which is what a PUT or a PATCH needs.
            ZHLN::HTTP::Request put;
            put.url          = server.Url("/echo");
            put.method       = "PUT";
            put.body         = {0x70, 0x75, 0x74}; // "put"
            auto putResponse = ZHLN::HTTP::Fetch(put);
            if (ZHLN::Test::ExpectTrue(putResponse.has_value())) {
                const Echo echo {Text(*putResponse)};
                ZHLN::Test::ExpectEq(echo.RequestLine(), std::string("PUT /echo HTTP/1.1"));
                ZHLN::Test::ExpectEq(echo.Body(), std::string("put"));
            }

            ZHLN::HTTP::Request remove;
            remove.url          = server.Url("/echo");
            remove.method       = "DELETE";
            auto deleteResponse = ZHLN::HTTP::Fetch(remove);
            if (ZHLN::Test::ExpectTrue(deleteResponse.has_value())) {
                const Echo echo {Text(*deleteResponse)};
                ZHLN::Test::ExpectEq(echo.RequestLine(), std::string("DELETE /echo HTTP/1.1"));
            }

            // A GET with a body is unusual but not the client's business to
            // refuse: some search APIs are built on it.
            ZHLN::HTTP::Request query;
            query.url          = server.Url("/echo");
            query.body         = {0x71}; // "q"
            auto queryResponse = ZHLN::HTTP::Fetch(query);
            if (ZHLN::Test::ExpectTrue(queryResponse.has_value())) {
                const Echo echo {Text(*queryResponse)};
                ZHLN::Test::ExpectEq(echo.RequestLine(), std::string("GET /echo HTTP/1.1"));
                ZHLN::Test::ExpectEq(echo.Body(), std::string("q"));
            }
            return {};
        }

        // --- 8. What the client asks for, as the server saw it ---
        std::expected<void, ZHLN::Error> caller_headers_reach_the_wire() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            ZHLN::HTTP::Request request;
            request.url     = server.Url("/echo");
            request.headers = {{.name = "X-Custom", .value = "value"}, {.name = "If-None-Match", .value = "\"abc123\""}};

            auto response = ZHLN::HTTP::Fetch(request);
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            const Echo echo {Text(*response)};
            ZHLN::Test::ExpectTrue(echo.Has("X-Custom: value"));
            // A quoted value survives whole, which is the shape an ETag has.
            ZHLN::Test::ExpectTrue(echo.Has("If-None-Match: \"abc123\""));
            // Bodies arrive decoded, which is only true because the client said
            // it could take them that way.
            ZHLN::Test::ExpectTrue(echo.Has("Accept-Encoding:"));
            return {};
        }

        // --- 9. Response headers keep their shape ---
        std::expected<void, ZHLN::Error> response_headers_keep_duplicates_and_lose_padding() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            auto response = ZHLN::HTTP::Get(server.Url("/spacing"));
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            // RFC 9110 allows optional whitespace either side of the colon, and
            // none of it is part of the value.
            ZHLN::Test::ExpectEq(Value(*response, "X-Space"), std::string("padded"));
            // Field names are case-insensitive, so the lookup folds -- while the
            // record keeps the casing the peer used, which is what a caller sees
            // when it walks headers itself.
            ZHLN::Test::ExpectEq(Value(*response, "x-SPACE"), std::string("padded"));
            ZHLN::Test::ExpectTrue(Find(*response, "x-SPACE") == nullptr);
            ZHLN::Test::ExpectTrue(Find(*response, "X-Space") != nullptr);
            ZHLN::Test::ExpectTrue(ZHLN::HTTP::SameFieldName("Content-Type", "content-TYPE"));
            ZHLN::Test::ExpectFalse(ZHLN::HTTP::SameFieldName("Content-Type", "Content-Types"));
            ZHLN::Test::ExpectFalse(response->FindHeader("X-Absent").has_value());
            // Set-Cookie arrives more than once in real answers; keeping only one
            // would be a decision the caller has to be able to make.
            ZHLN::Test::ExpectEq(Count(*response, "Set-Cookie"), size_t {2});
            ZHLN::Test::ExpectTrue(Find(*response, "X-Empty") != nullptr);
            ZHLN::Test::ExpectEq(Value(*response, "X-Empty"), std::string(""));
            return {};
        }

        // --- 10. A redirect is followed, and only the last answer's headers stay ---
        std::expected<void, ZHLN::Error> a_redirect_is_followed_and_only_the_last_block_survives() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            auto response = ZHLN::HTTP::Get(server.Url("/redirect"));
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                ZHLN::Println("    [HTTP] redirect failed: {}", response.error().Message());
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            ZHLN::Test::ExpectEq(response->statusCode, 200);
            ZHLN::Test::ExpectEq(Text(*response), std::string("hello"));
            // The 302's Location is from an answer that was not the last one.
            // Leaving it in the list would make the response look like both.
            ZHLN::Test::ExpectTrue(Find(*response, "Location") == nullptr);
            ZHLN::Test::ExpectEq(Value(*response, "X-Test"), std::string("alpha"));
            return {};
        }

        // --- 11. Redirects can be declined ---
        std::expected<void, ZHLN::Error> redirects_can_be_declined() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            ZHLN::HTTP::Request request;
            request.url             = server.Url("/redirect");
            request.followRedirects = false;

            auto response = ZHLN::HTTP::Fetch(request);
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            ZHLN::Test::ExpectEq(response->statusCode, 302);
            ZHLN::Test::ExpectEq(Value(*response, "Location"), std::string("/ok"));
            ZHLN::Test::ExpectTrue(response->body.empty());
            return {};
        }

        // --- 12. A loop ends as TooManyRedirects, not as a hang ---
        std::expected<void, ZHLN::Error> a_redirect_loop_ends_as_too_many_redirects() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            auto response = ZHLN::HTTP::Get(server.Url("/loop"));
            if (!ZHLN::Test::ExpectFalse(response.has_value())) {
                return {};
            }

            ZHLN::Test::ExpectTrue(response.error().Is(ZHLN::HTTP::HTTPError::TooManyRedirects));
            // The limit is finite and published, so a caller can reason about
            // how many requests a fetch can turn into.
            ZHLN::Test::ExpectTrue(ZHLN::HTTP::kMaxRedirects > 0 && ZHLN::HTTP::kMaxRedirects < 100);
            // One request, plus one per redirect the limit allows.
            ZHLN::Test::ExpectEq(server.Requests(), static_cast<uint64_t>(ZHLN::HTTP::kMaxRedirects) + 1);
            return {};
        }

        // --- 13. A redirect off HTTP is refused ---
        std::expected<void, ZHLN::Error> a_redirect_off_http_is_refused() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            // The server here is the hostile one: it answers a GET with a
            // redirect to file://. Which protocols a redirect may use is the
            // client's decision, and the client's answer is http and https.
            auto response = ZHLN::HTTP::Get(server.Url("/tofile"));
            if (!ZHLN::Test::ExpectFalse(response.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue(response.error().Is(ZHLN::HTTP::HTTPError::InvalidURL));
            return {};
        }

        // --- 14. HEAD is an option, not a verb ---
        std::expected<void, ZHLN::Error> head_gets_the_headers_and_no_body() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            ZHLN::HTTP::Request request;
            request.url    = server.Url("/head");
            request.method = "HEAD";

            auto response = ZHLN::HTTP::Fetch(request);
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                ZHLN::Println("    [HTTP] HEAD failed: {}", response.error().Message());
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            // The server names a Content-Length of 5 and sends no body, which is
            // correct for HEAD. A client that asked for the body by sending the
            // verb alone would sit here until its timeout instead.
            ZHLN::Test::ExpectEq(response->statusCode, 200);
            ZHLN::Test::ExpectTrue(response->body.empty());
            ZHLN::Test::ExpectEq(Value(*response, "X-Test"), std::string("head"));
            ZHLN::Test::ExpectEq(Value(*response, "Content-Length"), std::string("5"));
            return {};
        }

        // --- 15. A compressed body arrives as the document ---
        std::expected<void, ZHLN::Error> a_compressed_body_arrives_decoded() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            auto response = ZHLN::HTTP::Get(server.Url("/gzip"));
            if (!ZHLN::Test::ExpectTrue(response.has_value())) {
                return std::unexpected(HTTPTestError::FetchFailed);
            }

            ZHLN::Test::ExpectEq(response->statusCode, 200);
            ZHLN::Test::ExpectEq(Text(*response), std::string("decoded body"));
            // The header still says what came over the wire, which is why it can
            // disagree with the length of the body.
            ZHLN::Test::ExpectEq(Value(*response, "Content-Encoding"), std::string("gzip"));
            ZHLN::Test::ExpectEq(Value(*response, "Content-Length"), std::to_string(LoopbackServer::kGzipOfDecodedBody.size()));
            return {};
        }

        // --- 16. A peer that never answers ends as a timeout ---
        std::expected<void, ZHLN::Error> a_silent_server_ends_as_a_timeout() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            ZHLN::HTTP::Request request;
            request.url            = server.Url("/slow");
            request.timeoutSeconds = 1;

            const auto started  = std::chrono::steady_clock::now();
            auto       response = ZHLN::HTTP::Fetch(request);
            const auto elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

            if (!ZHLN::Test::ExpectFalse(response.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue(response.error().Is(ZHLN::HTTP::HTTPError::Timeout));
            // The budget is the caller's, so it is honoured rather than rounded
            // up to whatever libcurl felt like waiting.
            ZHLN::Test::ExpectTrue(elapsed < 5000);
            return {};
        }

        // --- 17. The failures that need no server at all ---
        std::expected<void, ZHLN::Error> the_failures_that_need_no_server() {
            // A URL libcurl cannot parse.
            auto malformed = ZHLN::HTTP::Get("http://exa mple.example/path");
            if (ZHLN::Test::ExpectFalse(malformed.has_value())) {
                ZHLN::Test::ExpectTrue(malformed.error().Is(ZHLN::HTTP::HTTPError::InvalidURL));
            }

            // An empty URL is refused before a handle is created for it.
            auto empty = ZHLN::HTTP::Get("");
            if (ZHLN::Test::ExpectFalse(empty.has_value())) {
                ZHLN::Test::ExpectTrue(empty.error().Is(ZHLN::HTTP::HTTPError::InvalidURL));
            }

            // A scheme this client does not speak, asked for directly rather than
            // through a redirect.
            auto file = ZHLN::HTTP::Get("file:///etc/passwd");
            if (ZHLN::Test::ExpectFalse(file.has_value())) {
                ZHLN::Test::ExpectTrue(file.error().Is(ZHLN::HTTP::HTTPError::InvalidURL));
            }

            // Nothing listening. Port 1 is not reachable without privileges, and
            // the refusal comes from the kernel rather than from a host.
            auto refused = ZHLN::HTTP::Get("http://127.0.0.1:1/", 5);
            if (ZHLN::Test::ExpectFalse(refused.has_value())) {
                ZHLN::Test::ExpectTrue(refused.error().Is(ZHLN::HTTP::HTTPError::ConnectionFailed));
            }

            // .invalid is reserved by RFC 6761: a resolver must not answer it, so
            // this is a name that does not exist rather than a name that might.
            auto unresolved = ZHLN::HTTP::Get("http://zahlen-http-suite.invalid/", 5);
            if (ZHLN::Test::ExpectFalse(unresolved.has_value())) {
                ZHLN::Test::ExpectTrue(unresolved.error().Is(ZHLN::HTTP::HTTPError::ConnectionFailed));
            }
            return {};
        }

        // --- 18. A request that would smuggle is refused ---
        std::expected<void, ZHLN::Error> a_request_that_would_smuggle_is_refused() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            const auto before = server.Requests();

            // libcurl sends what it is given, so each of these is a second header
            // or a second request line on the wire unless the client declines to
            // build the request at all. None of them reaches the server, and the
            // two kinds of fault stay two kinds of answer: a bad URL is
            // InvalidURL, a bad method or header is MalformedRequest.
            ZHLN::HTTP::Request value;
            value.url          = server.Url("/echo");
            value.headers      = {{.name = "X-Evil", .value = "a\r\nX-Injected: b"}};
            auto valueResponse = ZHLN::HTTP::Fetch(value);
            if (ZHLN::Test::ExpectFalse(valueResponse.has_value())) {
                ZHLN::Test::ExpectTrue(valueResponse.error().Is(ZHLN::HTTP::HTTPError::MalformedRequest));
            }

            ZHLN::HTTP::Request name;
            name.url          = server.Url("/echo");
            name.headers      = {{.name = "X-Evil\r\nX-Injected", .value = "b"}};
            auto nameResponse = ZHLN::HTTP::Fetch(name);
            if (ZHLN::Test::ExpectFalse(nameResponse.has_value())) {
                ZHLN::Test::ExpectTrue(nameResponse.error().Is(ZHLN::HTTP::HTTPError::MalformedRequest));
            }

            ZHLN::HTTP::Request method;
            method.url          = server.Url("/echo");
            method.method       = "GET /echo HTTP/1.1\r\nX-Injected: b\r\nGET";
            auto methodResponse = ZHLN::HTTP::Fetch(method);
            if (ZHLN::Test::ExpectFalse(methodResponse.has_value())) {
                ZHLN::Test::ExpectTrue(methodResponse.error().Is(ZHLN::HTTP::HTTPError::MalformedRequest));
            }

            ZHLN::HTTP::Request url;
            url.url          = server.Url("/echo HTTP/1.1\r\nX-Injected: b\r\n\r\nGET /echo");
            auto urlResponse = ZHLN::HTTP::Fetch(url);
            if (ZHLN::Test::ExpectFalse(urlResponse.has_value())) {
                ZHLN::Test::ExpectTrue(urlResponse.error().Is(ZHLN::HTTP::HTTPError::InvalidURL));
            }

            ZHLN::Test::ExpectEq(server.Requests(), before);

            // An ordinary punctuation-heavy header is not caught in the same net:
            // RFC 9110 tokens include most of the printable ASCII that is not a
            // separator, and quoted values are common enough to matter.
            ZHLN::HTTP::Request honest;
            honest.url          = server.Url("/echo");
            honest.method       = "PATCH";
            honest.headers      = {{.name = "X_Weird-Token.1", .value = "\"quoted, value; with=punctuation\""}};
            auto honestResponse = ZHLN::HTTP::Fetch(honest);
            if (ZHLN::Test::ExpectTrue(honestResponse.has_value())) {
                const Echo echo {Text(*honestResponse)};
                ZHLN::Test::ExpectEq(echo.RequestLine(), std::string("PATCH /echo HTTP/1.1"));
                ZHLN::Test::ExpectTrue(echo.Has("X_Weird-Token.1: \"quoted, value; with=punctuation\""));
            }
            ZHLN::Test::ExpectEq(server.Requests(), before + 1);
            return {};
        }

        // --- 19. Fetch is callable from several threads at once ---
        std::expected<void, ZHLN::Error> concurrent_fetches_stay_independent() {
            LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(HTTPTestError::ServerUnavailable);
            }

            // One easy handle per call, libcurl's global initialisation behind a
            // function-local static, and NOSIGNAL because a DNS timeout that
            // installs SIGALRM is a race against every other thread here.
            constexpr int        kThreads = 8;
            std::vector<int32_t> statuses(kThreads, 0);
            // Not std::vector<bool>: its proxy reference packs eight flags into a
            // byte, so threads writing different indices would race.
            std::vector<int32_t>      succeeded(kThreads, 0);
            std::vector<std::jthread> threads;
            threads.reserve(kThreads);

            const std::string url = server.Url("/ok");
            for (int i = 0; i < kThreads; ++i) {
                threads.emplace_back([&url, &statuses, &succeeded, i](std::stop_token) {
                    auto response = ZHLN::HTTP::Get(url);
                    succeeded[i]  = response.has_value() ? 1 : 0;
                    if (response.has_value()) {
                        statuses[i] = response->statusCode;
                    }
                });
            }
            threads.clear(); // joins every one

            bool allOk = true;
            for (int i = 0; i < kThreads; ++i) {
                allOk = allOk && succeeded[i] != 0 && statuses[i] == 200;
            }
            ZHLN::Test::ExpectTrue(allOk);
            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    // libcurl honours http_proxy and friends by default, and this suite's server
    // is on 127.0.0.1: a proxy in the environment would send every request here
    // somewhere else, and the suite would report failures that are about the
    // machine it ran on rather than about the client. Cleared for this process,
    // before anything is fetched. Setting a Windows variable to "" removes it,
    // which is what unsetenv does elsewhere.
    for (const char* name: {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    return ZHLN::Test::Runner::Run<HTTPTestSuite>();
}
