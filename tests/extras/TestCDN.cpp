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
// Not covered here: libcurl error mapping (TestHTTP's ground). Async expiry
// is covered through RequestAsset/Poll as well as the blocking Fetch/Load API.

#include "TestsFramework.hpp"
#include <CDN/CDN.hpp>
#include <HTTP/HTTPServer.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <RemoteAsset/FetchJob.hpp>
#include <RemoteAsset/FetchSlotTable.hpp>
#include <RemoteAsset/URLResolver.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

// Backdate persisted metadata instead of sleeping: expiry tests are deterministic.
auto AgeFile(const std::filesystem::path& file, std::chrono::seconds age) -> bool {
    std::error_code ec;
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - age, ec);
    return !ec;
}

auto WaitFor(ZHLN::CDN::CDNManager& cdn, ZHLN::Remote::FetchHandle handle) -> std::optional<std::expected<ZHLN::Remote::FetchPayload, ZHLN::ErrorCode>> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        if (auto result = cdn.Poll(handle)) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    cdn.Cancel(handle);
    return std::nullopt;
}

auto RevalidatingCDN(ZHLN::HTTP::LoopbackServer& server, std::string_view name, std::chrono::seconds interval = std::chrono::seconds::zero())
    -> std::expected<ZHLN::CDN::CDNManager, ZHLN::ErrorCode> {
    const auto origin = server.Url("");
    return ZHLN::CDN::CDNManager::Create({.baseURL = origin, .cacheDir = ScratchDir(name), .cachePolicy = {.revalidateAfter = interval}});
}

auto BodyText(const std::vector<uint8_t>& bytes) -> std::string_view {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto ExpectBody(ZHLN::CDN::CDNManager& cdn, std::string_view path, std::string_view expected) -> bool {
    auto bytes = cdn.Load(path, IsHello);
    return ZHLN::Test::ExpectTrue(bytes.has_value()) && ZHLN::Test::ExpectEq(BodyText(*bytes), expected);
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
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("fetch"), .cachePolicy = {.revalidateAfter = std::chrono::minutes(5)}};
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

        std::expected<void, ZHLN::ErrorCode> load_returns_raw_content_and_caches_successful_downloads() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string origin = server.Url("");
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = ScratchDir("load"), .cachePolicy = {.revalidateAfter = std::chrono::minutes(5)}};
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

            // FetchSync persists on download, so Load now leaves a cache file.
            ZHLN::Test::ExpectTrue(cdn.Cache().Exists(fileName));

            // Once the file is cached, Load answers from the cache.
            ZHLN::Test::ExpectTrue(cdn.Fetch("binary").has_value());
            const uint64_t before = server.Requests();
            auto           again  = cdn.Load("binary");
            ZHLN::Test::ExpectTrue(again.has_value() && again->size() == 256);
            ZHLN::Test::ExpectEq(server.Requests(), before);
            return {};
        }

        // --- automatic cache lifetime, exercised through the public CDN API ---

        std::expected<void, ZHLN::ErrorCode> create_automatically_removes_expired_files_from_previous_runs() {
            const auto dir = ScratchDir("startup-cleanup");
            std::ofstream(dir / "never-requested.bin", std::ios::binary) << "old";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "never-requested.bin", std::chrono::hours(24 * 8)));
            auto made = ZHLN::CDN::CDNManager::Create({.baseURL = "https://cdn.example.com", .cacheDir = dir});
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "never-requested.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> fetch_and_load_automatically_refetch_expired_valid_bytes() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const std::string origin = server.Url("");
            const auto        dir    = ScratchDir("blocking-expiry");
            auto              made   = ZHLN::CDN::CDNManager::Create({
                .baseURL     = origin,
                .cacheDir    = dir,
                .cachePolicy = {.maxAge = std::chrono::seconds(60), .revalidateAfter = std::chrono::minutes(5)},
            });
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            const auto file = dir / ZHLN::Remote::ResolveURL(server.Url("/ok")).cacheFileName;
            ZHLN::Test::ExpectEq(made->Cache().Policy().maxAge.count(), int64_t {60});
            ZHLN::Test::ExpectTrue(made->Fetch("ok", IsHello).has_value());
            const auto before = server.Requests();
            ZHLN::Test::ExpectTrue(AgeFile(file, std::chrono::seconds(120)));
            auto refreshed = made->Fetch("ok", IsHello);
            ZHLN::Test::ExpectTrue(refreshed.has_value() && *refreshed == file);
            ZHLN::Test::ExpectEq(server.Requests(), before + 1);
            ZHLN::Test::ExpectTrue(AgeFile(file, std::chrono::seconds(120)));
            auto loaded = made->Load("ok", IsHello);
            ZHLN::Test::ExpectTrue(loaded.has_value() && loaded->size() == 5);
            ZHLN::Test::ExpectEq(server.Requests(), before + 2);

            // The new bytes start a new lifetime and are reusable immediately.
            ZHLN::Test::ExpectTrue(made->Load("ok", IsHello).has_value());
            ZHLN::Test::ExpectEq(server.Requests(), before + 2);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> request_asset_uses_the_same_expiry_policy_without_caller_invalidation() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const std::string origin = server.Url("");
            const auto        dir    = ScratchDir("async-expiry");
            auto              made   = ZHLN::CDN::CDNManager::Create({
                .baseURL     = origin,
                .cacheDir    = dir,
                .cachePolicy = {.maxAge = std::chrono::seconds(60), .revalidateAfter = std::chrono::minutes(5)},
            });
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            // Exercise move construction as Create returns a movable manager;
            // the fetcher must retain the same owned policy and maintenance state.
            auto       cdn   = std::move(*made);
            const auto file  = dir / ZHLN::Remote::ResolveURL(server.Url("/ok")).cacheFileName;
            const auto first = cdn.RequestAsset("ok", IsHello);
            if (!ZHLN::Test::ExpectTrue(first.has_value())) {
                return {};
            }
            auto downloaded = WaitFor(cdn, *first);
            if (!ZHLN::Test::ExpectTrue(downloaded.has_value() && downloaded->has_value())) {
                return {};
            }
            ZHLN::Test::ExpectFalse(downloaded->value().fromCache);
            ZHLN::Test::ExpectTrue(AgeFile(file, std::chrono::seconds(120)));
            const auto before = server.Requests();
            const auto second = cdn.RequestAsset("ok", IsHello);
            if (!ZHLN::Test::ExpectTrue(second.has_value())) {
                return {};
            }
            auto refreshed = WaitFor(cdn, *second);
            if (ZHLN::Test::ExpectTrue(refreshed.has_value() && refreshed->has_value())) {
                ZHLN::Test::ExpectFalse(refreshed->value().fromCache);
                ZHLN::Test::ExpectEq(refreshed->value().data.size(), size_t {5});
            }
            ZHLN::Test::ExpectEq(server.Requests(), before + 1);
            const auto third = cdn.RequestAsset("ok", IsHello);
            if (ZHLN::Test::ExpectTrue(third.has_value())) {
                auto cached = cdn.Poll(*third); // cache hits are ready immediately
                ZHLN::Test::ExpectTrue(cached && *cached && cached->value().fromCache);
            }
            ZHLN::Test::ExpectEq(server.Requests(), before + 1);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cache_budget_does_not_turn_an_in_memory_download_into_a_failure() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const std::string origin = server.Url("");
            const auto        dir    = ScratchDir("budget");
            auto              made   = ZHLN::CDN::CDNManager::Create({
                .baseURL     = origin,
                .cacheDir    = dir,
                .cachePolicy = {.maxBytes = 256},
            });
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            ZHLN::Test::ExpectTrue(made->Fetch("ok").has_value());
            auto bytes = made->Load("binary"); // 256 body bytes plus metadata will not fit a 256-byte budget
            ZHLN::Test::ExpectTrue(bytes.has_value() && bytes->size() == 256);
            const auto key = ZHLN::Remote::ResolveURL(server.Url("/binary")).cacheFileName;
            ZHLN::Test::ExpectFalse(made->Cache().Exists(key));
            ZHLN::Test::ExpectTrue(made->Cache().Exists(ZHLN::Remote::ResolveURL(server.Url("/ok")).cacheFileName));
            auto path = made->Fetch("binary"); // Fetch specifically promises an on-disk file
            ZHLN::Test::ExpectFalse(path.has_value());
            if (!path) {
                ZHLN::Test::ExpectTrue(path.error().Is(ZHLN::CDN::CDNError::CacheWrite));
            }
            const auto request = made->RequestAsset("binary");
            if (ZHLN::Test::ExpectTrue(request.has_value())) {
                auto payload = WaitFor(*made, *request);
                if (ZHLN::Test::ExpectTrue(payload && payload->has_value())) {
                    ZHLN::Test::ExpectEq(payload->value().data.size(), size_t {256});
                    ZHLN::Test::ExpectTrue(payload->value().localCachePath.empty());
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> an_unusable_cache_directory_does_not_fail_an_in_memory_load() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const std::string origin = server.Url("");
            const auto        root   = ScratchDir("unusable-directory") / "not-a-directory";
            std::ofstream(root) << "not a cache";
            auto made = ZHLN::CDN::CDNManager::Create({.baseURL = origin, .cacheDir = root});
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            auto bytes = made->Load("ok", IsHello);
            ZHLN::Test::ExpectTrue(bytes.has_value() && bytes->size() == 5);
            auto path = made->Fetch("ok", IsHello);
            ZHLN::Test::ExpectFalse(path.has_value());
            if (!path) {
                ZHLN::Test::ExpectTrue(path.error().Is(ZHLN::CDN::CDNError::CacheWrite));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> an_expired_entry_is_not_a_fallback_when_the_server_refuses_it() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const std::string origin = server.Url("");
            const auto        dir    = ScratchDir("expired-notfound");
            auto              made   = ZHLN::CDN::CDNManager::Create({
                .baseURL     = origin,
                .cacheDir    = dir,
                .cachePolicy = {.maxAge = std::chrono::seconds(60), .revalidateAfter = std::chrono::minutes(5)},
            });
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            const auto key = ZHLN::Remote::ResolveURL(server.Url("/notfound")).cacheFileName;
            std::ofstream(dir / key, std::ios::binary) << "hello";
            ZHLN::Test::ExpectTrue(AgeFile(dir / key, std::chrono::seconds(120)));
            const auto before = server.Requests();
            auto       loaded = made->Load("notfound", IsHello);
            ZHLN::Test::ExpectFalse(loaded.has_value());
            if (!loaded) {
                ZHLN::Test::ExpectTrue(loaded.error().Is(ZHLN::CDN::CDNError::NotFound));
            }
            ZHLN::Test::ExpectEq(server.Requests(), before + 1);
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / key));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> callers_can_explicitly_disable_age_expiry() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(CDNTestError::ServerUnavailable);
            }
            const std::string origin = server.Url("");
            const auto        dir    = ScratchDir("no-expiry");
            auto              made   = ZHLN::CDN::CDNManager::Create({
                .baseURL     = origin,
                .cacheDir    = dir,
                .cachePolicy = {.maxAge = std::chrono::seconds::zero(), .revalidateAfter = std::chrono::minutes(5)},
            });
            if (!ZHLN::Test::ExpectTrue(made.has_value())) {
                return std::unexpected(CDNTestError::ManagerRefused);
            }
            auto first = made->Fetch("ok", IsHello);
            if (!ZHLN::Test::ExpectTrue(first.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue(AgeFile(*first, std::chrono::hours(24 * 30)));
            const auto before = server.Requests();
            ZHLN::Test::ExpectTrue(made->Load("ok", IsHello).has_value());
            ZHLN::Test::ExpectEq(server.Requests(), before);
            return {};
        }

        // --- Freshness at the origin, not just a disk TTL ---
        std::expected<void, ZHLN::ErrorCode> development_defaults_revalidate_every_request() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            const auto origin = server.Url("");
            auto       made   = ZHLN::CDN::CDNManager::Create({.baseURL = origin, .cacheDir = ScratchDir("dev-default")});
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ZHLN::Test::ExpectEq(made->Cache().Policy().revalidateAfter.count(), int64_t {ZHLN::isDev ? 0 : 300});
            ExpectBody(*made, "asset", "hello");
            const auto before = server.Requests();
            ExpectBody(*made, "asset", "hello");
            ZHLN::Test::ExpectEq(server.Requests(), before + (ZHLN::isDev ? 1 : 0));
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {ZHLN::isDev ? 1U : 0U});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> an_unchanged_etag_reuses_verified_bytes_and_survives_reopening_the_cache() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "W/\"opaque-v1\"", .omitValidatorsOn304 = true});
            const auto                 dir    = ScratchDir("persisted-etag");
            const auto                 origin = server.Url("");
            const ZHLN::CDN::CDNConfig config {.baseURL = origin, .cacheDir = dir, .cachePolicy = {.revalidateAfter = std::chrono::seconds::zero()}};
            {
                auto first = ZHLN::CDN::CDNManager::Create(config);
                if (!ZHLN::Test::ExpectTrue(server.IsListening() && first.has_value()))
                    return std::unexpected(CDNTestError::ManagerRefused);
                ExpectBody(*first, "asset", "hello");
            }
            const auto file     = dir / ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            const auto written  = std::filesystem::last_write_time(file);
            auto       reopened = ZHLN::CDN::CDNManager::Create(config);
            if (!ZHLN::Test::ExpectTrue(reopened.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            auto path = reopened->Fetch("asset", IsHello);
            ZHLN::Test::ExpectTrue(path && *path == file);
            ExpectBody(*reopened, "asset", "hello");
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {2});
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {2});
            ZHLN::Test::ExpectTrue(std::filesystem::last_write_time(file) == written); // no body rewrite for a 304
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> editing_the_same_url_is_detected_by_etag_even_if_size_and_last_modified_do_not_change() {
            ZHLN::HTTP::LoopbackServer server;
            const std::string          date = "Thu, 08 Oct 2026 10:00:00 GMT";
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\"", .lastModified = date});
            auto made = RevalidatingCDN(server, "changed-etag");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            server.SetResource("/asset", {.body = "world", .etag = "\"v2\"", .lastModified = date});
            ExpectBody(*made, "asset", "world"); // IsHello accepts both five-byte bodies.
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {1});
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {0}); // ETag wins over the unchanged timestamp.
            const auto key    = ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            const auto cached = made->Cache().ReadEntry(key, IsHello);
            ZHLN::Test::ExpectTrue(cached && cached->origin.etag == "\"v2\"");
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> last_modified_is_used_when_etag_is_absent() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .lastModified = "Thu, 08 Oct 2026 10:00:00 GMT"});
            auto made = RevalidatingCDN(server, "last-modified");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            ExpectBody(*made, "asset", "hello");
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            server.SetResource("/asset", {.body = "world", .lastModified = "Fri, 09 Oct 2026 10:00:00 GMT"});
            ExpectBody(*made, "asset", "world");
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {2});
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> validatorless_origins_and_legacy_cache_files_are_downloaded_instead_of_trusted_forever() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello"});
            auto made = RevalidatingCDN(server, "no-validators");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            const auto key = ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            std::ofstream(made->Cache().Root() / key, std::ios::binary) << "stale"; // valid-length legacy data, no metadata
            ExpectBody(*made, "asset", "hello");
            server.SetResource("/asset", {.body = "world", .etag = "*"});
            ExpectBody(*made, "asset", "world");
            server.SetResource("/asset", {.body = "hello", .etag = "*"}); // wildcard is not a reusable entity tag
            ExpectBody(*made, "asset", "hello");
            ZHLN::Test::ExpectEq(server.Requests(), uint64_t {3});
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {0});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> corrupt_but_valid_length_bytes_are_refetched_without_sending_the_old_etag() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            auto made = RevalidatingCDN(server, "checksum-before-304", std::chrono::hours(1));
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            const auto file = made->Cache().Root() / ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            std::ofstream(file, std::ios::binary | std::ios::trunc) << "jello";
            ExpectBody(*made, "asset", "hello");
            ZHLN::Test::ExpectEq(server.Requests(), uint64_t {2});
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {0});
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {0});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> origin_checks_run_on_the_async_path_and_return_the_updated_content() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            auto made = RevalidatingCDN(server, "async-revalidation");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            auto unchanged = made->RequestAsset("asset", IsHello);
            if (!ZHLN::Test::ExpectTrue(unchanged.has_value()))
                return {};
            auto checked = WaitFor(*made, *unchanged);
            ZHLN::Test::ExpectTrue(checked && *checked && checked->value().fromCache);
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            server.SetResource("/asset", {.body = "world", .etag = "\"v2\""});
            auto changed = made->RequestAsset("asset", IsHello);
            if (!ZHLN::Test::ExpectTrue(changed.has_value()))
                return {};
            auto refreshed = WaitFor(*made, *changed);
            if (ZHLN::Test::ExpectTrue(refreshed && *refreshed)) {
                ZHLN::Test::ExpectFalse(refreshed->value().fromCache);
                ZHLN::Test::ExpectEq(BodyText(refreshed->value().data), std::string_view("world"));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_304_retries_unconditionally_if_the_cached_body_or_metadata_disappears_in_flight() {
            for (const bool removeMetadata: {false, true}) {
                ZHLN::HTTP::LoopbackServer server;
                server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
                auto made = RevalidatingCDN(server, removeMetadata ? "304-missing-metadata" : "304-missing-body");
                if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                    return std::unexpected(CDNTestError::ManagerRefused);
                ExpectBody(*made, "asset", "hello");
                const auto file   = made->Cache().Root() / ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
                const auto remove = removeMetadata ? std::filesystem::path(file.string() + ".meta") : file;
                auto       once   = std::make_shared<std::atomic<bool>>(false);
                server.SetResource("/asset", {.body = "hello", .etag = "\"v1\"", .beforeReply = [remove, once] {
                                                  if (!once->exchange(true)) {
                                                      std::error_code ec;
                                                      std::filesystem::remove(remove, ec);
                                                  }
                                              }});
                auto path = made->Fetch("asset", IsHello);
                ZHLN::Test::ExpectTrue(path && *path == file && std::filesystem::exists(*path));
                ZHLN::Test::ExpectEq(server.Requests(), uint64_t {3}); // initial 200, unusable 304, unconditional 200
                ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> no_cache_and_server_max_age_can_shorten_a_long_client_revalidation_interval() {
            for (const auto& [control, age]: std::vector<std::pair<std::string, std::string>> {{"No-CaChe", ""}, {"max-age=0", ""}, {"max-age=\"60\"", "61"}}) {
                ZHLN::HTTP::LoopbackServer server;
                server.SetResource("/asset", {.body = "hello", .etag = "\"v1\"", .cacheControl = control, .age = age});
                auto made = RevalidatingCDN(server, "origin-policy", std::chrono::hours(1));
                if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                    return std::unexpected(CDNTestError::ManagerRefused);
                ExpectBody(*made, "asset", "hello");
                ExpectBody(*made, "asset", "hello");
                ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            }
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\"", .cacheControl = "max-age=60"});
            auto made = RevalidatingCDN(server, "server-max-age", std::chrono::hours(1));
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            ExpectBody(*made, "asset", "hello"); // fresh, no request
            ZHLN::Test::ExpectEq(server.Requests(), uint64_t {1});
            const auto file = made->Cache().Root() / ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            ZHLN::Test::ExpectTrue(AgeFile(std::filesystem::path(file.string() + ".meta"), std::chrono::seconds(120)));
            ExpectBody(*made, "asset", "hello");
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> no_store_is_obeyed_on_both_200_and_304_responses() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            auto made = RevalidatingCDN(server, "no-store");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            const auto key = ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\"", .cacheControl = "no-store"});
            ExpectBody(*made, "asset", "hello"); // 304 can change storage policy too.
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
            ZHLN::Test::ExpectFalse(made->Cache().Exists(key));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(made->Cache().Root() / (key + ".meta")));
            ExpectBody(*made, "asset", "hello"); // now an unconditional 200, still not stored
            ZHLN::Test::ExpectFalse(made->Cache().Exists(key));
            auto path = made->Fetch("asset", IsHello);
            ZHLN::Test::ExpectFalse(path.has_value());
            if (!path)
                ZHLN::Test::ExpectTrue(path.error().Is(ZHLN::CDN::CDNError::CacheWrite));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> rejected_updates_and_server_failures_do_not_masquerade_as_successful_cache_hits() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            auto made = RevalidatingCDN(server, "failed-revalidation");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            server.SetResource("/asset", {.body = "bad", .etag = "\"v2\""});
            auto rejected = made->Load("asset", IsHello);
            ZHLN::Test::ExpectTrue(!rejected && rejected.error().Is(ZHLN::CDN::CDNError::Rejected));
            server.SetResource("/asset", {.body = "error", .status = 500});
            auto failed = made->Load("asset", IsHello);
            ZHLN::Test::ExpectTrue(!failed && failed.error().Is(ZHLN::CDN::CDNError::HTTPStatus));
            server.SetResource("/asset", {.body = "world", .etag = "\"v2\""});
            ExpectBody(*made, "asset", "world");
            server.SetResource("/asset", {.status = 404});
            auto removed = made->Load("asset", IsHello);
            ZHLN::Test::ExpectTrue(!removed && removed.error().Is(ZHLN::CDN::CDNError::NotFound));
            ZHLN::Test::ExpectFalse(made->Cache().Exists(ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> unsolicited_304_and_partial_responses_are_not_cached_as_complete_assets() {
            for (const int status: {304, 206}) {
                ZHLN::HTTP::LoopbackServer server;
                server.SetResource("/asset", {.body = "hello", .status = status});
                auto made = RevalidatingCDN(server, "invalid-status");
                if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                    return std::unexpected(CDNTestError::ManagerRefused);
                auto bytes = made->Load("asset", IsHello);
                ZHLN::Test::ExpectTrue(!bytes && bytes.error().Is(ZHLN::CDN::CDNError::HTTPStatus));
                ZHLN::Test::ExpectEq(server.Requests(), uint64_t {1}); // bounded: no retry loop
                ZHLN::Test::ExpectFalse(made->Cache().Exists(ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> redirected_resources_do_not_share_validators_even_when_their_etags_collide() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            server.SetResource("/other", {.body = "world", .etag = "\"v1\""}); // same token, different resource
            auto made = RevalidatingCDN(server, "redirect-validators");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            server.SetResource("/asset", {.location = "/other", .status = 302});
            ExpectBody(*made, "asset", "world");
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {0});
            const auto conditionals = server.ConditionalRequests();
            ExpectBody(*made, "asset", "world"); // redirected metadata has no reusable original-URL validator
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), conditionals);
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {0});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> metadata_for_a_different_url_cannot_authorize_even_a_fresh_cache_hit() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "world", .etag = "\"v2\""});
            auto made = RevalidatingCDN(server, "url-binding", std::chrono::hours(1));
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            const auto                 key = ZHLN::Remote::ResolveURL(server.Url("/asset")).cacheFileName;
            ZHLN::Remote::DiskCache    seed(made->Cache().Root(), {.revalidateAfter = std::chrono::hours(1)});
            const std::vector<uint8_t> bytes {'h', 'e', 'l', 'l', 'o'};
            ZHLN::Test::ExpectTrue(seed.WriteAtomic(key, bytes, {.url = server.Url("/different"), .etag = "\"v1\""}));
            ExpectBody(*made, "asset", "world");
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {0});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_conflicting_etag_on_304_forces_one_unconditional_body_download() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            auto made = RevalidatingCDN(server, "conflicting-304");
            if (!ZHLN::Test::ExpectTrue(server.IsListening() && made.has_value()))
                return std::unexpected(CDNTestError::ManagerRefused);
            ExpectBody(*made, "asset", "hello");
            server.SetResource("/asset", {.etag = "\"v2\"", .status = 304, .beforeReply = [&server] {
                                              server.SetResource("/asset", {.body = "world", .etag = "\"v2\""});
                                          }});
            ExpectBody(*made, "asset", "world");
            ZHLN::Test::ExpectEq(server.Requests(), uint64_t {3});
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {1});
            ZHLN::Test::ExpectEq(server.NotModifiedResponses(), uint64_t {1});
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
    ZHLN::TaskSystem::Scope taskScope;
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
