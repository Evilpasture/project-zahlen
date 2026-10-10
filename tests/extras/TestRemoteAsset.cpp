// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestRemoteAsset.cpp
//
// RemoteAsset is an optional layer (extensions/net/RemoteAsset) over an optional
// third-party library (libcurl, through extensions/net/HTTP), so this suite is built
// only when both exist. The pure half -- URL rewrites, the GLB validator, the
// disk cache -- is tested against a temporary directory and never leaves the
// machine. The async half is tested against a loopback server of its own
// (extensions/net/HTTP/HTTPServer.hpp), on 127.0.0.1 on an ephemeral port, so no
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
#include <Zahlen/Threading/TaskSystem.hpp>

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

// Backdate persisted metadata instead of sleeping: expiry tests are deterministic.
auto AgeFile(const std::filesystem::path& file, std::chrono::seconds age) -> bool {
    std::error_code ec;
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - age, ec);
    return !ec;
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
    bytes[4] = 2; // version 2, little-endian (all four version bytes matter)
    bytes[5] = bytes[6] = bytes[7] = 0;
    const uint32_t total = static_cast<uint32_t>(bytes.size());
    bytes[8]  = static_cast<uint8_t>(total);
    bytes[9]  = static_cast<uint8_t>(total >> 8);
    bytes[10] = static_cast<uint8_t>(total >> 16);
    bytes[11] = static_cast<uint8_t>(total >> 24);
    return bytes;
}

// Polls @p fetcher until @p id is terminal or the budget runs out.
auto WaitTerminal(ZHLN::Remote::AsyncAssetFetcher& fetcher, ZHLN::Remote::FetchHandle id, int milliseconds = 10000) -> FetchStatus {
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

            // All four bytes of the version field must describe version 2.
            std::vector<uint8_t> old = MakeGLB(16);
            old[4] = 1;
            ZHLN::Test::ExpectFalse(IsGLB(AsSpan(old)));
            std::vector<uint8_t> badVersionHighByte = MakeGLB(16);
            badVersionHighByte[5] = 1;
            ZHLN::Test::ExpectFalse(IsGLB(AsSpan(badVersionHighByte)));

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

            // A rejected entry is a miss and is removed by the cache itself;
            // callers no longer need an Exists/Invalidate cleanup pair.
            ZHLN::Test::ExpectFalse(cache.Read("a.bin", [](std::span<const uint8_t>) noexcept -> bool { return false; }).has_value());
            ZHLN::Test::ExpectFalse(cache.Exists("a.bin"));

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
            // Make the payloads differ without corrupting the GLB header:
            // either writer must be a valid cache hit when its rename wins.
            for (size_t i = 12; i < b.size(); ++i) {
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

        std::expected<void, ZHLN::ErrorCode> default_policy_reclaims_old_files_on_open_without_requesting_them() {
            const auto dir = ScratchDir("startup-expiry");
            for (const char* name: {"unvisited.bin", "legacy-12345678.glb", "fresh.bin"}) {
                std::ofstream(dir / name, std::ios::binary) << "cached";
            }
            ZHLN::Test::ExpectTrue(AgeFile(dir / "unvisited.bin", std::chrono::hours(24 * 8)));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "legacy-12345678.glb", std::chrono::hours(24 * 8)));

            ZHLN::Remote::DiskCache cache(dir);
            ZHLN::Test::ExpectEq(cache.Policy().maxAge.count(), std::chrono::seconds(std::chrono::hours(24 * 7)).count());
            ZHLN::Test::ExpectEq(cache.Policy().maxBytes, uintmax_t {1024ULL * 1024ULL * 1024ULL});
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "unvisited.bin"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "legacy-12345678.glb"));
            ZHLN::Test::ExpectTrue(cache.Read("fresh.bin", AnyNonEmpty).has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> requested_entries_expire_even_when_directory_sweeps_are_throttled() {
            const auto                      dir = ScratchDir("expiry");
            const ZHLN::Remote::CachePolicy policy {
                .maxAge          = std::chrono::seconds(60),
                .cleanupInterval = std::chrono::hours(1),
            };
            ZHLN::Remote::DiskCache cache(dir, policy);
            const auto              bytes = MakeGLB(16);
            for (const char* name: {"read.bin", "exists.bin"}) {
                ZHLN::Test::ExpectTrue(cache.WriteAtomic(name, bytes));
                ZHLN::Test::ExpectTrue(AgeFile(dir / name, std::chrono::seconds(120)));
            }
            // The last write may sweep read.bin, so plant it again after that
            // sweep to prove that the individual lookup cannot be throttled.
            std::ofstream(dir / "read.bin", std::ios::binary) << "old";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "read.bin", std::chrono::seconds(120)));
            ZHLN::Test::ExpectFalse(cache.Read("read.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "read.bin"));
            ZHLN::Test::ExpectFalse(cache.Exists("exists.bin"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "exists.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cache_hits_do_not_renew_the_download_time() {
            const auto              dir = ScratchDir("no-sliding-expiry");
            ZHLN::Remote::DiskCache cache(dir, {.maxAge = std::chrono::hours(1)});
            const auto              bytes = MakeGLB(16);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("fresh.bin", bytes));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "fresh.bin", std::chrono::minutes(30)));
            const auto written = std::filesystem::last_write_time(dir / "fresh.bin");
            ZHLN::Test::ExpectTrue(cache.Read("fresh.bin", IsGLB).has_value());
            ZHLN::Test::ExpectTrue(cache.Read("fresh.bin", nullptr).has_value());
            ZHLN::Test::ExpectTrue(std::filesystem::last_write_time(dir / "fresh.bin") == written);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> ordinary_reads_sweep_unrequested_expired_entries() {
            const auto              dir = ScratchDir("read-sweep");
            ZHLN::Remote::DiskCache cache(
                dir, {
                         .maxAge          = std::chrono::seconds(60),
                         .cleanupInterval = std::chrono::seconds::zero(),
                     }
            );
            std::ofstream(dir / "forgotten.bin", std::ios::binary) << "old";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "forgotten.bin", std::chrono::seconds(120)));
            ZHLN::Test::ExpectFalse(cache.Read("never-cached.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "forgotten.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> writes_sweep_even_when_read_side_maintenance_is_not_due() {
            const auto              dir = ScratchDir("write-sweep");
            ZHLN::Remote::DiskCache cache(
                dir, {
                         .maxAge          = std::chrono::seconds(60),
                         .cleanupInterval = std::chrono::hours(1),
                     }
            );
            std::ofstream(dir / "forgotten.bin", std::ios::binary) << "old";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "forgotten.bin", std::chrono::seconds(120)));
            ZHLN::Test::ExpectFalse(cache.Read("never-cached.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectTrue(std::filesystem::exists(dir / "forgotten.bin")); // sweep was throttled
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("new.bin", MakeGLB(16)));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "forgotten.bin"));
            ZHLN::Test::ExpectTrue(cache.Exists("new.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> zero_limits_opt_out_and_invalid_bytes_are_still_removed() {
            const auto dir = ScratchDir("opt-out");
            std::ofstream(dir / "old.bin", std::ios::binary) << "cached";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "old.bin", std::chrono::hours(24 * 365)));
            ZHLN::Remote::DiskCache cache(dir, {.maxAge = std::chrono::seconds::zero(), .maxBytes = 0});
            ZHLN::Test::ExpectTrue(cache.Read("old.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectFalse(cache.Read("old.bin", IsGLB).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "old.bin"));
            std::ofstream(dir / "empty.bin", std::ios::binary).close();
            ZHLN::Test::ExpectFalse(cache.Read("empty.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "empty.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> migration_preserves_expiry_and_invalidation_cannot_resurrect_legacy_bytes() {
            const auto              dir = ScratchDir("legacy-expiry");
            ZHLN::Remote::DiskCache cache(dir, {.maxAge = std::chrono::seconds(60)});
            std::ofstream(dir / "old.glb", std::ios::binary) << "cached";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "old.glb", std::chrono::seconds(120)));
            ZHLN::Test::ExpectFalse(cache.Read("old.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "old.bin"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "old.glb"));

            std::ofstream(dir / "manual.glb", std::ios::binary) << "cached";
            cache.Invalidate("manual.bin");
            ZHLN::Test::ExpectFalse(cache.Read("manual.bin", AnyNonEmpty).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "manual.glb"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> the_budget_evicts_oldest_downloads_and_keeps_the_just_written_file() {
            const auto                 dir = ScratchDir("byte-budget");
            ZHLN::Remote::DiskCache    cache(dir, {.maxAge = std::chrono::seconds::zero(), .maxBytes = 8});
            const std::vector<uint8_t> bytes(4, 42);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("old.bin", bytes));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "old.bin", std::chrono::hours(2)));
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("middle.bin", bytes));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "middle.bin", std::chrono::hours(1)));
            ZHLN::Test::ExpectTrue(cache.Read("old.bin", AnyNonEmpty).has_value()); // not a freshness renewal
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("new.bin", bytes));
            ZHLN::Test::ExpectFalse(cache.Exists("old.bin"));
            ZHLN::Test::ExpectTrue(cache.Exists("middle.bin"));
            ZHLN::Test::ExpectTrue(cache.Exists("new.bin"));
            ZHLN::Test::ExpectEq(std::filesystem::file_size(dir / "middle.bin") + std::filesystem::file_size(dir / "new.bin"), uintmax_t {8});

            // Replacing a file counts its new size only, not old plus new.
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("new.bin", bytes));
            ZHLN::Test::ExpectTrue(cache.Exists("middle.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> reopening_enforces_the_budget_without_a_download() {
            const auto dir = ScratchDir("startup-budget");
            std::ofstream(dir / "old.bin", std::ios::binary) << "1234";
            std::ofstream(dir / "new.bin", std::ios::binary) << "5678";
            ZHLN::Test::ExpectTrue(AgeFile(dir / "old.bin", std::chrono::hours(2)));
            ZHLN::Remote::DiskCache cache(dir, {.maxBytes = 4});
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "old.bin"));
            ZHLN::Test::ExpectTrue(cache.Exists("new.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> oversized_and_failed_writes_preserve_existing_entries_and_leave_no_temps() {
            const auto                 dir = ScratchDir("failed-writes");
            ZHLN::Remote::DiskCache    cache(dir, {.maxBytes = 8});
            const std::vector<uint8_t> bytes(4, 42);
            const std::vector<uint8_t> tooLarge(9, 43);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("keep.bin", bytes));
            ZHLN::Test::ExpectFalse(cache.WriteAtomic("huge.bin", tooLarge));
            ZHLN::Test::ExpectFalse(cache.WriteAtomic("keep.bin", tooLarge));
            ZHLN::Test::ExpectFalse(cache.Exists("huge.bin"));
            std::filesystem::create_directories(dir / "blocked.bin");
            std::ofstream(dir / "blocked.bin" / "untouched.bin") << "not cache data";
            ZHLN::Test::ExpectFalse(cache.WriteAtomic("blocked.bin", bytes)); // rename fails
            const auto hit = cache.Read("keep.bin", AnyNonEmpty);
            ZHLN::Test::ExpectTrue(hit && *hit == bytes);
            ZHLN::Test::ExpectTrue(std::filesystem::exists(dir / "blocked.bin" / "untouched.bin"));
            for (const auto& entry: std::filesystem::directory_iterator(dir)) {
                ZHLN::Test::ExpectTrue(entry.path().filename().string().find(".tmp-") == std::string::npos);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> sweeps_clean_abandoned_temps_but_do_not_follow_links_or_subdirectories() {
            const auto dir  = ScratchDir("safe-sweep");
            const auto root = dir / "cache";
            std::filesystem::create_directories(root / "nested");
            for (const auto& file:
                 {root / "old.bin.tmp-1-2", root / "old.bin.tmp-1-2-3", root / "recent.bin.tmp-1-3", root / "nested" / "keep.bin", dir / "outside.bin"}) {
                std::ofstream(file, std::ios::binary) << "cached";
            }
            ZHLN::Test::ExpectTrue(AgeFile(root / "old.bin.tmp-1-2", std::chrono::hours(48)));
            ZHLN::Test::ExpectTrue(AgeFile(root / "old.bin.tmp-1-2-3", std::chrono::hours(48)));
            ZHLN::Test::ExpectTrue(AgeFile(root / "nested" / "keep.bin", std::chrono::hours(48)));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "outside.bin", std::chrono::hours(48)));
            std::error_code linkError;
            std::filesystem::create_symlink(dir / "outside.bin", root / "linked.bin", linkError);
            ZHLN::Remote::DiskCache cache(root, {.maxAge = std::chrono::seconds::zero(), .maxBytes = 1});
            ZHLN::Test::ExpectFalse(std::filesystem::exists(root / "old.bin.tmp-1-2"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(root / "old.bin.tmp-1-2-3"));
            ZHLN::Test::ExpectTrue(std::filesystem::exists(root / "recent.bin.tmp-1-3"));
            ZHLN::Test::ExpectTrue(std::filesystem::exists(root / "nested" / "keep.bin"));
            ZHLN::Test::ExpectTrue(std::filesystem::exists(dir / "outside.bin"));
            // Windows can require privileges to create a symlink; exercise it
            // whenever the filesystem allows the fixture to create one.
            if (!linkError) {
                ZHLN::Test::ExpectTrue(std::filesystem::is_symlink(root / "linked.bin"));
                ZHLN::Test::ExpectFalse(cache.Read("linked.bin", AnyNonEmpty).has_value());
                ZHLN::Test::ExpectFalse(cache.Exists("linked.bin"));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> unsafe_keys_cannot_read_write_or_invalidate_outside_the_cache() {
            const auto dir = ScratchDir("unsafe-keys");
            std::ofstream(dir / "outside.bin", std::ios::binary) << "untouched";
            ZHLN::Remote::DiskCache        cache(dir / "cache");
            const auto                     bytes = MakeGLB(16);
            const std::vector<std::string> unsafe {
                "",
                ".",
                "..",
                "../outside.bin",
                "..\\outside.bin",
                "nested/asset.bin",
                "a:stream",
                "reserved.bin.tmp-1-2",
                (dir / "outside.bin").string(),
                std::string("bad\0.bin", 8),
            };
            for (const auto& key: unsafe) {
                ZHLN::Test::ExpectFalse(cache.WriteAtomic(key, bytes));
                ZHLN::Test::ExpectFalse(cache.Read(key, AnyNonEmpty).has_value());
                ZHLN::Test::ExpectFalse(cache.Exists(key));
                cache.Invalidate(key);
            }
            ZHLN::Test::ExpectEq(std::filesystem::file_size(dir / "outside.bin"), uintmax_t {9});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> very_large_durations_do_not_overflow_the_filesystem_clock() {
            const auto              dir = ScratchDir("large-duration");
            ZHLN::Remote::DiskCache cache(
                dir, {
                         .maxAge          = std::chrono::seconds::max(),
                         .cleanupInterval = std::chrono::seconds::max(),
                     }
            );
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("old.bin", MakeGLB(16)));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "old.bin", std::chrono::hours(24 * 365)));
            ZHLN::Test::ExpectTrue(cache.Read("old.bin", IsGLB).has_value());
            ZHLN::Test::ExpectTrue(cache.Exists("old.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> future_timestamps_do_not_grant_unlimited_freshness() {
            const auto              dir = ScratchDir("future-time");
            ZHLN::Remote::DiskCache cache(dir);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("future.bin", MakeGLB(16)));
            ZHLN::Test::ExpectTrue(AgeFile(dir / "future.bin", -std::chrono::hours(24)));
            ZHLN::Test::ExpectFalse(cache.Read("future.bin", IsGLB).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "future.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> copies_and_independent_caches_coordinate_writes_reads_and_sweeps() {
            const auto                      dir = ScratchDir("shared-maintenance");
            const ZHLN::Remote::CachePolicy policy {.maxBytes = 1024, .cleanupInterval = std::chrono::seconds::zero()};
            ZHLN::Remote::DiskCache         cache(dir, policy);
            auto                            copy = cache;
            ZHLN::Remote::DiskCache         peer(dir / ".", policy); // same root, independently opened
            const auto                      a = MakeGLB(64);
            const auto                      b = MakeGLB(128);
            std::atomic<bool>               valid {true};
            const auto                      writer = [&](ZHLN::Remote::DiskCache& target, const std::vector<uint8_t>& bytes) {
                for (int i = 0; i < 50; ++i) {
                    if (!target.WriteAtomic("shared.bin", bytes)) {
                        valid.store(false, std::memory_order::relaxed);
                    }
                }
            };
            std::thread first([&] { writer(cache, a); });
            std::thread second([&] { writer(copy, b); });
            for (int i = 0; i < 100; ++i) {
                // Each read sweeps, including while the other instances write.
                if (const auto hit = peer.Read("shared.bin", IsGLB); hit && *hit != a && *hit != b) {
                    valid.store(false, std::memory_order::relaxed);
                }
            }
            first.join();
            second.join();
            ZHLN::Test::ExpectTrue(valid.load(std::memory_order::relaxed));
            const auto hit = peer.Read("shared.bin", IsGLB);
            ZHLN::Test::ExpectTrue(hit && (*hit == a || *hit == b));
            return {};
        }

        // --- Persisted origin metadata and complete-body integrity ---
        std::expected<void, ZHLN::ErrorCode> origin_metadata_round_trips_and_has_a_separate_revalidation_clock() {
            const auto                      dir = ScratchDir("origin-metadata");
            ZHLN::Remote::DiskCache         cache(dir, {.revalidateAfter = std::chrono::minutes(5)});
            const ZHLN::Remote::CacheOrigin origin {.url = "https://assets.example/test.glb", .etag = "W/\"opaque-v1\""};
            const auto                      bytes = MakeGLB(16);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("model.bin", bytes, origin));
            const auto bodyTime = std::filesystem::last_write_time(dir / "model.bin");
            auto       entry    = cache.ReadEntry("model.bin", IsGLB);
            if (!ZHLN::Test::ExpectTrue(entry.has_value()))
                return {};
            ZHLN::Test::ExpectTrue(entry->origin == origin);
            ZHLN::Test::ExpectFalse(entry->needsRevalidation);
            ZHLN::Test::ExpectTrue(AgeFile(dir / "model.bin.meta", std::chrono::minutes(10)));
            entry = cache.ReadEntry("model.bin", IsGLB);
            if (!ZHLN::Test::ExpectTrue(entry.has_value()))
                return {};
            ZHLN::Test::ExpectTrue(entry->needsRevalidation);
            ZHLN::Test::ExpectTrue(cache.MarkRevalidated("model.bin", *entry, origin));
            entry = cache.ReadEntry("model.bin", IsGLB);
            ZHLN::Test::ExpectTrue(entry && !entry->needsRevalidation);
            ZHLN::Test::ExpectTrue(std::filesystem::last_write_time(dir / "model.bin") == bodyTime);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> the_checksum_rejects_same_size_corruption_that_still_passes_the_glb_header() {
            const auto              dir = ScratchDir("body-integrity");
            ZHLN::Remote::DiskCache cache(dir);
            auto                    bytes = MakeGLB(16);
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("model.bin", bytes, {.url = "https://assets.example/model", .etag = "\"v1\""}));
            bytes.back() ^= 0xFF;
            ZHLN::Test::ExpectTrue(IsGLB(bytes)); // The format/header alone cannot detect this edit.
            {
                std::ofstream out(dir / "model.bin", std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            ZHLN::Test::ExpectFalse(cache.Read("model.bin", IsGLB).has_value());
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "model.bin"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "model.bin.meta"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> missing_metadata_requires_revalidation_and_malformed_metadata_is_not_trusted() {
            const auto                      dir = ScratchDir("metadata-fail-closed");
            ZHLN::Remote::DiskCache         cache(dir, {.revalidateAfter = std::chrono::hours(1)});
            const auto                      bytes = MakeGLB(16);
            const ZHLN::Remote::CacheOrigin origin {.url = "https://assets.example/model", .etag = "\"v1\""};
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("model.bin", bytes, origin));
            std::filesystem::remove(dir / "model.bin.meta");
            auto legacy = cache.ReadEntry("model.bin", IsGLB);
            ZHLN::Test::ExpectTrue(legacy && legacy->needsRevalidation && legacy->origin.url.empty());
            for (const auto& bad: {std::string("broken metadata"), std::string(64 * 1024, 'x')}) {
                ZHLN::Test::ExpectTrue(cache.WriteAtomic("model.bin", bytes, origin));
                std::ofstream(dir / "model.bin.meta", std::ios::binary | std::ios::trunc) << bad;
                ZHLN::Test::ExpectFalse(cache.ReadEntry("model.bin", IsGLB).has_value());
                ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "model.bin.meta"));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> mismatched_metadata_and_body_never_authorize_a_conditional_hit() {
            const auto              dir = ScratchDir("mismatched-pair");
            ZHLN::Remote::DiskCache cache(dir);
            const auto              first  = MakeGLB(16);
            auto                    second = first;
            second.back() ^= 0xFF;
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("a.bin", first, {.url = "https://assets.example/a", .etag = "\"a\""}));
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("b.bin", second, {.url = "https://assets.example/b", .etag = "\"b\""}));
            std::filesystem::copy_file(dir / "a.bin.meta", dir / "b.bin.meta", std::filesystem::copy_options::overwrite_existing);
            ZHLN::Test::ExpectFalse(cache.ReadEntry("b.bin", IsGLB).has_value());
            ZHLN::Test::ExpectTrue(cache.ReadEntry("a.bin", IsGLB).has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_late_304_cannot_clobber_a_replacement_or_accept_a_corrupted_snapshot() {
            const auto              dir = ScratchDir("snapshot-check");
            ZHLN::Remote::DiskCache cache(dir);
            const auto              first  = MakeGLB(16);
            auto                    second = first;
            second.back() ^= 0xFF;
            const ZHLN::Remote::CacheOrigin a {.url = "https://assets.example/model", .etag = "\"a\""};
            const ZHLN::Remote::CacheOrigin b {.url = a.url, .etag = "\"b\""};
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("model.bin", first, a));
            auto snapshot = cache.ReadEntry("model.bin", IsGLB);
            if (!ZHLN::Test::ExpectTrue(snapshot.has_value()))
                return {};
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("model.bin", second, b));
            ZHLN::Test::ExpectFalse(cache.MarkRevalidated("model.bin", *snapshot, a));
            auto replaced = cache.ReadEntry("model.bin", IsGLB);
            ZHLN::Test::ExpectTrue(replaced && replaced->data == second && replaced->origin == b);
            if (!replaced)
                return {};
            {
                std::ofstream out(dir / "model.bin", std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(first.data()), static_cast<std::streamsize>(first.size()));
            }
            ZHLN::Test::ExpectFalse(cache.MarkRevalidated("model.bin", *replaced, b));
            ZHLN::Test::ExpectFalse(cache.Exists("model.bin"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> metadata_is_budgeted_and_evicted_with_its_body_and_orphans_are_removed() {
            const auto                      dir   = ScratchDir("metadata-budget");
            const auto                      bytes = MakeGLB(16);
            const ZHLN::Remote::CacheOrigin origin {.url = "https://assets.example/model", .etag = "\"v1\""};
            ZHLN::Remote::DiskCache         seed(dir);
            ZHLN::Test::ExpectTrue(seed.WriteAtomic("a.bin", bytes, origin));
            const auto              footprint = std::filesystem::file_size(dir / "a.bin") + std::filesystem::file_size(dir / "a.bin.meta");
            ZHLN::Remote::DiskCache bounded(dir, {.maxBytes = footprint});
            ZHLN::Test::ExpectTrue(AgeFile(dir / "a.bin", std::chrono::hours(1)));
            ZHLN::Test::ExpectTrue(bounded.WriteAtomic("b.bin", bytes, origin));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "a.bin"));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "a.bin.meta"));
            ZHLN::Test::ExpectTrue(bounded.Exists("b.bin"));
            bounded.Invalidate("b.bin");
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "b.bin.meta"));
            ZHLN::Remote::DiskCache tooSmall(dir, {.maxBytes = footprint - 1});
            ZHLN::Test::ExpectFalse(tooSmall.WriteAtomic("c.bin", bytes, origin));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "c.bin"));
            std::ofstream(dir / "orphan.bin.meta") << "orphan";
            ZHLN::Remote::DiskCache reopened(dir);
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "orphan.bin.meta"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> local_replacement_discards_http_metadata_and_failed_metadata_commit_is_a_miss() {
            const auto                      dir = ScratchDir("metadata-write-failure");
            ZHLN::Remote::DiskCache         cache(dir);
            const auto                      bytes = MakeGLB(16);
            const ZHLN::Remote::CacheOrigin origin {.url = "https://assets.example/model", .etag = "\"v1\""};
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("local.bin", bytes, origin));
            ZHLN::Test::ExpectTrue(cache.WriteAtomic("local.bin", bytes));
            ZHLN::Test::ExpectFalse(std::filesystem::exists(dir / "local.bin.meta"));
            const auto local = cache.ReadEntry("local.bin", IsGLB);
            ZHLN::Test::ExpectTrue(local && local->needsRevalidation && local->origin.url.empty());
            std::filesystem::create_directories(dir / "blocked.bin.meta");
            std::ofstream(dir / "blocked.bin.meta" / "keep.txt") << "not metadata";
            ZHLN::Test::ExpectFalse(cache.WriteAtomic("blocked.bin", bytes, origin));
            ZHLN::Test::ExpectFalse(cache.Exists("blocked.bin"));
            ZHLN::Test::ExpectTrue(std::filesystem::exists(dir / "blocked.bin.meta" / "keep.txt"));
            for (const auto& entry: std::filesystem::directory_iterator(dir)) {
                ZHLN::Test::ExpectTrue(entry.path().filename().string().find(".tmp-") == std::string::npos);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> concurrent_instances_commit_the_body_and_its_metadata_as_one_in_process_entry() {
            const auto                      dir = ScratchDir("concurrent-origin-pairs");
            ZHLN::Remote::DiskCache         first(dir);
            ZHLN::Remote::DiskCache         second(dir);
            auto                            reader = first;
            const auto                      a      = MakeGLB(16);
            const auto                      b      = MakeGLB(32);
            const ZHLN::Remote::CacheOrigin originA {.url = "https://assets.example/model", .etag = "\"a\""};
            const ZHLN::Remote::CacheOrigin originB {.url = originA.url, .etag = "\"b\""};
            ZHLN::Test::ExpectTrue(first.WriteAtomic("model.bin", a, originA));
            std::atomic<bool> intact {true};
            std::jthread      writerA([&] {
                for (int i = 0; i < 40; ++i)
                    if (!first.WriteAtomic("model.bin", a, originA))
                        intact.store(false);
            });
            std::jthread      writerB([&] {
                for (int i = 0; i < 40; ++i)
                    if (!second.WriteAtomic("model.bin", b, originB))
                        intact.store(false);
            });
            for (int i = 0; i < 80; ++i) {
                const auto entry = reader.ReadEntry("model.bin", IsGLB);
                if (!entry || !((entry->data == a && entry->origin == originA) || (entry->data == b && entry->origin == originB)))
                    intact.store(false);
            }
            writerA.join();
            writerB.join();
            ZHLN::Test::ExpectTrue(intact.load());
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

            const auto        id     = fetcher.Request(url, AnyNonEmpty, false);
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

        std::expected<void, ZHLN::ErrorCode> a_completed_transfer_can_be_reaped_without_losing_its_result() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                   dir     = ScratchDir("reap");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir), 10);
            const auto                   id      = fetcher.Request(server.Url("/ok"), AnyNonEmpty, false);
            const FetchStatus            status  = WaitTerminal(fetcher, id);
            ZHLN::Test::ExpectEq(status, FetchStatus::Succeeded);
            if (status != FetchStatus::Succeeded) {
                return {};
            }

            // A worker publishes its result just before marking itself done.
            // Give Poll repeated chances to reap it: destroying a joinable
            // Job would terminate the entire test process before Take.
            for (int i = 0; i < 20; ++i) {
                fetcher.Poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto result = fetcher.Take(id);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result.has_value()) {
                ZHLN::Test::ExpectEq(std::string_view(reinterpret_cast<const char*>(result->data.data()), result->data.size()), std::string_view("hello"));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> a_second_request_for_the_same_url_is_a_cache_hit() {
            ZHLN::HTTP::LoopbackServer server;
            if (!ZHLN::Test::ExpectTrue(server.IsListening())) {
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            }

            const auto                   dir   = ScratchDir("cachehit");
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(dir, {.revalidateAfter = std::chrono::minutes(5)}), 10);
            const std::string            url   = server.Url("/ok");

            const auto first = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, first), FetchStatus::Succeeded);
            ZHLN::Test::ExpectTrue(fetcher.Take(first).has_value()); // drain the slot

            const uint64_t requestsBefore = server.Requests();
            const auto     second         = fetcher.Request(url, AnyNonEmpty, false);
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

            const auto id = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, id), FetchStatus::Succeeded);
            auto first = fetcher.Take(id);
            ZHLN::Test::ExpectTrue(first.has_value());
            // The id is back to Idle, and the slot is empty: a second Take
            // gets nothing, not a copy.
            ZHLN::Test::ExpectEq(fetcher.Status(id), FetchStatus::Idle);
            ZHLN::Test::ExpectFalse(fetcher.Take(id).has_value());

            // Unknown ids are Idle, not an out-of-bounds.
            ZHLN::Test::ExpectEq(fetcher.Status(ZHLN::Remote::FetchHandle {}), FetchStatus::Idle);
            ZHLN::Test::ExpectFalse(fetcher.Take(ZHLN::Remote::FetchHandle {}).has_value());
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
            const auto     id = fetcher.Request(server.Url("/notfound"), AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, id), FetchStatus::Failed);
            auto result = fetcher.Take(id);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result.has_value()) {
                ZHLN::Test::ExpectTrue(result->errorMessage.find("HTTP 404") != std::string::npos);
                ZHLN::Test::ExpectTrue(result->data.empty());
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> force_refresh_is_unconditional_even_when_an_etag_is_available() {
            ZHLN::HTTP::LoopbackServer server;
            server.SetResource("/asset", {.body = "hello", .etag = "\"v1\""});
            if (!ZHLN::Test::ExpectTrue(server.IsListening()))
                return std::unexpected(RemoteAssetTestError::ServerUnavailable);
            ZHLN::Remote::AsyncAssetFetcher fetcher(ZHLN::Remote::DiskCache(ScratchDir("force-with-etag")), 10);
            auto                            first = fetcher.FetchSync(server.Url("/asset"));
            ZHLN::Test::ExpectTrue(first.has_value());
            const auto forced = fetcher.Request(server.Url("/asset"), AnyNonEmpty, true);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, forced), FetchStatus::Succeeded);
            const auto refreshed = fetcher.Take(forced);
            ZHLN::Test::ExpectTrue(refreshed && !refreshed->fromCache);
            ZHLN::Test::ExpectEq(server.Requests(), uint64_t {2});
            ZHLN::Test::ExpectEq(server.ConditionalRequests(), uint64_t {0});
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

            const auto first = fetcher.Request(url, AnyNonEmpty, false);
            ZHLN::Test::ExpectEq(WaitTerminal(fetcher, first), FetchStatus::Succeeded);
            ZHLN::Test::ExpectTrue(fetcher.Take(first).has_value()); // drain the slot

            const uint64_t before = server.Requests();
            const auto     second = fetcher.Request(url, AnyNonEmpty, true);
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
    ZHLN::TaskSystem::Scope taskScope;
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
