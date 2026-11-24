// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

/**
 * @brief Address as an integer, so that failures print as numbers and the
 * suite never has to format a raw pointer.
 */
auto Addr(const void* address) -> size_t {
    return static_cast<size_t>(std::bit_cast<uintptr_t>(address));
}

} // namespace

namespace {

struct PlatformTestSuite {
    struct Tests {
        // --- 1. Page Primitives ---
        std::expected<void, ZHLN::ErrorCode> page_size_and_alignment() {
            const size_t page = ZHLN::GetPageSize();

            ZHLN::Test::ExpectGt(page, 0);
            ZHLN::Test::ExpectTrue(std::has_single_bit(page));

            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(0), static_cast<size_t>(0));
            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(1), page);
            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(page), page);
            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(page + 1), page * 2);

            return {};
        }

        // --- 2. Guarded Regions ---
        std::expected<void, ZHLN::ErrorCode> guarded_region_layout() {
            const size_t page = ZHLN::GetPageSize();

            const ZHLN::GuardedRegion region = ZHLN::AllocateGuardedRegion(1000);
            if (!ZHLN::Test::ExpectTrue(region.valid())) {
                return {};
            }

            ZHLN::Test::ExpectEq(Addr(region.base) % page, static_cast<size_t>(0));
            ZHLN::Test::ExpectEq(Addr(region.begin), Addr(region.base) + page);
            ZHLN::Test::ExpectEq(Addr(region.end), Addr(region.begin) + ZHLN::AlignUpToPage(1000));
            ZHLN::Test::ExpectEq(region.size, (Addr(region.end) - Addr(region.begin)) + (page * 2));

            // The whole payload is writable; the guard pages around it are what
            // makes stepping out of bounds a fault instead of silent corruption.
            std::memset(region.begin, 0x5A, Addr(region.end) - Addr(region.begin));

            ZHLN::FreeGuardedRegion(region);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> guarded_region_round_trip() {
            constexpr size_t SIZES[] = {1, 4096, 64 * 1024, 512 * 1024};

            for (const size_t bytes: SIZES) {
                const ZHLN::GuardedRegion region = ZHLN::AllocateGuardedRegion(bytes);
                if (!ZHLN::Test::ExpectTrue(region.valid())) {
                    continue;
                }

                const size_t payload = Addr(region.end) - Addr(region.begin);
                ZHLN::Test::ExpectGe(payload, bytes);

                std::memset(region.begin, 0xAB, payload);
                ZHLN::FreeGuardedRegion(region);
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> guarded_region_rejects_empty_request() {
            ZHLN::Test::ExpectFalse(ZHLN::AllocateGuardedRegion(0).valid());
            return {};
        }

        // --- 3. Cached Stack Bounds ---
        std::expected<void, ZHLN::ErrorCode> stack_bounds_round_trip() {
            const ZHLN::StackBounds bounds = ZHLN::GetCurrentStackBounds();

            // Either the platform records stack bounds for us -- both fields
            // set, base above limit -- or it tracks nothing and both are null.
            const bool tracked = (bounds.base != nullptr);
            ZHLN::Test::ExpectTrue(tracked == (bounds.limit != nullptr));
            if (tracked) {
                ZHLN::Test::ExpectGt(Addr(bounds.base), Addr(bounds.limit));
            }

            // Writing the current bounds back must not perturb them.
            ZHLN::SetCurrentStackBounds(bounds);
            const ZHLN::StackBounds after = ZHLN::GetCurrentStackBounds();
            ZHLN::Test::ExpectTrue(Addr(after.base) == Addr(bounds.base));
            ZHLN::Test::ExpectTrue(Addr(after.limit) == Addr(bounds.limit));

            return {};
        }

        // --- 4. Runtime shared libraries ---
        std::expected<void, ZHLN::ErrorCode> shared_library_open_and_symbol() {
            ZHLN::SharedLibrary lib;
            ZHLN::Test::ExpectFalse(lib.IsOpen());
            ZHLN::Test::ExpectFalse(lib.Open(""));
            ZHLN::Test::ExpectFalse(lib.Open("zahlen_no_such_library_xyz"));

#if defined(_WIN32)
            static constexpr const char* const names[] = {"kernel32.dll"};
            if (!ZHLN::Test::ExpectTrue(lib.OpenAny(names))) {
                return {};
            }
            using TickFn          = unsigned long(WINAPI*)();
            const TickFn tick     = lib.Symbol<TickFn>("GetTickCount");
            ZHLN::Test::ExpectTrue(tick != nullptr);
            if (tick != nullptr) {
                tick();
            }
#else
            static constexpr const char* const names[] = {
                "libc.so.6",
                "libc.so",
                "/usr/lib/libSystem.B.dylib",
                "libSystem.B.dylib",
            };
            if (!ZHLN::Test::ExpectTrue(lib.OpenAny(names))) {
                return {};
            }
            using StrlenFn               = size_t (*)(const char*);
            const StrlenFn strlen_fn     = lib.Symbol<StrlenFn>("strlen");
            if (!ZHLN::Test::ExpectTrue(strlen_fn != nullptr)) {
                return {};
            }
            ZHLN::Test::ExpectEq(strlen_fn("abc"), static_cast<size_t>(3));
#endif
            ZHLN::Test::ExpectTrue(lib.GetSymbol("zahlen_no_such_symbol_xyz") == nullptr);
            lib.Close();
            ZHLN::Test::ExpectFalse(lib.IsOpen());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> shared_library_naming_and_search_path() {
            ZHLN::Test::ExpectFalse(ZHLN::SharedLibrary::kSuffix.empty());
            ZHLN::Test::ExpectEq(ZHLN::SharedLibrary::kSuffix.front(), '.');
            ZHLN::Test::ExpectTrue(
                ZHLN::SharedLibrary::kEnvPathSeparator == ':' ||
                ZHLN::SharedLibrary::kEnvPathSeparator == ';'
            );

            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::HasLibraryExtension("libfoo.so"));
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::HasLibraryExtension("libfoo.dylib"));
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::HasLibraryExtension("FOO.DLL"));
            ZHLN::Test::ExpectFalse(ZHLN::SharedLibrary::HasLibraryExtension("freecam"));
            ZHLN::Test::ExpectFalse(ZHLN::SharedLibrary::HasLibraryExtension("libfoo.a"));

            ZHLN::Test::ExpectFalse(ZHLN::SharedLibrary::IsExplicitPath("freecam"));
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::IsExplicitPath("plugins/freecam"));
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::IsExplicitPath("plugins\\freecam"));
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::IsExplicitPath("libzahlen_plugin_freecam.so"));
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::IsExplicitPath("zahlen_plugin_freecam.dll"));

            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::FormatFileNames("").empty());
            const auto formatted = ZHLN::SharedLibrary::FormatFileNames("zahlen_plugin_freecam");
            ZHLN::Test::ExpectFalse(formatted.empty());
            for (const auto& name: formatted) {
                ZHLN::Test::ExpectTrue(name.ends_with(ZHLN::SharedLibrary::kSuffix));
                ZHLN::Test::ExpectTrue(name.find("zahlen_plugin_freecam") != std::string::npos);
            }

            const std::string sep(1, ZHLN::SharedLibrary::kEnvPathSeparator);
            const auto        split = ZHLN::SharedLibrary::SplitSearchPath(
                sep + "/alpha/plugins" + sep + sep + "/beta/plugins" + sep
            );
            if (ZHLN::Test::ExpectEq(split.size(), static_cast<size_t>(2))) {
                ZHLN::Test::ExpectEq(split[0].generic_string(), std::string {"/alpha/plugins"});
                ZHLN::Test::ExpectEq(split[1].generic_string(), std::string {"/beta/plugins"});
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> executable_and_module_directory_lookup() {
            const auto exeDir = ZHLN::FS::Paths::ExecutableDir();
            ZHLN::Test::ExpectFalse(exeDir.empty());
            ZHLN::Test::ExpectTrue(std::filesystem::is_directory(exeDir));

            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::ModulePath(nullptr).empty());
            ZHLN::Test::ExpectTrue(ZHLN::SharedLibrary::ModuleDirectory(nullptr).empty());

            static constexpr int kLocalAnchor = 42;
            const auto           localDir     = ZHLN::SharedLibrary::ModuleDirectory(&kLocalAnchor);
            ZHLN::Test::ExpectFalse(localDir.empty());
            ZHLN::Test::ExpectTrue(std::filesystem::is_directory(localDir));

            const auto runtimePath = ZHLN::SharedLibrary::RuntimeModulePath();
            const auto runtimeDir  = ZHLN::SharedLibrary::RuntimeModuleDirectory();
            ZHLN::Test::ExpectFalse(runtimePath.empty());
            ZHLN::Test::ExpectEq(runtimePath.parent_path().string(), runtimeDir.string());
            ZHLN::Test::ExpectTrue(std::filesystem::is_directory(runtimeDir));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> open_plugin_distinguishes_not_found_and_load_failed() {
            ZHLN::Test::TempDir   scratch;
            std::error_code       ec;
            const auto            envDir    = scratch.Path() / "env";
            const auto            searchDir = scratch.Path() / "search";
            ZHLN::Test::ExpectTrue(std::filesystem::create_directories(envDir, ec));
            ZHLN::Test::ExpectTrue(std::filesystem::create_directories(searchDir, ec));

            const std::filesystem::path dirs[] = {searchDir};

            // Nothing anywhere: success with no value, not an error.
            const auto missing = ZHLN::SharedLibrary::OpenPlugin(
                "definitely_missing_plugin_xyz",
                ZHLN::PluginSearchOptions {
                    .envPath           = envDir.string(),
                    .searchDirs        = dirs,
                    .allowSystemSearch = false,
                }
            );
            ZHLN::Test::ExpectTrue(missing.has_value());
            ZHLN::Test::ExpectFalse(missing.has_value() && missing->has_value());

            const auto names = ZHLN::SharedLibrary::FormatFileNames("zahlen_plugin_bogus");
            if (!ZHLN::Test::ExpectFalse(names.empty())) {
                return {};
            }
            // Directory matching a candidate plugin name must be skipped cleanly.
            ZHLN::Test::ExpectTrue(std::filesystem::create_directories(envDir / names.front(), ec));

            // Invalid regular file in searchDirs must report LoadFailed with its path.
            const auto badFile = searchDir / names.front();
            {
                std::ofstream out(badFile, std::ios::binary);
                out << "not a valid shared library";
            }
            // A file that exists and will not load is an error, and stops the
            // search rather than sliding on to the next candidate.
            const auto rejected = ZHLN::SharedLibrary::OpenPlugin(
                "bogus",
                ZHLN::PluginSearchOptions {
                    .envPath           = envDir.string(),
                    .searchDirs        = dirs,
                    .allowSystemSearch = false,
                }
            );
            ZHLN::Test::ExpectFalse(rejected.has_value());
            if (!rejected.has_value()) {
                ZHLN::Test::ExpectTrue(rejected.error() == ZHLN::SharedLibraryError::LoadFailed);
            }
            return {};
        }
    };
};

} // namespace
