// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/HTTP/HTTP.cpp
//
// The whole of the libcurl dependency lives in this translation unit: HTTP.hpp
// names no curl type, no CURLOPT and no CURLcode, so nothing that includes it
// needs libcurl's include path and nothing outside this file is recompiled when
// libcurl is upgraded.

#include <HTTP/HTTP.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <curl/curl.h>
#include <optional>
#include <span>
#include <string>
#include <utility>

// What this file is written against: the option names it sets (all of them
// decades old except the two _STR protocol options, which are guarded on their
// own below), CURLE_PEER_FAILED_VERIFICATION (7.62, where the certificate
// failure stopped being spelled as a CA-bundle problem) and
// CURLE_WEIRD_SERVER_REPLY (7.69, where code 8 stopped being FTP-specific and
// got a name that fits HTTP). 7.69 is the older of the two constraints that
// MapCURLError needs; anything newer is not referenced, and a code this build
// was not written against arrives in that switch's default. The CMake side
// rejects older libcurl at configure time; this is the same floor for a build
// that reached the compiler some other way.
#if !defined(LIBCURL_VERSION_NUM) || (LIBCURL_VERSION_NUM < 0x074500)
#error "extras/HTTP needs libcurl >= 7.69: CURLE_WEIRD_SERVER_REPLY and CURLE_PEER_FAILED_VERIFICATION by name."
#endif

namespace ZHLN::HTTP {

namespace {

// ---------------------------------------------------------------------------
// Everything between here and Fetch speaks libcurl and nothing else: plain
// types in, plain types out, no ZHLN::Error and no std::expected. That split is
// deliberate. It is the part that has to be right about curl's ABI -- the LONG
// options that take a long and not an int through a variadic call, the
// callbacks that must consume every byte handed to them, the handle that must
// be cleaned up on every path -- and it is the part that can be lifted into a
// harness and compiled against libcurl alone, without the engine's C++26
// reflection build.
// ---------------------------------------------------------------------------

/// The result of the one-time curl_global_init, kept for the process.
///
/// libcurl asks for that call exactly once and documents it as not thread-safe,
/// so it happens behind a function-local static: the first Fetch to arrive runs
/// it and the rest wait on the initialisation guard the language provides. The
/// matching curl_global_cleanup runs from the same static's destructor at exit,
/// which is why nothing in here may be called from another translation unit's
/// static destructor that outlives this one.
struct CURLRuntime {
    CURLcode init = CURLE_FAILED_INIT;

    CURLRuntime() noexcept: init(curl_global_init(CURL_GLOBAL_DEFAULT)) {
    }
    CURLRuntime(const CURLRuntime&)            = delete;
    CURLRuntime& operator=(const CURLRuntime&) = delete;

    ~CURLRuntime() {
        curl_global_cleanup();
    }
};

auto Runtime() noexcept -> const CURLRuntime& {
    static const CURLRuntime runtime;
    return runtime;
}

/// An easy handle per transfer, because a handle is not reentrant and Fetch is
/// callable from several threads. Cleaned up on every path, including the ones
/// that leave through an early return.
struct EasyHandle {
    CURL* handle = nullptr;

    explicit EasyHandle(CURL* easy) noexcept: handle(easy) {
    }
    EasyHandle(const EasyHandle&)            = delete;
    EasyHandle& operator=(const EasyHandle&) = delete;

    ~EasyHandle() {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

/// The list of "Name: value" strings handed to CURLOPT_HTTPHEADER.
struct HeaderList {
    curl_slist* list = nullptr;

    HeaderList()                             = default;
    HeaderList(const HeaderList&)            = delete;
    HeaderList& operator=(const HeaderList&) = delete;

    ~HeaderList() {
        if (list != nullptr) {
            curl_slist_free_all(list);
        }
    }
};

struct NativeRequest {
    std::string_view         url;
    std::string_view         method;
    std::span<const Header>  headers;
    std::span<const uint8_t> body;
    uint32_t                 timeoutSeconds  = 0;
    bool                     followRedirects = true;
};

struct NativeResponse {
    int32_t              statusCode = 0;
    std::vector<Header>  headers;
    std::vector<uint8_t> body;
};

/// What the two callbacks share: where to put things, and whether the body has
/// already gone over the cap (after which every further chunk is refused).
struct Transfer {
    NativeResponse& response;
    bool            overflow = false;
};

/// The body callback. Returning anything but size * count tells curl the write
/// failed and aborts the transfer with CURLE_WRITE_ERROR, which is how the
/// kMaxBodyBytes guard is enforced: the alternative is a std::vector that grows
/// until the process dies.
size_t WriteBody(char* data, size_t size, size_t count, void* userData) noexcept {
    auto&        transfer = *static_cast<Transfer*>(userData);
    const size_t bytes    = size * count;
    auto&        body     = transfer.response.body;

    if (body.size() + bytes > kMaxBodyBytes) {
        transfer.overflow = true;
        return 0;
    }

    const auto* const first = reinterpret_cast<const uint8_t*>(data);
    body.insert(body.end(), first, first + bytes);
    return bytes;
}

/// The header callback, called once per line: the status line, each field, and
/// the blank line that ends the block. Following redirects means several
/// blocks arrive in one transfer, and what the caller wants is the last one's
/// headers, so a status line clears what has been collected so far. That also
/// disposes of a 100-continue interim block and of a proxy's CONNECT headers.
size_t WriteHeader(char* data, size_t size, size_t count, void* userData) noexcept {
    auto&                  transfer = *static_cast<Transfer*>(userData);
    const std::string_view line(data, size * count);

    if (line.starts_with("HTTP/")) {
        transfer.response.headers.clear();
        return line.size();
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        // The terminating blank line, or anything else curl passes that is not
        // a field. Consumed either way -- see WriteBody on why the count has to
        // come back whole.
        return line.size();
    }

    Header header;
    header.name = std::string(line.substr(0, colon));

    // RFC 9110 puts optional whitespace either side of the colon and CRLF at
    // the end of the line; neither belongs in a value a caller compares
    // against. Names are kept exactly as the peer sent them, because field
    // names are case-insensitive and folding them here would lose what the
    // server actually said.
    auto       value = line.substr(colon + 1);
    const auto first = value.find_first_not_of(" \t");
    value            = (first == std::string_view::npos) ? std::string_view {} : value.substr(first);
    const auto last  = value.find_last_not_of(" \t\r\n");
    value            = (last == std::string_view::npos) ? std::string_view {} : value.substr(0, last + 1);
    header.value     = std::string(value);

    transfer.response.headers.push_back(std::move(header));
    return line.size();
}

/// libcurl's transfer codes, grouped onto the eight HTTPError reports.
///
/// Grouped by name, never by value: libcurl promises not to renumber CURLcode,
/// but a name says what it means and a value does not, and the switch is the
/// place that would have to be revisited if curl ever grew a failure worth its
/// own enumerator here. Codes newer than the 7.69 floor are deliberately not
/// named -- HTTP/3, QUIC, the proxy-specific ones -- and arrive in the default,
/// which is the honest bucket for "a failure this build was not written
/// against".
auto MapCURLError(CURLcode code) noexcept -> HTTPError {
    switch (code) {
        // A handle that could not be created, an option curl refused, or an
        // allocation that failed: the client itself, not the network.
        case CURLE_OK: // Perform maps failures only; CURLE_OK here is a caller bug
        case CURLE_FAILED_INIT:
        case CURLE_FUNCTION_NOT_FOUND:
        case CURLE_OUT_OF_MEMORY:
        case CURLE_BAD_FUNCTION_ARGUMENT:
        case CURLE_UNKNOWN_OPTION:
        case CURLE_SETOPT_OPTION_SYNTAX:
            return HTTPError::InternalError;

        // The URL could not be turned into a request at all.
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
        case CURLE_NOT_BUILT_IN:
            return HTTPError::InvalidURL;

        // Nothing was ever going to answer, or what answered was not speaking
        // HTTP.
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_REMOTE_ACCESS_DENIED:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_GOT_NOTHING:
            return HTTPError::ConnectionFailed;

        case CURLE_OPERATION_TIMEDOUT:
            return HTTPError::Timeout;

        case CURLE_TOO_MANY_REDIRECTS:
            return HTTPError::TooManyRedirects;

        // Everything TLS: the handshake, and every way a certificate can be
        // rejected. Kept apart from ConnectionFailed on purpose, because the
        // two have different fixes.
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        case CURLE_SSL_INVALIDCERTSTATUS:
        case CURLE_SSL_SHUTDOWN_FAILED:
        case CURLE_SSL_ENGINE_NOTFOUND:
        case CURLE_SSL_ENGINE_SETFAILED:
        case CURLE_SSL_ENGINE_INITFAILED:
            return HTTPError::SSLHandshakeFailed;

        default:
            return HTTPError::TransferFailed;
    }
}

/// RFC 9110's token characters: what a method and a field name may be made of.
auto IsTokenCharacter(char character) noexcept -> bool {
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) {
        return true;
    }
    return std::string_view("!#$%&'*+-.^_`|~").find(character) != std::string_view::npos;
}

auto IsToken(std::string_view text) noexcept -> bool {
    if (text.empty()) {
        return false;
    }
    for (const char character: text) {
        if (!IsTokenCharacter(character)) {
            return false;
        }
    }
    return true;
}

/// Whether a string would break out of the field it is placed in. A
/// std::string_view can hold a NUL, and a NUL in a URL or a header is a
/// truncated one once it reaches curl as a C string.
auto CarriesLineBreak(std::string_view text) noexcept -> bool {
    return text.find_first_of("\r\n") != std::string_view::npos || text.find('\0') != std::string_view::npos;
}

/// Why a request cannot be put on the wire at all, or nothing when it can.
///
/// Checked before libcurl sees any of it, and before a handle exists for it,
/// because libcurl sends what it is handed: a CR or LF inside a header value
/// reaches the wire as a second header, and one inside the method or the URL as
/// a second request line. That is request smuggling, the door is opened by a
/// string the caller built out of something it did not write itself, and no
/// version of libcurl closes it for us.
///
/// The URL is answered with InvalidURL and the method and headers with
/// MalformedRequest, so "the thing you are fetching from is wrong" and "the
/// thing you put in the request is wrong" stay two different answers. @p detail
/// names the field, which is the part an enumerator cannot carry.
auto RequestFault(const NativeRequest& request, std::string& detail) noexcept -> std::optional<HTTPError> {
    if (request.url.empty()) {
        detail = "URL is empty";
        return HTTPError::InvalidURL;
    }
    if (CarriesLineBreak(request.url)) {
        detail = "URL carries a CR, LF or NUL";
        return HTTPError::InvalidURL;
    }
    if (!IsToken(request.method)) {
        detail = "method \"" + std::string(request.method) + "\" is not an HTTP token";
        return HTTPError::MalformedRequest;
    }
    for (const Header& header: request.headers) {
        if (!IsToken(header.name)) {
            detail = "header name \"" + header.name + "\" is not an HTTP token";
            return HTTPError::MalformedRequest;
        }
        if (CarriesLineBreak(header.value)) {
            detail = "value of header \"" + header.name + "\" carries a CR, LF or NUL";
            return HTTPError::MalformedRequest;
        }
    }
    return std::nullopt;
}

/// Runs one transfer on a handle of its own.
///
/// CURLE_OK means the transfer completed, whatever the server said about it: a
/// 404 is a completed transfer with statusCode 404. Anything else leaves
/// libcurl's own description of what went wrong in @p detail, which is the part
/// HTTPError cannot express -- the host that did not resolve, the certificate
/// that did not verify.
auto Perform(const NativeRequest& request, NativeResponse& response, std::string& detail) noexcept -> CURLcode {
    // The request is expected to have been through RequestFault, which Fetch does
    // before it gets here: what follows builds a transfer, and building one out
    // of a string carrying a newline is what puts a second request on the wire.
    const CURLRuntime& runtime = Runtime();
    if (runtime.init != CURLE_OK) {
        detail = curl_easy_strerror(runtime.init);
        return runtime.init;
    }

    CURL* const easy = curl_easy_init();
    if (easy == nullptr) {
        detail = "curl_easy_init returned no handle";
        return CURLE_FAILED_INIT;
    }
    const EasyHandle easyGuard(easy);

    // curl wants NUL-terminated strings for the URL and the method and the
    // request hands over views, so those two get a copy. The body does not:
    // CURLOPT_POSTFIELDS borrows the caller's bytes for the length of the
    // transfer, and the request outlives it.
    const std::string urlZ(request.url);
    const std::string methodZ(request.method);

    Transfer                              transfer {response, false};
    std::array<char, CURL_ERROR_SIZE + 1> errorBuffer {};

    // curl_easy_setopt reports a rejected option as a return code rather than
    // failing loudly, and an option that silently did not take is worse than one
    // that failed: CURLOPT_NOSIGNAL not being set means a SIGALRM handler
    // installed under a threaded engine. So every set is recorded and the first
    // failure is the one reported.
    CURLcode   setup = CURLE_OK;
    const auto apply = [&setup](CURLcode result) noexcept {
        if (setup == CURLE_OK) {
            setup = result;
        }
    };

    // The LONG options take a long through a variadic call, which is why every
    // one of them is spelled static_cast<long>: an int reaches curl as the wrong
    // width on LP64 and the option is set from half a value.
    apply(curl_easy_setopt(easy, CURLOPT_URL, urlZ.c_str()));
    apply(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
    apply(curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSeconds)));
    apply(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &WriteBody));
    apply(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer));
    apply(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &WriteHeader));
    apply(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer));
    apply(curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errorBuffer.data()));

    // Verification is on, explicitly, so that a libcurl built with different
    // defaults cannot change what this client does. There is no knob: see
    // HTTP.hpp.
    apply(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L));
    apply(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L));

    // Every content encoding libcurl was built with, decoded on the way in, so
    // Response::body is the document rather than the wire bytes.
    apply(curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, ""));

#if LIBCURL_VERSION_NUM >= 0x075502
    // Protocols this client will speak, for the URL as given and for anything it
    // is redirected to. Left unrestricted, a fetched manifest can point curl at
    // file:// or gopher:// and get an answer. 7.85.2 replaced the CURLPROTO_*
    // bitmask with a string, and the bitmask spelling below is what an older
    // libcurl has; both mean the same thing here. The name lives in the branch
    // that uses it, so the other branch has nothing unused to warn about.
    constexpr const char* allowedProtocols = "http,https";
    apply(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, allowedProtocols));
    apply(curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, allowedProtocols));
#else
    apply(curl_easy_setopt(easy, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS)));
    apply(curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS)));
#endif

    if (request.followRedirects) {
        apply(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L));
        apply(curl_easy_setopt(easy, CURLOPT_MAXREDIRS, static_cast<long>(kMaxRedirects)));
    }

    // HEAD is the one method that is an option rather than a verb: CUSTOMREQUEST
    // would send the word and then wait for a body that HEAD promises not to
    // send, which arrives as a timeout rather than as a response.
    if (methodZ == "HEAD") {
        apply(curl_easy_setopt(easy, CURLOPT_NOBODY, 1L));
    } else {
        apply(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, methodZ.c_str()));
    }

    // A body, or a method that says there is one. POSTFIELDSIZE 0 is what makes
    // curl send "Content-Length: 0": a request line that promises a body and
    // names no length leaves the server reading. CURLOPT_POST sets the request
    // up as one that carries a body while CUSTOMREQUEST above still decides the
    // verb on the wire, which is how a PUT or a PATCH gets its payload.
    const bool carriesBody = !request.body.empty() || methodZ == "POST" || methodZ == "PUT" || methodZ == "PATCH";
    if (carriesBody) {
        static constexpr char kNoBody[] = "";
        const char* const     bytes     = request.body.empty() ? kNoBody : reinterpret_cast<const char*>(request.body.data());
        apply(curl_easy_setopt(easy, CURLOPT_POST, 1L));
        apply(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, bytes));
        apply(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size())));
    }

    HeaderList headerList;
    for (const Header& header: request.headers) {
        const std::string line = header.name + ": " + header.value;
        // On failure curl_slist_append returns NULL and leaves the list it was
        // given alone, so the guard still owns something worth freeing -- which
        // is why the result goes to a local rather than straight into the list.
        curl_slist* const appended = curl_slist_append(headerList.list, line.c_str());
        if (appended == nullptr) {
            detail = "curl_slist_append could not allocate a header line";
            return CURLE_OUT_OF_MEMORY;
        }
        headerList.list = appended;
    }
    if (headerList.list != nullptr) {
        apply(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList.list));
    }

    if (setup != CURLE_OK) {
        detail = curl_easy_strerror(setup);
        return setup;
    }

    const CURLcode performed = curl_easy_perform(easy);

    // Checked before the transfer's own code, because refusing a chunk is what
    // aborts the transfer: curl reports CURLE_WRITE_ERROR and its text for that
    // says only that the destination refused the data. Why it refused is worth
    // more than the code.
    if (transfer.overflow) {
        detail = "response body exceeded " + std::to_string(kMaxBodyBytes) + " bytes";
        return CURLE_WRITE_ERROR;
    }

    if (performed != CURLE_OK) {
        // ERRORBUFFER carries what the code alone cannot; when curl left it
        // empty, its own text for the code is still better than nothing.
        detail = (errorBuffer[0] != '\0') ? std::string(errorBuffer.data()) : std::string(curl_easy_strerror(performed));
        return performed;
    }
    long           status = 0;
    const CURLcode info   = curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (info != CURLE_OK) {
        detail = curl_easy_strerror(info);
        return info;
    }
    response.statusCode = static_cast<int32_t>(status);
    return CURLE_OK;
}

/// HTTPError says which kind of thing went wrong; the detail says which host,
/// which field, which certificate. That text has nowhere to live in a
/// std::expected<Response, Error>, so it goes to the log at Verbose: there for
/// whoever is debugging a fetch, silent by default.
void LogFailure(const Request& request, HTTPError failure, const std::string& detail) {
    ZHLN::Log<ZHLN::LogChannel::StdErr, ZHLN::LogLevel::Verbose>("[HTTP] {} {} failed: {} ({})", request.method, request.url, ToString(failure), detail);
}

// ---------------------------------------------------------------------------

} // namespace

auto Fetch(const Request& request) noexcept -> std::expected<Response, Error> {
    const NativeRequest native {
        .url             = request.url,
        .method          = request.method,
        .headers         = request.headers,
        .body            = request.body,
        .timeoutSeconds  = request.timeoutSeconds,
        .followRedirects = request.followRedirects,
    };

    NativeResponse transferred;
    std::string    detail;

    // Refused before a handle exists for it, and before anything is sent.
    if (const auto fault = RequestFault(native, detail)) {
        LogFailure(request, *fault, detail);
        return std::unexpected(*fault);
    }

    const CURLcode code = Perform(native, transferred, detail);
    if (code != CURLE_OK) {
        const auto failure = MapCURLError(code);
        LogFailure(request, failure, detail);
        return std::unexpected(failure);
    }

    Response response;
    response.statusCode = transferred.statusCode;
    response.headers    = std::move(transferred.headers);
    response.body       = std::move(transferred.body);
    return response;
}

auto Get(std::string_view url, uint32_t timeoutSeconds) noexcept -> std::expected<Response, Error> {
    Request request;
    request.url            = url;
    request.method         = "GET";
    request.timeoutSeconds = timeoutSeconds;
    return Fetch(request);
}

auto Post(std::string_view url, std::string_view body, std::string_view contentType, uint32_t timeoutSeconds) noexcept -> std::expected<Response, Error> {
    Request request;
    request.url            = url;
    request.method         = "POST";
    request.timeoutSeconds = timeoutSeconds;

    const auto* const first = reinterpret_cast<const uint8_t*>(body.data());
    request.body.assign(first, first + body.size());
    if (!contentType.empty()) {
        request.headers.push_back(Header {.name = "Content-Type", .value = std::string(contentType)});
    }
    return Fetch(request);
}

} // namespace ZHLN::HTTP
