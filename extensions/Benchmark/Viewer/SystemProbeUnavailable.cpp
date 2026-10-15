// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/SystemProbeUnavailable.cpp -- INTERNAL backend.
//
// The probe for every platform without a counter backend of its own: Windows,
// macOS, the BSDs. It exists so that the panel, the CSV and the log all have an
// answer everywhere -- "this platform has no backend", said once -- rather than
// a build that only compiles on Linux.
//
// Adding a platform is a file beside this one, not a change anywhere above it:
// define ZHLN::Bench::Platform::MakeSystemProbe() under the right preprocessor
// guard and implement Sample() from the platform's own counters (Windows would
// read PDH or NtQuerySystemInformation for CPU time and the adapter's
// performance counters for GPU; macOS, host_statistics and IOKit). Nothing in
// SystemProbe.hpp, SystemTelemetry.hpp or the panel changes, which is the whole
// point of the seam.
#if !defined(__linux__)

#include "SystemProbe.hpp"

namespace ZHLN::Bench::Platform {

namespace {

inline constexpr std::string_view kReason =
    "no hardware counter backend is compiled in for this platform (Linux procfs and sysfs are the only ones today)";

class UnavailableProbe final: public SystemProbe {
  public:
    // Nothing to read, and the reason is Unavailable()'s: an empty sample would
    // claim the machine answered, and it did not.
    [[nodiscard]] auto Sample() noexcept -> std::optional<ProbeSample> override {
        return std::nullopt;
    }

    [[nodiscard]] auto BackendName() const noexcept -> std::string_view override {
        return {};
    }

    [[nodiscard]] auto Unavailable() const noexcept -> std::string_view override {
        return kReason;
    }

    // The reason above is about the whole probe, so there is nothing left to say
    // about one half of it.
    [[nodiscard]] auto GpuUnavailable() const noexcept -> std::string_view override {
        return {};
    }

    [[nodiscard]] auto GpuName() const noexcept -> std::string_view override {
        return {};
    }
};

} // namespace

auto MakeSystemProbe(std::string_view gpuNameHint) -> std::unique_ptr<SystemProbe> {
    static_cast<void>(gpuNameHint); // a platform with no counters has nothing to match a device to
    return std::make_unique<UnavailableProbe>();
}

} // namespace ZHLN::Bench::Platform

#endif // !__linux__
