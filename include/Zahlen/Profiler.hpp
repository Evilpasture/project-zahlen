// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN {

struct CullingStats {
    static inline uint32_t TotalObjects  = 0;
    static inline uint32_t CulledObjects = 0;
    static inline bool     EnableCulling = true; // Toggle for testing!
    static inline bool     FreezeFrustum = false;
};

class CPUProfiler {
  public:
    static void Record(std::string_view name, float timeMS) noexcept;

    // O(1) iteration interface for the UI loop
    using MetricCallback = void (*)(const char* name, float cpuTimeMS, float rollingAverageMS, const float* history, size_t historyCount, void* userData);
    static void IterateMetrics(MetricCallback callback, void* userData) noexcept;
};

// RAII Timer - Stripped of <chrono> and <string> templates
struct ScopedTimer {
    const char* name;
    uint64_t    start;

    ScopedTimer(const char* n) noexcept;
    ~ScopedTimer() noexcept;
};

} // namespace ZHLN

#define ZHLN_PROFILE_SCOPE(name) ZHLN::ScopedTimer _prof_timer(name)
