// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Defer.hpp>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace {

enum class DeferTestError : uint8_t { UnexpectedCount = 1 };

struct DeferTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> unnamed_guards_run_in_reverse_order() {
            int sequence = 0;
            {
                ZHLN::defer _([&] { sequence = sequence * 10 + 1; });
                ZHLN::defer _([&] { sequence = sequence * 10 + 2; });
            }
            if (sequence != 21) return std::unexpected(DeferTestError::UnexpectedCount);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> early_return_runs_cleanup() {
            int count = 0;
            auto fail = [&]() -> bool {
                ZHLN::defer _([&] { ++count; });
                return false;
            };
            if (fail() || count != 1) return std::unexpected(DeferTestError::UnexpectedCount);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> moved_and_dismissed_guards_run_at_most_once() {
            int count = 0;
            {
                ZHLN::defer first([&] { ++count; });
                auto second = std::move(first);
            }
            {
                ZHLN::defer guard([&] { ++count; });
                guard.Dismiss();
            }
            if (count != 1) return std::unexpected(DeferTestError::UnexpectedCount);
            return {};
        }
    };
};

static_assert(!std::is_copy_constructible_v<ZHLN::defer<void (*)()>>);
static_assert(std::is_move_constructible_v<ZHLN::defer<void (*)()>>);

} // namespace

auto RunDeferSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<DeferTestSuite>();
}
