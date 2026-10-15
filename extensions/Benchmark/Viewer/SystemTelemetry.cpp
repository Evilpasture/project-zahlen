// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/SystemTelemetry.cpp
#include <Benchmark/Viewer/SystemTelemetry.hpp>
#include <Benchmark/Viewer/SystemProbe.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <utility>

namespace ZHLN::Bench {

namespace {

// A core's average usage, and zero for one the window has nothing for: the order
// the busiest list is built in.
[[nodiscard]] auto AverageUsage(const CoreTelemetry& core) noexcept -> float {
    return core.usagePct ? core.usagePct->avg : 0.0f;
}

// ZHLN_SYSTEM_TELEMETRY=0 leaves every counter unread: for a run whose numbers
// must not depend on a probe, or for a machine whose /proc reads are themselves
// under measurement.
[[nodiscard]] auto ProbeDisabledByEnvironment() noexcept -> bool {
    const char* setting = std::getenv("ZHLN_SYSTEM_TELEMETRY");
    return setting != nullptr && std::string_view(setting) == "0";
}

// A window of samples with the summary the panel reads: mean, ends, the two tail
// percentiles, and the wall time the samples actually cover -- each sample
// arrives with its own interval, and the one that falls out of the window takes
// its interval with it, so the window is the real span rather than a nominal
// count times a nominal period.
//
// The samples are kept in order, oldest first, and a full window makes room by
// shifting down one slot. That costs a memmove of a few kilobytes per sample --
// four times a second, against a counter read -- and it buys the thing the panel
// draws from: the newest samples are one contiguous span, so a core's history is
// a view rather than a copy into a caller's buffer.
//
// The percentile pass copies and partitions, so it runs on a stride rather than
// on every push, exactly as the frame-time window does.
class SeriesRing {
  public:
    // Pushes per summary. A machine-wide series is pushed four times a second,
    // so this is a summary every four seconds, which is the scale a four-minute
    // window moves at.
    static constexpr size_t kSummaryStride = 16;

    explicit SeriesRing(size_t capacity = 0) {
        Resize(capacity);
    }

    void Resize(size_t capacity) {
        _capacity = capacity;
        _samples.clear();
        _spans.clear();
        _samples.reserve(capacity);
        _spans.reserve(capacity);
        _scratch.assign(capacity, 0.0f);
        Reset();
    }

    void Reset() {
        _samples.clear();
        _spans.clear();
        _sinceSummary  = 0;
        _windowSeconds = 0.0;
        _summary       = HardwareSeries {};
    }

    void Push(float sample, float spanSeconds) {
        if (_capacity == 0) {
            return;
        }

        if (_samples.size() == _capacity) {
            _windowSeconds -= static_cast<double>(_spans.front());
            std::shift_left(_samples.begin(), _samples.end(), 1);
            std::shift_left(_spans.begin(), _spans.end(), 1);
            _samples.back() = sample;
            _spans.back()   = spanSeconds;
        } else {
            _samples.push_back(sample);
            _spans.push_back(spanSeconds);
        }
        _windowSeconds += static_cast<double>(spanSeconds);
        _summary.last = sample;

        // A summary while the window fills, then one per stride.
        if (_samples.size() < kSummaryStride || ++_sinceSummary >= kSummaryStride) {
            Summarize();
            _sinceSummary = 0;
        }
    }

    // The summary so far, or nothing before the window has a sample in it. The
    // flag the old shape carried beside the numbers is what this optional says.
    [[nodiscard]] auto Summary() const noexcept -> std::optional<HardwareSeries> {
        if (_samples.empty()) {
            return std::nullopt;
        }
        return _summary;
    }

    [[nodiscard]] auto WindowSeconds() const noexcept -> double {
        return _windowSeconds;
    }

    // The newest up to `count` samples, oldest first. A view into the window: it
    // is valid until the next Push, which is the frame it is drawn in.
    [[nodiscard]] auto Newest(size_t count) const noexcept -> std::span<const float> {
        const size_t shown = std::min(count, _samples.size());
        return std::span<const float> {_samples.data() + (_samples.size() - shown), shown};
    }

  private:
    void Summarize() {
        if (_samples.empty()) {
            return;
        }

        const auto [low, high] = std::ranges::minmax(_samples);
        const double sum       = std::accumulate(_samples.begin(), _samples.end(), 0.0);

        _summary.avg     = static_cast<float>(sum / static_cast<double>(_samples.size()));
        _summary.min     = low;
        _summary.max     = high;
        _summary.p1      = Percentile(0.01f);
        _summary.p99     = Percentile(0.99f);
        _summary.samples = _samples.size();
    }

    // The value at `fraction` of the window, ascending: p1 is the low tail, p99
    // the high one. A value, not the mean of the tail -- see the header.
    [[nodiscard]] auto Percentile(float fraction) -> float {
        const size_t count = _samples.size();
        if (count == 0) {
            return 0.0f;
        }

        std::ranges::copy(_samples, _scratch.begin());

        const auto   begin  = _scratch.begin();
        const auto   end    = begin + static_cast<std::ptrdiff_t>(count);
        const double scaled = std::clamp(static_cast<double>(fraction) * static_cast<double>(count - 1), 0.0, static_cast<double>(count - 1));
        const auto   at     = begin + static_cast<std::ptrdiff_t>(static_cast<size_t>(scaled));

        std::nth_element(begin, at, end);
        return *at;
    }

    std::vector<float> _samples;
    std::vector<float> _spans;   // the interval each held sample covers
    std::vector<float> _scratch; // percentile scratch
    size_t             _capacity      = 0;
    size_t             _sinceSummary  = 0;
    double             _windowSeconds = 0.0;
    HardwareSeries     _summary {};
};

} // namespace

struct SystemTelemetry::Impl {
    std::unique_ptr<Platform::SystemProbe> probe;

    // When the last sample was taken; the interval since it is what the accepted
    // samples are stamped with.
    std::chrono::steady_clock::time_point lastSample {};
    bool                                  started = false;

    // Machine-wide series.
    SeriesRing cpuUsage {kWindowSamples};
    SeriesRing cpuClock {kWindowSamples};
    SeriesRing gpuUsage {kWindowSamples};
    SeriesRing gpuClock {kWindowSamples};

    // One logical core: the two windows it keeps, and the summary handed out.
    // The rings live under the kernel's own core numbers -- a core that went
    // offline and came back keeps its slot rather than inheriting a neighbour's
    // history -- and the summaries are updated in place, so Cores() is a view
    // over storage that does not move.
    struct Core {
        SeriesRing   usage {kCoreWindowSamples};
        SeriesRing   clock {kCoreWindowSamples};
    };
    std::vector<Core>          cores;
    std::vector<CoreTelemetry> summaries;

    // The summaries ordered busiest first, rebuilt whenever the window is
    // sampled -- the order a fixed-height panel asks for, at the pace the
    // numbers change rather than once per frame. Kept as storage the getter
    // hands a view into, so a frame allocates nothing to draw the list.
    std::vector<CoreTelemetry> busiest;

    SystemSnapshot snapshot;
    std::string    gpuNameHint;
    std::string    backend;

    bool enabled = true;

    // Called when the core count changes, which is once at start-up and again
    // only if the kernel starts or stops reporting a CPU.
    void ResizeCores(size_t count) {
        if (cores.size() == count) {
            return;
        }
        cores.resize(count); // a core the kernel has just started reporting opens empty
        summaries.resize(count);
        for (size_t index = 0; index < count; ++index) {
            summaries[index].index = static_cast<unsigned>(index);
        }
    }

    // The summaries Cores() hands out, from the windows as they stand, and the
    // same summaries in the order BusiestCores() hands them out.
    void RefreshCores() {
        for (size_t index = 0; index < cores.size(); ++index) {
            summaries[index].usagePct = cores[index].usage.Summary();
            summaries[index].clockMHz = cores[index].clock.Summary();
        }
        snapshot.coreCount = static_cast<unsigned>(summaries.size());

        // A core with no usage in its window has nothing to be busy with, so it
        // is not a candidate -- the list is of cores that answered.
        busiest.clear(); // the capacity survives, so this settles after the first sample
        for (const CoreTelemetry& summary: summaries) {
            if (summary.usagePct) {
                busiest.push_back(summary);
            }
        }
        std::ranges::sort(busiest, [](const CoreTelemetry& a, const CoreTelemetry& b) noexcept {
            return AverageUsage(a) > AverageUsage(b);
        });
    }
};

SystemTelemetry::SystemTelemetry(): _impl(std::make_unique<Impl>()) {
    _impl->enabled = !ProbeDisabledByEnvironment();
    if (!_impl->enabled) {
        _impl->snapshot.unavailableReason = "disabled by ZHLN_SYSTEM_TELEMETRY=0";
        ZHLN::Log("[Bench] system telemetry: disabled by ZHLN_SYSTEM_TELEMETRY=0.");
    }
}

SystemTelemetry::~SystemTelemetry() = default;

void SystemTelemetry::SetGpuNameHint(std::string_view vulkanDeviceName) {
    if (!_impl->gpuNameHint.empty() || vulkanDeviceName.empty()) {
        return; // the first non-empty name wins: a device does not change under us
    }
    _impl->gpuNameHint.assign(vulkanDeviceName);
}

void SystemTelemetry::Tick() {
    if (!_impl->enabled) {
        return;
    }

    // The probe is built on the first tick rather than in the constructor: it
    // binds a driver library and takes a first counter read, and a session that
    // only ever samples frames should not pay for that at construction. The
    // GPU-name hint is set by then, which is what lets the device be matched.
    if (_impl->probe == nullptr) {
        _impl->probe   = Platform::MakeSystemProbe(_impl->gpuNameHint);
        _impl->backend = std::string(_impl->probe->BackendName());
        if (_impl->backend.empty()) {
            // One line, once: a platform with no counters is a fact the reader
            // should not have to guess at from an empty panel.
            const std::string_view reason = _impl->probe->Unavailable();
            _impl->snapshot.unavailableReason = reason.empty() ? "nothing to read on this platform" : std::string(reason);
            ZHLN::Log("[Bench] system telemetry: {}.", _impl->snapshot.unavailableReason);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (_impl->started && (now - _impl->lastSample) < kSampleInterval) {
        return;
    }

    // The interval this sample covers: the paced interval normally, longer when
    // the host's frames were slow enough to miss a slot, and zero for the first
    // sample (nothing precedes it to measure).
    float spanSeconds = 0.0f;
    if (_impl->started) {
        spanSeconds = std::chrono::duration<float>(now - _impl->lastSample).count();
    }
    _impl->lastSample = now;
    _impl->started    = true;

    const std::optional<Platform::ProbeSample> reading = _impl->probe->Sample();
    if (!reading) {
        // The probe answered when it was built and stopped answering: /proc went
        // away, or the platform has nothing at all. The panel keeps the history
        // it has and says why it is not growing.
        const std::string_view reason = _impl->probe->Unavailable();
        _impl->snapshot.unavailableReason = reason.empty() ? "the platform stopped answering" : std::string(reason);
        return;
    }

    _impl->snapshot.unavailableReason.clear();

    // A series the platform did not answer with is left as it was: recording a
    // zero would put a level in the window that the machine never held.
    const auto push = [spanSeconds](SeriesRing& window, const std::optional<float>& value) {
        if (value) {
            window.Push(*value, spanSeconds);
        }
    };
    push(_impl->cpuUsage, reading->cpuUsagePct);
    push(_impl->cpuClock, reading->cpuClockMHz);
    push(_impl->gpuUsage, reading->gpuUsagePct);
    push(_impl->gpuClock, reading->gpuClockMHz);

    _impl->snapshot.cpuUsagePct   = _impl->cpuUsage.Summary();
    _impl->snapshot.cpuClockMHz   = _impl->cpuClock.Summary();
    _impl->snapshot.gpuUsagePct   = _impl->gpuUsage.Summary();
    _impl->snapshot.gpuClockMHz   = _impl->gpuClock.Summary();
    _impl->snapshot.windowSeconds = _impl->cpuUsage.WindowSeconds();

    const bool gpuAnswering = _impl->snapshot.gpuUsagePct.has_value() || _impl->snapshot.gpuClockMHz.has_value();
    _impl->snapshot.gpuUnavailableReason = gpuAnswering ? std::string() : std::string(_impl->probe->GpuUnavailable());

    if (const std::string_view device = _impl->probe->GpuName(); !device.empty()) {
        _impl->snapshot.gpuName.assign(device);
    } else if (!_impl->gpuNameHint.empty()) {
        _impl->snapshot.gpuName = _impl->gpuNameHint; // the renderer's own name is the label
    }

    // The cores: the probe's list sets the count, and each core carries only the
    // readings it actually had.
    _impl->ResizeCores(reading->cores.size());
    for (size_t index = 0; index < reading->cores.size() && index < _impl->cores.size(); ++index) {
        const Platform::ProbeCore& core = reading->cores[index];
        push(_impl->cores[index].usage, core.usagePct);
        push(_impl->cores[index].clock, core.clockMHz);
    }
    _impl->RefreshCores();

    ++_impl->snapshot.samples;
}

void SystemTelemetry::Reset() {
    _impl->cpuUsage.Reset();
    _impl->cpuClock.Reset();
    _impl->gpuUsage.Reset();
    _impl->gpuClock.Reset();
    for (Impl::Core& core: _impl->cores) {
        core.usage.Reset();
        core.clock.Reset();
    }

    // The device, the backend and the reason for an empty panel are facts about
    // the machine, not about the window: they survive a reset, while every
    // series starts over.
    std::string reason     = std::move(_impl->snapshot.unavailableReason);
    std::string gpuReason  = std::move(_impl->snapshot.gpuUnavailableReason);
    std::string gpuName    = std::move(_impl->snapshot.gpuName);

    _impl->snapshot                      = SystemSnapshot {};
    _impl->snapshot.unavailableReason    = std::move(reason);
    _impl->snapshot.gpuUnavailableReason = std::move(gpuReason);
    _impl->snapshot.gpuName              = std::move(gpuName);

    _impl->RefreshCores(); // which also rebuilds the busiest-first ordering
}

auto SystemTelemetry::Snapshot() const noexcept -> const SystemSnapshot& {
    return _impl->snapshot;
}

auto SystemTelemetry::Cores() const noexcept -> std::span<const CoreTelemetry> {
    return _impl->summaries;
}

auto SystemTelemetry::CoreUsageHistory(unsigned index, size_t count) const noexcept -> std::span<const float> {
    if (index >= _impl->cores.size()) {
        return {};
    }
    return _impl->cores[index].usage.Newest(count);
}

auto SystemTelemetry::BusiestCores(size_t count) const noexcept -> std::span<const CoreTelemetry> {
    return std::span<const CoreTelemetry>(_impl->busiest).first(std::min(count, _impl->busiest.size()));
}

auto SystemTelemetry::BackendName() const noexcept -> std::string_view {
    return _impl->backend;
}

auto SystemTelemetry::IsEnabled() const noexcept -> bool {
    return _impl->enabled;
}

} // namespace ZHLN::Bench
