// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/SystemProbe.hpp -- INTERNAL, not a public header.
//
// The seam between the machine-independent half of SystemTelemetry and the
// platform backends. It is deliberately as free of the platform as the public
// header is: raw readings, standard-library types, and one factory. The backend
// translates its own world (jiffies, sysfs write-ups, NVML handles) into this,
// and nothing above it learns how the numbers were obtained.
//
// Every function here returns its answer. There are no out parameters and no
// boolean results: a reading the platform did not give is an empty optional in
// one of these structs, a sample the backend could not take at all is an empty
// optional around the sample, and the one place with several ways to fail -- the
// NVML binding -- returns std::expected (see SystemProbeLinux.cpp). Presence is
// in the type, so a caller cannot read a value it forgot to check.
//
// A backend file is compiled into the viewer target and scoped to its platform
// by a preprocessor guard, so exactly one definition of MakeSystemProbe() lands
// in the library on any given platform.
//
// The namespace is named for what it holds -- the platform seam -- rather than
// for how private it is: the factory and the readings it speaks in have to be
// visible to SystemTelemetry.cpp and to whichever backend is compiled, so they
// cannot live in either translation unit's anonymous namespace. Everything a
// backend keeps to itself does live there; this block is only what crosses.
#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace ZHLN::Bench::Platform {

// One logical CPU as the backend read it, under the platform's own core number:
// the position in the sample's list *is* that number, so it is not stored twice.
// The optionals are per reading, not per backend -- a core the kernel stopped
// reporting for one read (hotplug, or a CPU taken offline) carries no usage for
// that sample, and a machine with no cpufreq carries no clocks at all. The
// caller leaves an absent reading out of that series' window rather than
// recording a dip to zero the machine never had.
struct ProbeCore {
    std::optional<float> usagePct;
    std::optional<float> clockMHz;
};

// One hardware sample: whatever the machine answered with. A series the platform
// does not expose is an empty optional, so a sample whose every series is empty
// is a sample that says "nothing to report" -- which is not the same as a
// backend that could not read at all: that returns nothing from Sample().
struct ProbeSample {
    std::optional<float>   cpuUsagePct; // machine-wide, all cores together
    std::optional<float>   cpuClockMHz; // mean across the cores of this sample
    std::optional<float>   gpuUsagePct;
    std::optional<float>   gpuClockMHz;
    std::vector<ProbeCore> cores; // one entry per CPU the backend knows about
};

// A source of hardware counters. One translation unit per platform implements
// it; nothing else in this target names a syscall, a /proc path or a vendor
// header.
class SystemProbe {
  public:
    SystemProbe()                                = default;
    SystemProbe(const SystemProbe&)              = delete;
    auto operator=(const SystemProbe&) -> SystemProbe& = delete;
    virtual ~SystemProbe()                       = default;

    // One reading, or nothing when the backend cannot read at all -- the source
    // it needs went away. Called at the telemetry's own pace, so an
    // implementation reads counters and does not wait on anything.
    [[nodiscard]] virtual auto Sample() noexcept -> std::optional<ProbeSample> = 0;

    // What is answering: "procfs + NVML", "procfs + DRM sysfs", and so on. Empty
    // when no backend is compiled in for this platform.
    [[nodiscard]] virtual auto BackendName() const noexcept -> std::string_view = 0;

    // Why the machine-wide series are empty, when they are: /proc/stat
    // unreadable, or no backend for this platform at all. Empty while they
    // answer.
    [[nodiscard]] virtual auto Unavailable() const noexcept -> std::string_view = 0;

    // Why the GPU series are empty while the machine's own counters are fine --
    // no NVML device and no sysfs card, say. Empty while they answer, and empty
    // for a backend that has already said in Unavailable() why there is nothing
    // to read at all.
    [[nodiscard]] virtual auto GpuUnavailable() const noexcept -> std::string_view = 0;

    // The device the counters are for, as the driver names it. Empty before the
    // backend has one to name, which is every backend that only has sysfs cards.
    [[nodiscard]] virtual auto GpuName() const noexcept -> std::string_view = 0;
};

// The backend for this platform. Exactly one of SystemProbeLinux.cpp and
// SystemProbeUnavailable.cpp defines it, each under its own preprocessor guard.
//
// It never returns nothing: a machine with no counters to read gets the backend
// that says so, whose BackendName() is empty and whose Unavailable() is the
// reason. That is what keeps a caller from having to tell "no probe" apart from
// "a probe with nothing to say".
[[nodiscard]] auto MakeSystemProbe(std::string_view gpuNameHint) -> std::unique_ptr<SystemProbe>;

} // namespace ZHLN::Bench::Platform
