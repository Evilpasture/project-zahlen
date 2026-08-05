// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/SkipList.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN {

namespace {

template <size_t N = 100>
struct ProfileDataInternal {
    std::array<ZHLN::Atomic<float>, N> history;
    ZHLN::Atomic<size_t>               writeIndex;
    ZHLN::Atomic<float>                latestValue;
    ZHLN::Atomic<float>                rollingAverageMS;

    void Push(float value) noexcept {
        size_t idx = writeIndex.fetch_add(1, std::memory_order::relaxed) % N;
        history[idx].store(value, std::memory_order::relaxed);
        latestValue.store(value, std::memory_order::relaxed);

        float avg     = rollingAverageMS.load(std::memory_order::relaxed);
        float nextAvg = avg * 0.95f + value * 0.05f;
        rollingAverageMS.store(nextAvg, std::memory_order::relaxed);
    }
};

static_assert(
    std::is_trivially_copyable_v<ProfileDataInternal<100>> && std::is_trivially_default_constructible_v<ProfileDataInternal<100>>,
    "ProfileDataInternal must remain a POD for SkipList compatibility"
);

static SkipList<std::string, ProfileDataInternal<100>, std::less<>> s_Metrics;

} // namespace

void CPUProfiler::Record(std::string_view name, float timeMS) noexcept {
    std::string key(name);
    const auto* dataPtr = s_Metrics.Find(key);

    if (dataPtr == nullptr) [[unlikely]] {
        s_Metrics.Insert(key, ProfileDataInternal<100> {});
        dataPtr = s_Metrics.Find(key);
    }

    auto* data = const_cast<ProfileDataInternal<100>*>(dataPtr);
    data->Push(timeMS);
}

void CPUProfiler::IterateMetrics(MetricCallback callback, void* userData) noexcept {
    if (callback == nullptr) {
        return;
    }

    s_Metrics.Iterate([&](const std::string& name, const ProfileDataInternal<100>& data) {
        float cpuTime    = data.latestValue.load(std::memory_order::relaxed);
        float rollingAvg = data.rollingAverageMS.load(std::memory_order::relaxed);

        size_t count = data.writeIndex.load(std::memory_order::relaxed);
        size_t limit = std::min(count, size_t(100));

        std::array<float, 100> flatHistory {};
        if (count < 100) {
            for (size_t j = 0; j < count; ++j) {
                flatHistory[j] = data.history[j].load(std::memory_order::relaxed);
            }
        } else {
            size_t startIdx = count % 100;
            for (size_t j = 0; j < 100; ++j) {
                flatHistory[j] = data.history[(startIdx + j) % 100].load(std::memory_order::relaxed);
            }
        }

        callback(name.c_str(), cpuTime, rollingAvg, flatHistory.data(), limit, userData);
    });
}

ScopedTimer::ScopedTimer(const char* n) noexcept: name(n) {
    auto now = std::chrono::high_resolution_clock::now();
    start    = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

ScopedTimer::~ScopedTimer() noexcept {
    auto     now      = std::chrono::high_resolution_clock::now();
    uint64_t end      = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    float    duration = static_cast<float>(end - start) / 1000.0f;
    CPUProfiler::Record(name, duration);
}

} // namespace ZHLN
