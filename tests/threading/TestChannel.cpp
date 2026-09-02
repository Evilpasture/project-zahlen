// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/Channel.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <atomic>
#include <expected>

enum class ChannelTestError : uint32_t {
    FifoOrderingViolated ZHLN_ANNOTATION(ZHLN::Description<"Channel popped messages out of chronological order.">{}) = 1,
    MessageLost ZHLN_ANNOTATION(ZHLN::Description<"One or more messages were dropped during channel push/pop.">{}),
};

struct ChannelTestSuite {
    ChannelTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ChannelTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // --- 1. Basic Push, TryPop & Size ---
        std::expected<void, ZHLN::Error> basic_push_try_pop() {
            ZHLN::Channel<int> chan;

            ZHLN::Test::ExpectEq(chan.Size(), static_cast<size_t>(0));

            chan.Push(10);
            chan.Push(20);
            chan.Push(30);

            ZHLN::Test::ExpectEq(chan.Size(), static_cast<size_t>(3));

            int val = 0;
            ZHLN::Test::ExpectTrue(chan.TryPop(val));
            ZHLN::Test::ExpectEq(val, 10);

            ZHLN::Test::ExpectTrue(chan.TryPop(val));
            ZHLN::Test::ExpectEq(val, 20);

            ZHLN::Test::ExpectTrue(chan.TryPop(val));
            ZHLN::Test::ExpectEq(val, 30);

            // Pop on empty channel should safely fail without stalling
            ZHLN::Test::ExpectFalse(chan.TryPop(val));
            ZHLN::Test::ExpectEq(chan.Size(), static_cast<size_t>(0));

            return {};
        }

        // --- 2. Concurrent Producer-Consumer over TaskSystem ---
        std::expected<void, ZHLN::Error> concurrent_channel_streaming() {
            ZHLN::Channel<int> chan;
            constexpr int      kTotalMessages = 1000;
            std::atomic<int>   receivedSum {0};
            std::atomic<int>   receivedCount {0};

            // Produce in parallel
            ZHLN::TaskSystem::ParallelFor(kTotalMessages, 64, [&](uint32_t start, uint32_t end, uint32_t) {
                for (uint32_t i = start; i < end; ++i) {
                    chan.Push(1);
                }
            });

            // Consume in parallel
            ZHLN::TaskSystem::ParallelFor(kTotalMessages, 64, [&](uint32_t, uint32_t, uint32_t) {
                int outVal = 0;
                if (chan.TryPop(outVal)) {
                    receivedSum.fetch_add(outVal, std::memory_order::relaxed);
                    receivedCount.fetch_add(1, std::memory_order::relaxed);
                }
            });

            // Drain any remaining messages sequentially
            int outVal = 0;
            while (chan.TryPop(outVal)) {
                receivedSum.fetch_add(outVal, std::memory_order::relaxed);
                receivedCount.fetch_add(1, std::memory_order::relaxed);
            }

            ZHLN::Test::ExpectEq(receivedCount.load(), kTotalMessages);
            ZHLN::Test::ExpectEq(receivedSum.load(), kTotalMessages);

            return {};
        }
    };
};

// Exported for the threading group binary (RunThreadingTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunChannelSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ChannelTestSuite>();
}

