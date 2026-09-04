// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Loop.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Queue.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Core/SkipList.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cstdlib>
#include <expected>
#include <string>
#include <vector>

// ============================================================================
// Lifetime Tracker for Memory Leak and Destruction Verification
// ============================================================================

struct LifetimeTracker {
    static inline int activeInstances = 0;
    int               id              = 0;

    explicit LifetimeTracker(int _id = 0) noexcept: id(_id) {
        activeInstances++;
    }
    LifetimeTracker(const LifetimeTracker& o) noexcept: id(o.id) {
        activeInstances++;
    }
    LifetimeTracker(LifetimeTracker&& o) noexcept: id(o.id) {
        activeInstances++;
    }
    ~LifetimeTracker() noexcept {
        activeInstances--;
    }

    LifetimeTracker& operator=(const LifetimeTracker& o) noexcept = default;
    LifetimeTracker& operator=(LifetimeTracker&& o) noexcept {
        id = o.id;
        return *this;
    }

    bool operator==(const LifetimeTracker& o) const noexcept {
        return id == o.id;
    }
};

// ============================================================================
// Test Suite Error Identifiers
// ============================================================================

enum class CoreContainersTestError : uint32_t {
    MemoryLeakDetected ZHLN_ANNOTATION(ZHLN::Description<"Resource lifetime mismatch: active memory leak detected.">{}) = 1,
    ArrayInvariantFailed ZHLN_ANNOTATION(ZHLN::Description<"Array operation violated container invariants.">{}),
    HashMapInvariantFailed ZHLN_ANNOTATION(ZHLN::Description<"HashMap failed retrieval, tombstone, or resizing test.">{}),
    ObjectPoolFailed ZHLN_ANNOTATION(ZHLN::Description<"ObjectPool slot recycling or chunk growth failed.">{}),
    QueueOrderFailed ZHLN_ANNOTATION(ZHLN::Description<"Queue FIFO ordering or concurrent popping failed.">{}),
    SkipListInvariantFailed ZHLN_ANNOTATION(ZHLN::Description<"SkipList ordering or concurrent GC failed.">{}),
    RadixSortFailed ZHLN_ANNOTATION(ZHLN::Description<"RadixSort64 produced unsorted or corrupted output.">{}),
    FixedStringFailed ZHLN_ANNOTATION(ZHLN::Description<"FixedString capacity truncation or manipulation failed.">{}),
    AtomicInvariantFailed ZHLN_ANNOTATION(ZHLN::Description<"Atomic POD operations returned incorrect values.">{}),
    RangesPipelineFailed ZHLN_ANNOTATION(ZHLN::Description<"Ranges combinator returned unexpected output.">{}),
    LoopUnrollFailed ZHLN_ANNOTATION(ZHLN::Description<"Compile-time loop unrolling failed to execute exact iteration count.">{})
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct ContainersTestSuite {
    ContainersTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ContainersTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // --- 1. ZHLN::Array Lifecycle & Leak Verification ---
        std::expected<void, ZHLN::Error> array_lifecycle_and_leak_check() {
            LifetimeTracker::activeInstances = 0;

            {
                ZHLN::Array<LifetimeTracker> arr;
                for (int i = 0; i < 50; ++i) {
                    arr.emplace_back(i);
                }
                ZHLN::Test::ExpectEq(LifetimeTracker::activeInstances, 50);
                ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(50));

                // Range-based insertion
                std::vector<LifetimeTracker> extras;
                extras.emplace_back(100);
                extras.emplace_back(101);
                arr.insert(arr.begin() + 10, extras.begin(), extras.end());
                ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(52));
                ZHLN::Test::ExpectEq(arr[10].id, 100);
                ZHLN::Test::ExpectEq(arr[11].id, 101);

                // Range-based erasure
                arr.erase(arr.begin() + 10, arr.begin() + 12);
                ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(50));
                ZHLN::Test::ExpectEq(arr[10].id, 10);

                // Copy Construction
                ZHLN::Array<LifetimeTracker> copyArr = arr;
                ZHLN::Test::ExpectEq(copyArr.size(), static_cast<size_t>(50));
                ZHLN::Test::ExpectTrue(copyArr == arr);

                // Move Assignment
                ZHLN::Array<LifetimeTracker> movedArr;
                movedArr = std::move(copyArr);
                ZHLN::Test::ExpectEq(movedArr.size(), static_cast<size_t>(50));
                ZHLN::Test::ExpectTrue(copyArr.empty());
            }

            // Verify all instances were destructed with 0 leaks
            ZHLN::Test::ExpectEq(LifetimeTracker::activeInstances, 0);
            if (LifetimeTracker::activeInstances != 0) {
                return std::unexpected(CoreContainersTestError::MemoryLeakDetected);
            }
            return {};
        }

        // --- 2. ZHLN::HashMap Stress, Collision & Resizing ---
        std::expected<void, ZHLN::Error> hashmap_stress_and_resizing() {
            ZHLN::HashMap<uint32_t, uint32_t> map;
            constexpr uint32_t                kTotalItems = 1000;

            // Insert 1,000 keys causing multiple dynamic bucket resizes
            for (uint32_t i = 0; i < kTotalItems; ++i) {
                map.Insert(i, i * 10);
            }
            ZHLN::Test::ExpectEq(map.Size(), static_cast<size_t>(kTotalItems));

            // Verify lookups
            for (uint32_t i = 0; i < kTotalItems; ++i) {
                const uint32_t* val = map.Find(i);
                if (val == nullptr || *val != i * 10) {
                    return std::unexpected(CoreContainersTestError::HashMapInvariantFailed);
                }
            }

            // Erase every even key (Tombstone Stress)
            for (uint32_t i = 0; i < kTotalItems; i += 2) {
                bool erased = map.Erase(i);
                ZHLN::Test::ExpectTrue(erased);
            }
            ZHLN::Test::ExpectEq(map.Size(), static_cast<size_t>(kTotalItems / 2));

            // Verify odd keys persist and even keys are gone
            for (uint32_t i = 0; i < kTotalItems; ++i) {
                const uint32_t* val = map.Find(i);
                if (i % 2 == 0) {
                    ZHLN::Test::ExpectTrue(val == nullptr);
                } else {
                    ZHLN::Test::ExpectTrue(val != nullptr && *val == i * 10);
                }
            }

            // Test ForEach visitor count
            size_t iteratedCount = 0;
            map.ForEach([&](uint32_t k, uint32_t v) {
                if (v == k * 10) {
                    iteratedCount++;
                }
            });
            ZHLN::Test::ExpectEq(iteratedCount, static_cast<size_t>(kTotalItems / 2));

            map.Clear();
            ZHLN::Test::ExpectEq(map.Size(), static_cast<size_t>(0));
            return {};
        }

        // --- 3. ZHLN::ObjectPool Chunk Growth & Slot Recycling ---
        std::expected<void, ZHLN::Error> object_pool_allocation_and_reuse() {
            LifetimeTracker::activeInstances = 0;

            {
                // Pool with 16 objects per chunk
                ZHLN::ObjectPool<LifetimeTracker, 16> pool;
                std::vector<LifetimeTracker*>         allocated;

                // Allocate 40 objects (Spans 3 chunks)
                allocated.reserve(40);
                for (int i = 0; i < 40; ++i) {
                    allocated.push_back(pool.Create(i));
                }
                ZHLN::Test::ExpectEq(LifetimeTracker::activeInstances, 40);
                ZHLN::Test::ExpectEq(allocated[25]->id, 25);

                // Destroy half of the objects
                for (int i = 0; i < 20; ++i) {
                    pool.Destroy(allocated[i]);
                }
                ZHLN::Test::ExpectEq(LifetimeTracker::activeInstances, 20);

                // Allocate 10 objects (Must recycle from free-list without new chunk allocs)
                std::vector<LifetimeTracker*> recycled;
                recycled.reserve(10);
                for (int i = 0; i < 10; ++i) {
                    recycled.push_back(pool.Create(100 + i));
                }
                ZHLN::Test::ExpectEq(LifetimeTracker::activeInstances, 30);

                // Clean up remaining objects
                for (size_t i = 20; i < allocated.size(); ++i) {
                    pool.Destroy(allocated[i]);
                }
                for (auto* ptr: recycled) {
                    pool.Destroy(ptr);
                }
            }

            ZHLN::Test::ExpectEq(LifetimeTracker::activeInstances, 0);
            return {};
        }

        // --- 4. ZHLN::Queue FIFO Wrap-around & Concurrency ---
        std::expected<void, ZHLN::Error> queue_circular_fifo_and_concurrency() {
            ZHLN::Queue<int, 4> q;

            // Test power-of-two circular buffer wrapping
            for (int i = 0; i < 20; ++i) {
                q.push(i);
                int  popped = -1;
                bool ok     = q.try_pop(popped);
                ZHLN::Test::ExpectTrue(ok);
                ZHLN::Test::ExpectEq(popped, i);
            }
            ZHLN::Test::ExpectTrue(q.empty());

            // Multi-threaded Producer-Consumer Task
            constexpr int        kConcurrentItems = 2000;
            ZHLN::Queue<int, 64> concurrentQ;
            ZHLN::Atomic<int>    totalPoppedSum {0};
            ZHLN::Atomic<int>    popCount {0};

            // Produce in parallel
            ZHLN::TaskSystem::ParallelFor(kConcurrentItems, 128, [&](uint32_t start, uint32_t end, uint32_t) {
                for (uint32_t i = start; i < end; ++i) {
                    concurrentQ.push(1);
                }
            });

            ZHLN::Test::ExpectEq(concurrentQ.size(), static_cast<size_t>(kConcurrentItems));

            // Consume in parallel
            ZHLN::TaskSystem::ParallelFor(kConcurrentItems, 128, [&](uint32_t, uint32_t, uint32_t) {
                int outVal = 0;
                if (concurrentQ.try_pop(outVal)) {
                    totalPoppedSum.fetch_add(outVal, std::memory_order::relaxed);
                    popCount.fetch_add(1, std::memory_order::relaxed);
                }
            });

            // Drain any remainder sequentially
            int val = 0;
            while (concurrentQ.try_pop(val)) {
                totalPoppedSum.fetch_add(val, std::memory_order::relaxed);
                popCount.fetch_add(1, std::memory_order::relaxed);
            }

            ZHLN::Test::ExpectEq(popCount.load(), kConcurrentItems);
            ZHLN::Test::ExpectEq(totalPoppedSum.load(), kConcurrentItems);

            return {};
        }

        // --- 5. ZHLN::SkipList Sorting, Lookups & GC ---
        std::expected<void, ZHLN::Error> skiplist_ordering_and_gc() {
            ZHLN::SkipList<int, std::string> sl;

            // Insert keys out of order
            sl.Insert(40, "forty");
            sl.Insert(10, "ten");
            sl.Insert(30, "thirty");
            sl.Insert(20, "twenty");

            ZHLN::Test::ExpectEq(sl.Size(), static_cast<size_t>(4));

            // Verify lock-free Find
            const std::string* f20      = sl.Find(20);
            if (!ZHLN::Test::ExpectTrue(f20 != nullptr)) {
                return std::unexpected(CoreContainersTestError::SkipListInvariantFailed);
            }
            ZHLN::Test::ExpectEq(*f20, "twenty");

            // Verify strictly sorted traversal via Iterate
            std::vector<int> traversedKeys;
            sl.Iterate([&](int key, const std::string&) { traversedKeys.push_back(key); });

            ZHLN::Test::ExpectEq(traversedKeys.size(), static_cast<size_t>(4));
            if (traversedKeys.size() == 4) {
                ZHLN::Test::ExpectEq(traversedKeys[0], 10);
                ZHLN::Test::ExpectEq(traversedKeys[1], 20);
                ZHLN::Test::ExpectEq(traversedKeys[2], 30);
                ZHLN::Test::ExpectEq(traversedKeys[3], 40);
            }

            // Test Erase & QSR Garbage Collection
            bool erased = sl.Erase(20);
            ZHLN::Test::ExpectTrue(erased);
            ZHLN::Test::ExpectEq(sl.Size(), static_cast<size_t>(3));
            ZHLN::Test::ExpectTrue(sl.Find(20) == nullptr);

            return {};
        }

        // --- 6. ZHLN::RadixSort64 Key Ordering & Stability ---
        std::expected<void, ZHLN::Error> radix_sort_64bit() {
            constexpr uint32_t          kSortCount = 2048;
            std::vector<ZHLN::SortItem> items(kSortCount);
            std::vector<ZHLN::SortItem> temp(kSortCount);

            // Populate pseudo-random 64-bit keys
            uint64_t seed = 0x123456789ABCDEF0ULL;
            for (uint32_t i = 0; i < kSortCount; ++i) {
                seed     = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                items[i] = {.key = {seed}, .payload = i};
            }

            ZHLN::RadixSort64(items.data(), temp.data(), kSortCount);

            // Verify monotonic ascending order
            for (uint32_t i = 1; i < kSortCount; ++i) {
                if (items[i - 1].key.value > items[i].key.value) {
                    return std::unexpected(CoreContainersTestError::RadixSortFailed);
                }
            }

            return {};
        }

        // --- 7. ZHLN::FixedString Bounds, Truncation & Comparators ---
        std::expected<void, ZHLN::Error> fixed_string_manipulation() {
            ZHLN::String32 str = "Zahlen";
            ZHLN::Test::ExpectEq(str.size(), static_cast<size_t>(6));
            ZHLN::Test::ExpectEq(std::string_view(str), "Zahlen");

            str.append(" Engine");
            ZHLN::Test::ExpectEq(str.size(), static_cast<size_t>(13));
            ZHLN::Test::ExpectEq(std::string_view(str), "Zahlen Engine");

            // Verify truncation boundary (32 bytes max capacity = 31 chars + '\0')
            ZHLN::String32 longStr = "1234567890123456789012345678901234567890";
            ZHLN::Test::ExpectEq(longStr.size(), static_cast<size_t>(31));
            ZHLN::Test::ExpectEq(longStr[31], '\0');

            // Comparisons
            ZHLN::String32 a = "Alpha";
            ZHLN::String32 b = "Beta";
            ZHLN::Test::ExpectTrue(a < b);
            ZHLN::Test::ExpectTrue(a != b);

            char destBuf[16];
            a.copy_to(destBuf);
            ZHLN::Test::ExpectEq(std::string_view(destBuf), "Alpha");

            return {};
        }

        // --- 8. ZHLN::Atomic POD Correctness ---
        std::expected<void, ZHLN::Error> atomic_pod_operations() {
            ZHLN::Atomic<uint32_t> atom {};
            atom.store(100);

            ZHLN::Test::ExpectEq(atom.load(), 100u);
            ZHLN::Test::ExpectEq(atom.fetch_add(50), 100u);
            ZHLN::Test::ExpectEq(atom.load(), 150u);
            ZHLN::Test::ExpectEq(atom.fetch_sub(25), 150u);
            ZHLN::Test::ExpectEq(atom.load(), 125u);

            // Bitwise operations
            atom.store(0b1010);
            ZHLN::Test::ExpectEq(atom.fetch_or(0b0101), 0b1010u);
            ZHLN::Test::ExpectEq(atom.load(), 0b1111u);

            ZHLN::Test::ExpectEq(atom.fetch_and(0b0011), 0b1111u);
            ZHLN::Test::ExpectEq(atom.load(), 0b0011u);

            // CAS loop
            uint32_t expected = 0b0011;
            bool     success  = atom.compare_exchange_strong(expected, 0b1000);
            ZHLN::Test::ExpectTrue(success);
            ZHLN::Test::ExpectEq(atom.load(), 0b1000u);

            return {};
        }

        // --- 9. ZHLN::Ranges Extended Combinators ---
        std::expected<void, ZHLN::Error> ranges_extended_combinators() {
            using namespace ZHLN::Ranges;

            ZHLN::Array<int> numbers = {10, 20, 30, 40, 50, 60};

            // 1. Stride
            std::vector<int> strided;
            for (int val: numbers | Stride(2)) {
                strided.push_back(val);
            }
            // 10, 30, 50
            ZHLN::Test::ExpectEq(strided.size(), static_cast<size_t>(3));
            if (strided.size() == 3) {
                ZHLN::Test::ExpectEq(strided[0], 10);
                ZHLN::Test::ExpectEq(strided[1], 30);
                ZHLN::Test::ExpectEq(strided[2], 50);
            }

            // 2. Drop
            std::vector<int> dropped;
            for (int val: numbers | Drop(4)) {
                dropped.push_back(val);
            }
            // 50, 60
            ZHLN::Test::ExpectEq(dropped.size(), static_cast<size_t>(2));
            if (dropped.size() == 2) {
                ZHLN::Test::ExpectEq(dropped[0], 50);
                ZHLN::Test::ExpectEq(dropped[1], 60);
            }

            // 3. Zip
            ZHLN::Array<std::string> names    = {"A", "B", "C"};
            ZHLN::Array<int>         scores   = {100, 200, 300};
            size_t                   zipCount = 0;
            for (auto [name, score]: Zip(names, scores)) {
                if (name == "B") {
                    ZHLN::Test::ExpectEq(score, 200);
                }
                zipCount++;
            }
            ZHLN::Test::ExpectEq(zipCount, static_cast<size_t>(3));

            return {};
        }

        // --- 10. ZHLN::Loop Compile-Time Unrolling ---
        std::expected<void, ZHLN::Error> loop_unrolling() {
            int sum = 0;
            ZHLN::Unroll<4>([&](auto ic) { sum += static_cast<int>(decltype(ic)::value); });
            // 0 + 1 + 2 + 3 = 6
            ZHLN::Test::ExpectEq(sum, 6);

            int repeatCount = 0;
            ZHLN::Repeat<5>([&]() { repeatCount++; });
            ZHLN::Test::ExpectEq(repeatCount, 5);

            int unrollLoopSum = 0;
            ZHLN::UnrollLoop<4>(10, [&](size_t idx) { unrollLoopSum += static_cast<int>(idx); });
            // 0 + 1 + 2 + ... + 9 = 45
            ZHLN::Test::ExpectEq(unrollLoopSum, 45);

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunContainersSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ContainersTestSuite>();
}

