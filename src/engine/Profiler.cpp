// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/Core/SkipList.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace ZHLN {

namespace {

template <size_t N = 100>
struct ProfileDataInternal {
    std::array<std::atomic<float>, N> history {};
    std::atomic<size_t>               writeIndex {0};
    std::atomic<float>                latestValue {0.0f};
    std::atomic<float>                rollingAverageMS {0.0f};

    void Push(float value) noexcept {
        size_t idx = writeIndex.fetch_add(1, std::memory_order::relaxed) % N;
        history[idx].store(value, std::memory_order::relaxed);
        latestValue.store(value, std::memory_order::relaxed);

        float avg     = rollingAverageMS.load(std::memory_order::relaxed);
        float nextAvg = avg * 0.95f + value * 0.05f;
        rollingAverageMS.store(nextAvg, std::memory_order::relaxed);
    }
};

SkipList<std::string, std::shared_ptr<ProfileDataInternal<100>>, std::less<>> s_Metrics;
ZHLN::Mutex                                                                   s_MetricsInsertMutex {};

}

void CPUProfiler::Record(std::string_view name, float timeMS) noexcept {
    std::string key(name);
    const auto  push = [&](const std::shared_ptr<ProfileDataInternal<100>>& data) { data->Push(timeMS); };
    if (s_Metrics.With(key, push)) [[likely]] {
        return;
    }

    ZHLN::MutexGuard lock(s_MetricsInsertMutex);
    if (!s_Metrics.With(key, push)) {
        auto created = std::make_shared<ProfileDataInternal<100>>();
        created->Push(timeMS);
        s_Metrics.Insert(key, std::move(created));
    }
}

void CPUProfiler::IterateMetrics(MetricCallback callback, void* userData) noexcept {
    if (callback == nullptr) {
        return;
    }

    s_Metrics.Iterate([&](const auto& name, const std::shared_ptr<ProfileDataInternal<100>>& data) {
        float cpuTime    = data->latestValue.load(std::memory_order::relaxed);
        float rollingAvg = data->rollingAverageMS.load(std::memory_order::relaxed);

        size_t count = data->writeIndex.load(std::memory_order::relaxed);
        size_t limit = std::min(count, size_t(100));

        std::array<float, 100> flatHistory {};
        if (count < 100) {
            for (size_t j = 0; j < count; ++j) {
                flatHistory[j] = data->history[j].load(std::memory_order::relaxed);
            }
        } else {
            size_t startIdx = count % 100;
            for (size_t j = 0; j < 100; ++j) {
                flatHistory[j] = data->history[(startIdx + j) % 100].load(std::memory_order::relaxed);
            }
        }

        callback(name.c_str(), cpuTime, rollingAvg, flatHistory.data(), limit, static_cast<uint64_t>(count), userData);
    });
}

ScopedTimer::ScopedTimer(const char* n, std::source_location loc) noexcept: name((n != nullptr && n[0] != '\0') ? n : loc.function_name()) {
    auto now = std::chrono::high_resolution_clock::now();
    start    = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

ScopedTimer::~ScopedTimer() noexcept {
    auto     now      = std::chrono::high_resolution_clock::now();
    uint64_t end      = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    float    duration = static_cast<float>(end - start) / 1000.0f;
    CPUProfiler::Record(name, duration);
}

}
