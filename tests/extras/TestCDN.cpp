// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestCDN.cpp
//
// extensions/net/CDN is built over the remote-asset layer and libcurl, so this suite
// is built only when zahlen_cdn exists. Every assertion runs against the loopback
// server of extensions/net/HTTP (HTTPServer.hpp) on 127.0.0.1, so no test depends
// on a real CDN, a DNS record or a CI runner's egress rules. The base URL is the
// server's own origin, which is how the suite exercises the same configuration
// path a real deployment takes: a string handed in through CDNConfig.
//
// Not covered here: the libcurl error mapping (TestHTTP's ground), and the
// frame-thread path, which is AsyncAssetFetcher's (TestRemoteAsset).

#include "TestsFramework.hpp"
#include <CDN/CDN.hpp>
#include <HTTP/HTTPServer.hpp>
#include <RemoteAsset/URLResolver.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

// A validator for the loopback /ok route, whose body is exactly "hello".
auto IsHello(std::span<const uint8_t> bytes) noexcept -> bool {
    return bytes.size() == 5;
}

auto RejectAll(std::span<const uint8_t> /*bytes*/) noexcept -> bool {
    return false;
}

// The suite's scratch directory under the system temp, cleared at the start of
// each test. One per test so a failure in one cannot poison another.
auto ScratchDir(std::string_view name) -> std::filesystem::path {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("zhaln-test-cdn-{}", name);
    std::error_code             ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct CDNTestSuite {
    enum class CDNTestError : uint8_t {
        ServerUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The suite's loopback server did not come up, so there was nothing to fetch.">{}) = 1,
        ManagerRefused    ZHLN_ANNOTATION(ZHLN::Description<"CDNManager::Create refused a config the test expected to be usable.">{}),
    };

    struct Tests {
        // --- errors: the annotation is the message ---

        std::expected<void, ZHLN::ErrorCode> the_errors_are_annotated_enumerators_that_format_like_wire() {
            // An enumerator converts to ErrorCode and back without losing its identity.
            const ZHLN::ErrorCode code = ZHLN::CDN::CDNError::NotFound;
            ZHLN::Test::ExpectTrue(code.Is(ZHLN::CDN::CDNError::NotFound));
            ZHLN::Test::ExpectFalse(code.Is(ZHLN::CDN::CDNError::HTTPStatus));

            // Message() is the annotated template, read back through reflection.
            ZHLN::Test::ExpectEq(std::string(ZHLN::Error(code).Message()), std::string("the CDN has no asset at '{}' (HTTP 404)."));

            // FormatEnumMessage fills the template with the failure site's context, as Wire does.
            ZHLN::Test::ExpectEq(
                ZHLN::Reflect::FormatEnumMessage(ZHLN::CDN::CDNError::HTTPStatus, 503, std::string("models/helmet.glb")),
                std::string("the CDN answered HTTP 503 for 'models/helmet.glb'.")
            );
            return {};
        }

        // --- configuration ---

        std::expected<void, ZHLN::ErrorCode> a_base_url_must_be_an_absolute_http_url() {
            const char* const bad[] = {"", "ftp://example.com", "example.com/assets", "https://", "https:///path", "http://host with space"};
            for (const char* base: bad) {
                const ZHLN::CDN::CDNConfig config {.baseURL = base};
                auto                       made   = ZHLN::CDN::CDNManager::Create(config);
                ZHLN::Test::ExpectFalse(made.has_value());
                if (!made.has_value()) {
                    ZHLN::Test::ExpectTrue(made.error().Is(ZHLN::CDN::CDNError::InvalidConfig));
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> the_manager_copies_the_base_url() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }

            // The string the config views dies at the end of this scope. The
            // manager must not notice.
            auto made = [&]() -> std::expected<ZHLN::CDN::CDNManager, ZHLN::ErrorCode> {
                std::string transient = origin + "/";
                const ZHLN::CDN::CDNConfig config {
                    .baseURL = transient,
                    .cacheDir = ScratchDir("copy"),
                };
                return ZHLN::CDN::CDNManager::Create(config);
            }();
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }

            // A trailing '/' in the config is dropped, once.
            ZHLN::Test::ExpectEq(std::string(made->GetURL()), origin);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> relative_paths_join_under_the_base_and_unsafe_ones_are_refused() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("paths")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            const ZHLN::CDN::CDNManager& cdn = *made;

            auto joined = cdn.GetURL("models/helmet.glb");
            ZHLN::Test::ExpectTrue(joined.has_value());
            if (joined.has_value()) {
                ZHLN::Test::ExpectEq(*joined, server.Url("/models/helmet.glb"));
            }

            // A leading '/' is the same path, not a second spelling of it.
            auto leading = cdn.GetURL("/ok");
            ZHLN::Test::ExpectTrue(leading.has_value() && *leading == server.Url("/ok"));

            const char* const unsafe[] = {"", "/", "../secret", "a//b", "a/./b", "a/", "a b", "a?b=1", "a#frag", "a\\b"};
            for (const char* path: unsafe) {
                auto refused = cdn.GetURL(path);
                ZHLN::Test::ExpectFalse(refused.has_value());
                if (!refused.has_value()) {
                    ZHLN::Test::ExpectTrue(refused.error().Is(ZHLN::CDN::CDNError::InvalidPath));
                }
            }
            return {};
        }

        // --- Fetch: cache, else download to disk ---

        std::expected<void, ZHLN::ErrorCode> fetch_downloads_once_then_serves_from_the_cache() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("fetch")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::CDN::CDNManager& cdn = *made;

            const uint64_t before = server.Requests();
            auto           first  = cdn.Fetch("ok", IsHello);
            if (!ZHLN::Test::ExpectTrue(first.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue(server.Requests() > before); // it went on the wire
            ZHLN::Test::ExpectTrue(std::filesystem::exists(*first));
            ZHLN::Test::ExpectTrue(std::filesystem::path(*first).parent_path() == cdn.Cache().Root());

            // Load answers from the file Fetch just wrote: no request.
            const uint64_t beforeLoad = server.Requests();
            auto           bytes      = cdn.Load("ok", IsHello);
            ZHLN::Test::ExpectTrue(bytes.has_value() && bytes->size() == 5);
            ZHLN::Test::ExpectEq(server.Requests(), beforeLoad);

            const uint64_t mid   = server.Requests();
            auto           again = cdn.Fetch("ok", IsHello);
            ZHLN::Test::ExpectTrue(again.has_value() && first.has_value() && *again == *first);
            ZHLN::Test::ExpectEq(server.Requests(), mid); // the second Fetch was a cache hit
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_corrupt_cache_file_is_removed_and_refetched() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("corrupt")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::CDN::CDNManager& cdn      = *made;
            const std::string      url      = cdn.GetURL("ok").value_or(std::string());
            const std::string      fileName = ZHLN::Remote::ResolveURL(url).cacheFileName;

            // Four bytes where the validator wants five: a truncated download. Written
            // directly, because the cache's own writer is reached through a const
            // accessor here and the point is to plant a file the cache did not write.
            {
                std::ofstream out(cdn.Cache().Root() / fileName, std::ios::binary);
                out << "junk";
            }

            auto fetched = cdn.Fetch("ok", IsHello);
            if (!ZHLN::Test::ExpectTrue(fetched.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectEq(std::filesystem::file_size(*fetched), static_cast<uintmax_t>(5));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_rejected_body_is_not_cached() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("rejected")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::CDN::CDNManager& cdn = *made;

            auto refused = cdn.Fetch("ok", RejectAll);
            ZHLN::Test::ExpectFalse(refused.has_value());
            if (!refused.has_value()) {
                ZHLN::Test::ExpectTrue(refused.error().Is(ZHLN::CDN::CDNError::Rejected));
            }
            ZHLN::Test::ExpectFalse(cdn.Cache().Exists(ZHLN::Remote::ResolveURL(cdn.GetURL("ok").value_or(std::string())).cacheFileName));
            return {};
        }

        // --- Load: cache, else raw content ---

        std::expected<void, ZHLN::ErrorCode> load_returns_raw_content_and_writes_nothing() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("load")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::CDN::CDNManager& cdn      = *made;
            const std::string      url      = cdn.GetURL("binary").value_or(std::string());
            const std::string      fileName = ZHLN::Remote::ResolveURL(url).cacheFileName;

            // /binary is 256 bytes, 0x00 through 0xFF: raw content, unmodified.
            auto loaded = cdn.Load("binary");
            if (!ZHLN::Test::ExpectTrue(loaded.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectEq(loaded->size(), static_cast<size_t>(256));
            bool exact = loaded->size() == 256;
            for (size_t i = 0; exact && i < loaded->size(); ++i) {
                exact = ((*loaded)[i] == static_cast<uint8_t>(i));
            }
            ZHLN::Test::ExpectTrue(exact);

            // Load does not persist. Fetch does.
            ZHLN::Test::ExpectFalse(cdn.Cache().Exists(fileName));

            // Once the file is cached, Load answers from the cache.
            ZHLN::Test::ExpectTrue(cdn.Fetch("binary").has_value());
            const uint64_t before = server.Requests();
            auto           again  = cdn.Load("binary");
            ZHLN::Test::ExpectTrue(again.has_value() && again->size() == 256);
            ZHLN::Test::ExpectEq(server.Requests(), before);
            return {};
        }

        // --- transport semantics ---

        std::expected<void, ZHLN::ErrorCode> a_redirect_is_followed() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("redirect")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }

            auto loaded = made->Load("redirect", IsHello);
            if (ZHLN::Test::ExpectTrue(loaded.has_value())) {
                ZHLN::Test::ExpectTrue(loaded->size() == 5);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> an_http_error_is_reported_with_its_status_and_cached_nothing() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("status")};
            auto                       made = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::CDN::CDNManager& cdn = *made;

            auto missing = cdn.Fetch("notfound");
            ZHLN::Test::ExpectFalse(missing.has_value());
            if (!missing.has_value()) {
                ZHLN::Test::ExpectTrue(missing.error().Is(ZHLN::CDN::CDNError::NotFound));
            }

            auto broken = cdn.Load("servererror");
            ZHLN::Test::ExpectFalse(broken.has_value());
            if (!broken.has_value()) {
                ZHLN::Test::ExpectTrue(broken.error().Is(ZHLN::CDN::CDNError::HTTPStatus));
            }

            ZHLN::Test::ExpectFalse(cdn.Cache().Exists(ZHLN::Remote::ResolveURL(cdn.GetURL("notfound").value_or(std::string())).cacheFileName));
            return {};
        }
    };
};

} // namespace

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    // The loopback server is 127.0.0.1: a proxy in the environment would send
    // the transfer somewhere else and report a failure about the machine.
    for (const char* name: {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    return ZHLN::Test::Runner::Run<CDNTestSuite>();
}
