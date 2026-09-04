// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Platform.hpp>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace {

/**
 * @brief Address as an integer, so that failures print as numbers and the
 * suite never has to format a raw pointer.
 */
auto Addr(const void* address) -> size_t {
    return static_cast<size_t>(std::bit_cast<uintptr_t>(address));
}

} // namespace

struct PlatformTestSuite {
    struct Tests {
        // --- 1. Page Primitives ---
        std::expected<void, ZHLN::Error> page_size_and_alignment() {
            const size_t page = ZHLN::GetPageSize();

            ZHLN::Test::ExpectTrue(page > 0);
            ZHLN::Test::ExpectTrue(std::has_single_bit(page));

            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(0), static_cast<size_t>(0));
            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(1), page);
            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(page), page);
            ZHLN::Test::ExpectEq(ZHLN::AlignUpToPage(page + 1), page * 2);

            return {};
        }

        // --- 2. Guarded Regions ---
        std::expected<void, ZHLN::Error> guarded_region_layout() {
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

        std::expected<void, ZHLN::Error> guarded_region_round_trip() {
            constexpr size_t SIZES[] = {1, 4096, 64 * 1024, 512 * 1024};

            for (const size_t bytes: SIZES) {
                const ZHLN::GuardedRegion region = ZHLN::AllocateGuardedRegion(bytes);
                if (!ZHLN::Test::ExpectTrue(region.valid())) {
                    continue;
                }

                const size_t payload = Addr(region.end) - Addr(region.begin);
                ZHLN::Test::ExpectTrue(payload >= bytes);

                std::memset(region.begin, 0xAB, payload);
                ZHLN::FreeGuardedRegion(region);
            }

            return {};
        }

        std::expected<void, ZHLN::Error> guarded_region_rejects_empty_request() {
            ZHLN::Test::ExpectFalse(ZHLN::AllocateGuardedRegion(0).valid());
            return {};
        }

        // --- 3. Cached Stack Bounds ---
        std::expected<void, ZHLN::Error> stack_bounds_round_trip() {
            const ZHLN::StackBounds bounds = ZHLN::GetCurrentStackBounds();

            // Either the platform records stack bounds for us -- both fields
            // set, base above limit -- or it tracks nothing and both are null.
            const bool tracked = (bounds.base != nullptr);
            ZHLN::Test::ExpectTrue(tracked == (bounds.limit != nullptr));
            if (tracked) {
                ZHLN::Test::ExpectTrue(Addr(bounds.base) > Addr(bounds.limit));
            }

            // Writing the current bounds back must not perturb them.
            ZHLN::SetCurrentStackBounds(bounds);
            const ZHLN::StackBounds after = ZHLN::GetCurrentStackBounds();
            ZHLN::Test::ExpectTrue(Addr(after.base) == Addr(bounds.base));
            ZHLN::Test::ExpectTrue(Addr(after.limit) == Addr(bounds.limit));

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunPlatformSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<PlatformTestSuite>();
}
