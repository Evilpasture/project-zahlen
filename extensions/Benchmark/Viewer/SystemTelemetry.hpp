// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/SystemTelemetry.hpp
//
// What the machine was doing while the frames were being drawn: CPU usage and
// clocks per logical core, machine-wide CPU usage and clock, and the GPU's
// utilization and core clock -- sampled from whatever the platform exposes.
//
// This header names no platform. Nothing here includes a system header, a
// vendor header (<nvml.h>) or a path under /proc or /sys -- the counters come
// from a probe behind a pimpl, and the only translation units that touch a
// syscall are the platform backends (SystemProbeLinux.cpp and the stub beside
// it, both compiled into this target and each scoped to its own platform by a
// preprocessor guard). A consumer of this header links no platform anything,
// and a second backend -- Windows PDH, macOS host_statistics -- is one more
// file defining MakeSystemProbe(), with nothing here to change.
//
// Nothing here is a flag beside a value either. A series the platform never
// answered with is an empty optional, so a caller cannot read a number it forgot
// to check, and "no reading" cannot be confused with a reading of zero -- which
// for a clock is a real number (a parked core) and for usage is not.
//
// Reading the numbers:
//
//   * Percentiles are values, not the "mean of the slowest X%" the frame-time
//     lows use. A hardware reading is a level, not an interval, so the 99th
//     percentile of CPU usage is the level the machine held in its busiest 1%
//     of samples, and the 1st percentile of a clock is the level it fell to in
//     its lowest 1% -- which is where a thermal or power throttle shows up.
//
//   * Utilization arrives from the platform as a delta between two counter
//     reads (Linux accounts CPU time in jiffies, so there is no instantaneous
//     number to ask for). The probe holds the previous counters, which is why
//     the first sample after Initialize reports clocks but no usage.
//
//   * Sampling is paced: one hardware read per kSampleInterval however fast the
//     host's frames are. A counter read is a file read or a driver ioctl, and
//     the point of the panel is to cost the host nothing it can notice.
#pragma once

#include <Zahlen/Common.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Bench {

// One series' summary over the sampling window. Every field is in the series'
// own unit: percent for usage, MHz for clocks. The struct itself carries no
// "valid" flag -- presence is the optional around it, so a summary exists only
// once the window has something in it.
struct HardwareSeries {
    float    last    = 0.0f; // the most recent sample
    float    avg     = 0.0f; // mean over the window
    float    min     = 0.0f;
    float    max     = 0.0f;
    float    p1      = 0.0f; // 1st percentile -- the low tail
    float    p99     = 0.0f; // 99th percentile -- the high tail
    uint64_t samples = 0;    // samples in the window
};

// One logical CPU: the platform's own index, and its two series. A core whose
// clock the platform does not expose carries no clock series at all, which is
// how the panel knows to leave it out rather than draw a zero.
struct CoreTelemetry {
    unsigned                      index = 0;
    std::optional<HardwareSeries> usagePct;
    std::optional<HardwareSeries> clockMHz;
};

// Everything the panel and the CSV read, refreshed by each sample. An absent
// series means the platform does not expose that counter; gpuUnavailableReason
// says why, on a machine whose machine-wide counters are fine.
struct SystemSnapshot {
    uint64_t samples       = 0; // samples taken since construction or Reset
    double   windowSeconds = 0.0;
    unsigned coreCount     = 0;

    std::optional<HardwareSeries> cpuUsagePct; // machine-wide, all cores together
    std::optional<HardwareSeries> cpuClockMHz; // mean across the cores of each sample
    std::optional<HardwareSeries> gpuUsagePct;
    std::optional<HardwareSeries> gpuClockMHz;

    std::string gpuName;             // the renderer's device name, or the backend's label
    std::string unavailableReason;   // why there are no machine counters at all, empty while they answer
    std::string gpuUnavailableReason; // why the GPU counters are missing, empty while they answer
};

class ZHLN_STATIC_API SystemTelemetry {
  public:
    // Samples the machine-wide and GPU series keep: 1024 at four per second is
    // a little over four minutes of history, which is long enough for the tail
    // percentiles to mean something and short enough to react to a change.
    static constexpr size_t kWindowSamples = 1024;

    // Per-core series are shorter: 256 samples is a minute of history per core,
    // and a machine with 256 threads then costs 256 KiB rather than a megabyte.
    static constexpr size_t kCoreWindowSamples = 256;

    // How often the platform is asked. Four times a second is fast enough to
    // see a spike and slow enough that a counter read never lands in a frame's
    // profile.
    static constexpr std::chrono::milliseconds kSampleInterval {250};

    // Reads ZHLN_SYSTEM_TELEMETRY. Set it to "0" to leave the counters alone
    // entirely -- for a run whose numbers must not depend on a probe.
    SystemTelemetry();
    ~SystemTelemetry();

    SystemTelemetry(const SystemTelemetry&)                    = delete;
    auto operator=(const SystemTelemetry&) -> SystemTelemetry& = delete;

    // Samples the platform, at most once per kSampleInterval. Called once per
    // frame by whatever owns this; cheap when the interval has not elapsed.
    void Tick();

    // Drops the window and starts a new one. Clocks reappear on the next
    // sample; usage needs two reads, exactly as at startup.
    void Reset();

    // The device the renderer is drawing on, as Vulkan names it
    // (RenderInfo::gpuName). The Linux backend matches its counters to it --
    // NVML by name, DRM by vendor -- so a machine with two GPUs reports the one
    // in use rather than the first one. Must be set before the first
    // meaningful Tick(); later calls are ignored once a probe is matched.
    void SetGpuNameHint(std::string_view vulkanDeviceName);

    [[nodiscard]] auto Snapshot() const noexcept -> const SystemSnapshot&;

    // Every logical core, ascending by the platform's index. Empty before the
    // first sample, or when the platform has no CPU counters.
    [[nodiscard]] auto Cores() const noexcept -> std::span<const CoreTelemetry>;

    // The newest up to `count` samples of core `index`, oldest first -- what a
    // panel draws a core's own history from. A view into the window, so it is
    // valid until the next Tick(); empty for an index this machine does not have.
    [[nodiscard]] auto CoreUsageHistory(unsigned index, size_t count) const noexcept -> std::span<const float>;

    // The up to `count` cores with the highest average usage in the window,
    // busiest first -- what a fixed-height panel can show, most useful end first.
    // Only cores that answered with usage are listed. A view, so it is valid
    // until the next Tick(); the ordering is built when the window is sampled,
    // not per panel frame.
    [[nodiscard]] auto BusiestCores(size_t count) const noexcept -> std::span<const CoreTelemetry>;

    // What is answering: "procfs + NVML", "procfs + DRM sysfs", and so on. Empty
    // when no backend is compiled in for this platform.
    [[nodiscard]] auto BackendName() const noexcept -> std::string_view;

    // False when the probe was disabled by the environment, or when this
    // platform has no backend at all.
    [[nodiscard]] auto IsEnabled() const noexcept -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN::Bench
