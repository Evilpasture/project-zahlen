// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <utility>

namespace ZHLN {

struct CullingStats {
    static inline uint32_t TotalObjects      = 0;
    static inline uint32_t CulledObjects     = 0;
    static inline bool     EnableCulling     = true;
    static inline bool     FreezeFrustum     = false;
    static inline uint32_t TotalTriangles    = 0;
    static inline uint32_t RenderedTriangles = 0;
};

class ZHLN_API CPUProfiler {
  public:
    static void Record(std::string_view name, float timeMS) noexcept;

    using MetricCallback = void (*)(const char* name, float cpuTimeMS, float rollingAverageMS, const float* history, size_t historyCount, void* userData);
    static void IterateMetrics(MetricCallback callback, void* userData) noexcept;
};

struct ZHLN_API ScopedTimer {
    const char* name;
    uint64_t    start;

    /**
     * @brief RAII Timer.
     * If no name is provided, uses std::source_location to capture the enclosing function name.
     */
    explicit ScopedTimer(const char* n = nullptr, std::source_location loc = std::source_location::current()) noexcept;

    ~ScopedTimer() noexcept;
};

/**
 * @brief Higher-order functional scope profiler.
 *
 * Usage:
 *   ZHLN::ProfileScope("ALife Update", [&] {
 *       alife.Update(dt);
 *   });
 */
template <typename Func>
decltype(auto) ProfileScope(const char* name, Func&& func, std::source_location loc = std::source_location::current()) {
    ScopedTimer timer(name, loc);
    return std::forward<Func>(func)();
}

} // namespace ZHLN
