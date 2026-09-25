// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestRemoteAsset.cpp
//
// RemoteAsset is an optional layer (extras/RemoteAsset) over an optional
// third-party library (libcurl, through extras/HTTP), so this suite is built
// only when both exist. The pure half -- URL rewrites, the GLB validator, the
// disk cache -- is tested against a temporary directory and never leaves the
// machine. The async half is tested against a loopback server of its own
// (extras/HTTP/HTTPServer.hpp), on 127.0.0.1 on an ephemeral port, so no
// assertion depends on a CI runner's egress rules or a third-party host
// staying up.
//
// Not covered here, and why:
//
//   * A superseded transfer retiring between candidates. It needs two
//     transfers in flight with a timed gap between the server's answers, and
//     the behaviour it protects (a superseded worker keeps its cache file and
//     publishes to its own slot) is three lines the fetcher's comments name;
//     a timed test of it would be flaky by construction.
//   * The libcurl error mapping. That is TestHTTP's ground; this suite only
//     asks for a transfer and checks the fetcher's bookkeeping around it.

#include "TestsFramework.hpp"
#include <HTTP/HTTPServer.hpp>
#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <RemoteAsset/URLResolver.hpp>

#include <atomic>
#include <chrono>
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
#include <thread>
#include <vector>

namespace
{

using ZHLN::Remote::FetchStatus;
using ZHLN::Remote::ResolvedURL;
using ZHLN::Remote::Validators::AnyNonEmpty;
using ZHLN::Remote::Validators::IsGLB;

// The suite's scratch directory, under the system temp, cleared at the start
// of each test (stale files from a crashed run included). One per test so a
// failure in one cannot poison another; the suite is one process per binary
// and its tests run in order, so no pid is in the name.
auto ScratchDir(std::string_view name) -> std::filesystem::path {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("zhaln-test-remoteasset-{}", name);
    std::error_code             ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

template <typename T>
auto AsSpan(std::vector<T>& vec) -> std::span<T> {
    return std::span(vec);
}

// A GLB container of @p contentBytes around the 12-byte header: magic,
// version 2, and a length field that agrees with the total size.
auto MakeGLB(size_t contentBytes) -> std::vector<uint8_t> {
    std::vector<uint8_t> bytes(12 + contentBytes, 0xAB);
    bytes[0] = 'g';
    bytes[1] = 'l';
    bytes[2] = 'T';
    bytes[3] = 'F';
    bytes[4] = 2; // version, little-endian
    const uint32_t total = static_cast<uint32_t>(bytes.size());
    bytes[8]  = static_cast<uint8_t>(total);
    bytes[9]  = static_cast<uint8_t>(total >> 8);
    bytes[10] = static_cast<uint8_t>(total >> 16);
    bytes[11] = static_cast<uint8_t>(total >> 24);
    return bytes;
}

// Polls @p fetcher until @p id is terminal or the budget runs out.
auto WaitTerminal(ZHLN::Remote::AsyncAssetFetcher& fetcher, uint32_t id, int milliseconds = 10000) -> FetchStatus {
    for (int waited = 0; waited < milliseconds; waited += 10) {
        const FetchStatus status = fetcher.Status(id);
        if (status == FetchStatus::Succeeded || status == FetchStatus::Failed) {
            return status;
        }
        fetcher.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fetcher.Status(id);
}

} // namespace

struct RemoteAssetTestSuite {
    enum class RemoteAssetTestError : uint8_t {
        ServerUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The suite's loopback server did not come up, so there was nothing to fetch."> {}) = 1,
        DiskUnavailable   ZHLN_ANNOTATION(ZHLN::Description<"The scratch directory could not be written, so there was nothing to cache."> {}),
    };

    struct Tests {
        // --- URLResolver: candidate spellings ---
        std::expected<void, ZHLN::ErrorCode> a_plain_url_is_its_only_candidate() {
            const ResolvedURL r = ZHLN::Remote::ResolveURL("https://example.com/models/helmet.glb");

            ZHLN::Test::ExpectEq(r.primary, "https://example.com/models/helmet.glb");
            ZHLN::Test::ExpectTrue(r.fallbacks.empty());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_github_blob_url_gets_both_raw_spellings_in_order() {
            const std::string url = "https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Fox/glTF-Binary/Fox.glb";
            const ResolvedURL r   = ZHLN::Remote::ResolveURL(url);

            ZHLN::Test::ExpectEq(r.primary, url);
            ZHLN::Test::ExpectTrue(r.fallbacks.size() == 2);
            ZHLN::Test::ExpectEq(
                r.fallbacks[0], "https://github.com/KhronosGroup/glTF-Sample-Assets/raw/main/Models/Fox/glTF-Binary/Fox.glb"
            );
            ZHLN::Test::ExpectEq(
                r.fallbacks[1], "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Fox/glTF-Binary/Fox.glb"
            );
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_non_github_blob_path_stays_single() {
            // The rewrite is for github.com's page links; a /blob/ in some
            // other host's path is not one of them.
            const ResolvedURL r = ZHLN::Remote::ResolveURL("https://host.example/blob/main/file.glb");

            ZHLN::Test::ExpectTrue(r.fallbacks.empty());
            return {};
        }

        // --- URLResolver: the cache file name ---
        std::expected<void, ZHLN::ErrorCode> the_cache_name_is_stem_hash_extension_and_deterministic() {
            const ResolvedURL a = ZHLN::Remote::ResolveURL("https://example.com/a/Model.glb");
            const ResolvedURL b = ZHLN::Remote::ResolveURL("https://example.com/a/Model.glb");
            const ResolvedURL c = ZHLN::Remote::ResolveURL("https://example.com/b/Model.glb");

            ZHLN::Test::ExpectEq(a.cacheFileName, b.cacheFileName);
            ZHLN::Test::ExpectTrue(a.cacheFileName != c.cacheFileName);
            ZHLN::Test::ExpectTrue(a.cacheFileName.rfind("Model-", 0) == 0);
            ZHLN::Test::ExpectTrue(a.cacheFileName.ends_with(".bin"));
            // stem + "-" + 8 hex digits + ".bin"
            ZHLN::Test::ExpectTrue(a.cacheFileName.size() == 6 + 8 + 4);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> the_url_stem_is_sanitised() {
            ZHLN::Test::ExpectEq(ZHLN::Remote::UrlStem("https://x.com/a/b?y=1"), "b");
            ZHLN::Test::ExpectEq(ZHLN::Remote::UrlStem("https://x.com/a/b_c-d9.GLB"), "b_c-d9");
            ZHLN::Test::ExpectEq(ZHLN::Remote::UrlStem("https://x.com/a/!!!.glb"), "asset");
            return {};
        }

        // --- Validators ---
        std::expected<void, ZHLN::ErrorCode> the_glb_validator_checks_magic_version_and_length() {
            std::vector<uint8_t> full  = MakeGLB(100);
            std::vector<uint8_t> empty = MakeGLB(0);
            ZHLN::Test::ExpectTrue(IsGLB(AsSpan(full)));
            ZHLN::Test::ExpectTrue(IsGLB(AsSpan(empty)));

            // The magic is gone.
            std::vector<uint8_t> bad = MakeGLB(16);
            bad[0] = 'X';
            ZHLN::Test::ExpectFalse(IsGLB(AsSpan(bad)));

            // Version 1 is not version 2.
            std::vector<uint8_t> old = MakeGLB(16);
            old[4] = 1;
            ZHLN::Test::ExpectFalse(IsGLB(AsSpan(old)));

            // The container disagrees with its own length.
            std::vector<uint8_t> shortByOne = MakeGLB(16);
            shortByOne.resize(shortByOne.size() - 1);
            ZHLN::Test::ExpectFalse(IsGLB(AsSpan(shortByOne)));

            // Shorter than the header.
            std::vector<uint8_t> tiny(4, 'g');
            ZHLN::Test::ExpectFalse(IsGLB(AsSpan(tiny)));

            ZHLN::Test::ExpectTrue(AnyNonEmpty(AsSpan(bad)));
            ZHLN::Test::ExpectFalse(AnyNonEmpty(std::span<const uint8_t> {}));
            return {};
        }

        // --- DiskCache ---
        std::expected<void, ZHLN::ErrorCode> the_disk_cache_round_trips_through_its_validator() {
            const auto           dir   = ScratchDir("roundtrip");
            ZHLN::Remote::DiskCache cache(dir);
            if (!std::filesystem::exists(dir)) {
                return std::unexpected(RemoteAssetTestError::DiskUnavailable);
            }

            ZHLN::Test::ExpectFalse(cache.Exists("a.bin"));
            ZHLN::Test::ExpectFalse(cache.Read("a.bin", IsGLB).has_value());

            std::vector<uint8_t> bytes = MakeGLB(64);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("a.bin", std::span(bytes)));
            ZHLN::Test::ExpectTrue(cache.Exists("a.bin"));

            auto hit = cache.Read("a.bin", IsGLB);
            ZHLN::Test::ExpectTrue(hit.has_value());
            if (hit.has_value()) {
                ZHLN::Test::ExpectEq(hit->size(), bytes.size());
            }

            // A validator that rejects everything sees a miss, not a crash --
            // and the file is still there for the next reader.
            ZHLN::Test::ExpectFalse(cache.Read("a.bin", [](std::span<const uint8_t>) noexcept -> bool { return false; }).has_value());
            ZHLN::Test::ExpectTrue(cache.Exists("a.bin"));

            cache.Invalidate("a.bin");
            ZHLN::Test::ExpectFalse(cache.Exists("a.bin"));
            ZHLN::Test::ExpectFalse(cache.Read("a.bin", IsGLB).has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> the_disk_cache_migrates_the_legacy_glb_spelling() {
            const auto           dir   = ScratchDir("legacy");
            ZHLN::Remote::DiskCache cache(dir);
            if (!std::filesystem::exists(dir)) {
                return std::unexpected(RemoteAssetTestError::DiskUnavailable);
            }

            // The first cache the sample shipped used .glb names; a miss on the
            // current spelling must migrate the file, not re-download it.
            std::vector<uint8_t> bytes = MakeGLB(32);
            {
                std::ofstream out(dir / "legacy-12345678.glb", std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }

            auto hit = cache.Read("legacy-12345678.bin", IsGLB);
            ZHLN::Test::ExpectTrue(hit.has_value());
            ZHLN::Test::ExpectTrue(std::filesystem::exists(dir / "legacy-12345678.bin"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "legacy-12345678.glb"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> concurrent_writes_to_one_name_do_not_tear() {
            const auto           dir   = ScratchDir("concurrent");
            ZHLN::Remote::DiskCache cache(dir);
            if (!std::filesystem::exists(dir)) {
                return std::unexpected(RemoteAssetTestError::DiskUnavailable);
            }

            // Two writers, one name, different contents: each stages in its own
            // temp file, so the winner is a complete file, never a mix.
            std::vector<uint8_t> a = MakeGLB(64);
            std::vector<uint8_t> b = MakeGLB(128);
            for (size_t i = 0; i < b.size(); ++i) {
                b[i] ^= 0x5A;
            }

            // Plain std::thread, not jthread: the test wants two writes and a
            // join, no cancellation, and jthread's stop-token placement has
            // varied between library versions.
            std::atomic<int> done {0};
            auto             writer = [&](std::vector<uint8_t> bytes) {
                // Both writers must complete; the assertion is about the
                // file, not about either write's result.
                (void)cache.WriteAtomic("shared.bin", std::span(bytes));
                done++;
            };
            std::thread ta(writer, a);
            std::thread tb(writer, b);
            ta.join();
            tb.join();

            auto hit = cache.Read("shared.bin", IsGLB);
            ZHLN::Test::ExpectTrue(hit.has_value());
            if (hit.has_value()) {
                const bool isA = (*hit == a);
                const bool isB = (*hit == b);
                ZHLN::Test::ExpectTrue(isA || isB);
            }
            return {};
        }

        // --- AsyncAssetFetcher, against the loopback server ---
        std::expected<void, ZHLN::ErrorCode> a_fetch_lands_in_the_slot_and_the_cache() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                   dir   = ScratchDir("fetch");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir), 10);
            const std::string            url   = server.Url("/ok");

            const uint32_t id = fetcher.Request(url, AnyNonEmpty, false);
            const FetchStatus status = WaitTerminal(fetcher, id);
            ZHLN::Test::ExpectEq(status, FetchStatus::Succeeded);
            if (status != FetchStatus::Succeeded) {
                return {};
            }

            auto result = fetcher.Take(id);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result.has_value()) {
                ZHLN::Test::ExpectFalse(result->fromCache);
                ZHLN::Test::ExpectEq(std::string_view(reinterpret_cast<const char*>(result->data.data()), result->data.size()), std::string_view("hello"));
            }
            // The bytes are on disk now, under the resolved name.
            const std::filesystem::path expected = dir / ZHLN::Remote::ResolveURL(url).cacheFileName;
            ZHLN::Test::ExpectTrue(std::filesystem::exists(expected));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_second_request_for_the_same_url_is_a_cache_hit() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                   dir   = ScratchDir("cachehit");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir), 10);
            const std::string            url   = server.Url("/ok");

            const uint32_t first = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, first), FetchStatus::Succeeded);
            ZHLN::Test::ExpectTrue(fetcher.Take(first).has_value()); // drain the slot

            const uint64_t requestsBefore = server.Requests();
            const uint32_t second         = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(fetcher.Status(second), FetchStatus::Succeeded); // synchronous: no worker, no Poll
            ZHLN::Test::ExpectEq(server.Requests(), requestsBefore);              // and nothing went on the wire
            auto result = fetcher.Take(second);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result.has_value()) {
                ZHLN::Test::ExpectTrue(result->fromCache);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> take_is_a_move_and_a_one_way_door() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                  dir   = ScratchDir("takemove");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir), 10);
            const std::string           url   = server.Url("/ok");

            const uint32_t id = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, id), FetchStatus::Succeeded);
            auto first = fetcher.Take(id);
            ZHLN::Test::ExpectTrue(first.has_value());
            // The id is back to Idle, and the slot is empty: a second Take
            // gets nothing, not a copy.
            ZHLN::Test::ExpectEq(fetcher.Status(id), FetchStatus::Idle);
            ZHLN::Test::ExpectFalse(fetcher.Take(id).has_value());

            // Unknown ids are Idle, not an out-of-bounds.
            ZHLN::Test::ExpectEq(fetcher.Status(9999), FetchStatus::Idle);
            ZHLN::Test::ExpectFalse(fetcher.Take(9999).has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_refused_transfer_fails_with_its_reasons() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                  dir   = ScratchDir("fail");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir), 10);

            // /notfound answers 404; a plain URL has no fallback, so the
            // request fails with the status in its report.
            const uint32_t id = fetcher.Request(server.Url("/notfound"), AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, id), FetchStatus::Failed);
            auto result = fetcher.Take(id);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result.has_value()) {
                ZHLN::Test::ExpectTrue(result->errorMessage.find("HTTP 404") != std::string::npos);
                ZHLN::Test::ExpectTrue(result->data.empty());
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_forced_refresh_skips_the_cache_and_refetches() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                   dir   = ScratchDir("refresh");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir), 10);
            const std::string            url   = server.Url("/ok");

            const uint32_t first = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, first), FetchStatus::Succeeded);
            ZHLN::Test::ExpectTrue(fetcher.Take(first).has_value()); // drain the slot

            const uint64_t before = server.Requests();
            const uint32_t second = fetcher.Request(url, AnyNonEmpty, true);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, second), FetchStatus::Succeeded);
            ZHLN::Test::ExpectTrue(server.Requests() > before); // it went on the wire
            auto result = fetcher.Take(second);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result.has_value()) {
                ZHLN::Test::ExpectFalse(result->fromCache);
            }
            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    // The fetcher's workers go out to 127.0.0.1: a proxy in the environment
    // would send them somewhere else, and the suite would report failures
    // that are about the machine it ran on rather than about the fetcher.
    for (const char* name: {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    return ZHLN::Test::Runner::Run<RemoteAssetTestSuite>();
}
