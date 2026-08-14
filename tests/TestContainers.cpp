// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <expected>
#include <string>

enum class ContainerTestError : uint32_t {
    Success = 0,
    ArrayMismatch[[= ZHLN::Reflect::Description("Array elements or size mismatch after mutation.")]],
    HashMapMismatch[[= ZHLN::Reflect::Description("HashMap lookup or tombstone erasure failed.")]],
    RangesPipelineFailed[[= ZHLN::Reflect::Description("Ranges pipeline operator produced incorrect output.")]]
};

struct ContainersTestSuite {
    struct Tests {
        // --- 1. ZHLN::Array Operations ---
        std::expected<void, ZHLN::Error> array_push_pop_growth() {
            ZHLN::Array<int> arr;
            ZHLN::Test::ExpectTrue(arr.empty());

            for (int i = 0; i < 100; ++i) {
                arr.push_back(i);
            }
            ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(100));
            ZHLN::Test::ExpectEq(arr[0], 0);
            ZHLN::Test::ExpectEq(arr[99], 99);

            arr.pop_back();
            ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(99));
            ZHLN::Test::ExpectEq(arr.back(), 98);

            arr.clear();
            ZHLN::Test::ExpectTrue(arr.empty());
            return {};
        }

        std::expected<void, ZHLN::Error> array_emplace_and_erase() {
            ZHLN::Array<std::string> arr;
            arr.emplace_back("Alpha");
            arr.emplace_back("Beta");
            arr.emplace_back("Gamma");

            ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(3));
            ZHLN::Test::ExpectEq(arr[1], "Beta");

            // Erase middle element
            arr.erase(arr.begin() + 1);
            ZHLN::Test::ExpectEq(arr.size(), static_cast<size_t>(2));
            ZHLN::Test::ExpectEq(arr[0], "Alpha");
            ZHLN::Test::ExpectEq(arr[1], "Gamma");
            return {};
        }

        // --- 2. ZHLN::HashMap Operations & Tombstone Recycling ---
        std::expected<void, ZHLN::Error> hashmap_insert_find_erase() {
            ZHLN::HashMap<std::string, int> map;

            map.Insert("Player1", 100);
            map.Insert("Player2", 200);
            map.Insert("Player3", 300);

            ZHLN::Test::ExpectEq(map.Size(), static_cast<size_t>(3));

            const int* p1      = map.Find("Player1");
            auto       checkP1 = ZHLN::Test::AssertTrue(p1 != nullptr);
            if (!checkP1) {
                return checkP1;
            }
            ZHLN::Test::ExpectEq(*p1, 100);

            // Test O(1) Tombstone Erasure
            bool erased = map.Erase("Player2");
            ZHLN::Test::ExpectTrue(erased);
            ZHLN::Test::ExpectEq(map.Size(), static_cast<size_t>(2));
            ZHLN::Test::ExpectTrue(map.Find("Player2") == nullptr);

            // Insert again to test slot/tombstone recycling
            map.Insert("Player2_New", 250);
            ZHLN::Test::ExpectEq(map.Size(), static_cast<size_t>(3));

            const int* p2New   = map.Find("Player2_New");
            auto       checkP2 = ZHLN::Test::AssertTrue(p2New != nullptr);
            if (!checkP2) {
                return checkP2;
            }
            ZHLN::Test::ExpectEq(*p2New, 250);

            return {};
        }

        // --- 3. ZHLN::Ranges Pipe Syntax ---
        std::expected<void, ZHLN::Error> ranges_pipeline() {
            using namespace ZHLN::Ranges;

            ZHLN::Array<int> numbers;
            for (int i = 1; i <= 10; ++i) {
                numbers.push_back(i);
            }

            // Filter even numbers -> Multiply by 10 -> Take first 3
            int sum   = 0;
            int count = 0;

            auto pipeline = numbers | Filter([](int n) { return n % 2 == 0; }) | Transform([](int n) { return n * 10; }) | Take(3);

            for (int val: pipeline) {
                sum += val;
                count++;
            }

            // Expected elements: 20, 40, 60 -> sum = 120, count = 3
            ZHLN::Test::ExpectEq(count, 3);
            ZHLN::Test::ExpectEq(sum, 120);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ContainersTestSuite>();
}
