// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/FrameTelemetry.cpp
#include <Benchmark/Viewer/FrameTelemetry.hpp>
#include <optional>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <format>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>

namespace ZHLN::Bench {

namespace {

// One field of a series, for a CSV cell that has to be a number: the file's
// header says what a zero means in each of these columns, and zero is what an
// unanswered series writes.
[[nodiscard]] auto Reading(const std::optional<HardwareSeries>& series, float HardwareSeries::*member) noexcept -> float {
    return series ? (*series).*member : 0.0f;
}

// One series' most recent reading.
[[nodiscard]] auto LastReading(const std::optional<HardwareSeries>& series) noexcept -> float {
    return Reading(series, &HardwareSeries::last);
}

// GPU stages are recorded under their reflected enumerator name -- MainPass1,
// TAA, VolumetricLightInject -- while every CPU ScopedTimer in the tree is
// named in prose: "Physics (JPH)", "ECS System: Native C++ Gameplay Update",
// "Culling (ECS O(N))". By the time a reader sees a key the profiler has lost
// any type information, so the name is the only signal there is: identifier
// shaped is a stage, everything else is a CPU scope. A CPU scope named like an
// identifier would move into the GPU table, which is why the ones in the tree
// all read as sentences.
[[nodiscard]] auto IsGpuStageName(std::string_view name) noexcept -> bool {
    if (name.empty()) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) -> bool { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
}

// Where a capture lands when the caller names no path: the engine's cache
// directory, next to the pipeline cache -- derived data a host may delete.
[[nodiscard]] auto DefaultCapturePath() -> std::filesystem::path {
    std::array<char, 32> stamp {};
    const std::time_t    now = std::time(nullptr);
    std::tm              local {};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local);
    return ZHLN::FS::Paths::CacheDir() / "captures" / std::format("frame-telemetry-{}.csv", stamp.data());
}

// Profiler keys carry spaces and colons ("ECS System: Physics & Movement"), and
// quoting is CSV's own answer to that.
[[nodiscard]] auto Quoted(std::string_view text) -> std::string {
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('"');
    for (const char c: text) {
        quoted.push_back(c == '"' ? '\'' : c);
    }
    quoted.push_back('"');
    return quoted;
}

// One row's sparkline, from the metric's own history (oldest first, at most 100
// samples). Each bar is the worst sample of its slice, so a spike survives the
// downsample; bars older than the metric's first sample stay zero rather than
// being invented, which is why a freshly seen stage's line starts on the right,
// and `filled` carries how many bars are the metric's own so the panel can tell
// that padding apart from a sample that measured zero.
void FillSpark(FrameStageRow& row, const float* history, size_t historyCount) noexcept {
    const size_t bars = kStageSparkBars;
    row.spark.fill(0.0f);
    row.filled = 0;
    if (history == nullptr || historyCount == 0) {
        return;
    }

    const size_t covered  = std::min(historyCount, bars);
    const size_t firstBar = bars - covered;
    row.filled            = covered;
    for (size_t bar = 0; bar < covered; ++bar) {
        const size_t from = (bar * historyCount) / covered;
        const size_t to   = std::min(historyCount, std::max(from + 1, ((bar + 1) * historyCount) / covered));

        float peak = history[from];
        for (size_t i = from + 1; i < to; ++i) {
            peak = std::max(peak, history[i]);
        }
        row.spark[firstBar + bar] = peak;
    }
}

// lower_bound's half of the table lookup, against a name rather than a row.
struct ByName {
    auto operator()(const FrameStageRow& row, std::string_view name) const noexcept -> bool {
        return row.name < name;
    }
};

} // namespace

FrameTelemetry::FrameTelemetry() {
    m_scratch.resize(kWindowFrames);
}

FrameTelemetry::~FrameTelemetry() {
    // The summary line is worth having even when a host never stopped the
    // capture: the destructor is the last chance to write it.
    StopCapture();
}

void FrameTelemetry::Tick() {
    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = now - m_lastTick;
    m_lastTick         = now;

    if (!m_ticksStarted) {
        // The first tick only starts the clock: the interval before it belongs
        // to construction or to the frame the panel was switched on, not to a
        // frame that was measured.
        m_ticksStarted = true;
        SampleStages();
        return;
    }

    const float frameMs = std::chrono::duration<float, std::milli>(elapsed).count();

    m_snapshot.frameMsLast = frameMs;
    m_snapshot.instantFps  = frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;

    // The last frame is reported whatever it was -- the interval is a fact --
    // but a frame past the ceiling is counted apart instead of recorded into
    // the window (see the header).
    if (frameMs > kSampleCeilingMs) {
        ++m_snapshot.clippedSamples;
    } else {
        RecordSample(frameMs);
    }

    SampleStages();

    // The machine's counters, on their own cadence: this is a no-op on the
    // frames between hardware samples -- four a second, against however fast
    // the host's frames arrive.
    m_system.Tick();

    // After the stages: the row carries this frame's GPU total.
    if (IsCapturing()) {
        WriteCaptureRow(frameMs);
    }
}

void FrameTelemetry::Reset() {
    m_system.Reset();
    m_window.Clear();
    m_sinceStats   = 0;
    m_chartShown   = 0;
    m_ticksStarted = false;
    m_chart.fill(0.0f);
    m_snapshot = FrameTelemetrySnapshot {};

    // The next tick starts the new window: the interval since this call is not
    // a frame, so it is not a sample.
}

auto FrameTelemetry::Snapshot() const noexcept -> const FrameTelemetrySnapshot& {
    return m_snapshot;
}

auto FrameTelemetry::System() noexcept -> SystemTelemetry& {
    return m_system;
}

auto FrameTelemetry::System() const noexcept -> const SystemTelemetry& {
    return m_system;
}

void FrameTelemetry::SetGpuNameHint(std::string_view vulkanDeviceName) {
    m_system.SetGpuNameHint(vulkanDeviceName);
}

auto FrameTelemetry::ChartSamples() const noexcept -> std::span<const float> {
    return std::span<const float> {m_chart.data(), m_chartShown};
}

auto FrameTelemetry::Stages() const noexcept -> std::span<const FrameStageRow> {
    return std::span<const FrameStageRow> {m_stages.data(), m_stages.size()};
}

auto FrameTelemetry::GpuStageCount() const noexcept -> size_t {
    return m_gpuCount;
}

void FrameTelemetry::RecordSample(float frameMs) noexcept {
    m_window.Push(frameMs);
    ++m_snapshot.frames;

    UpdateChart();

    // A window shorter than the stride has nothing to partition, and the numbers
    // are wanted in the first second of a run: refresh every frame until the
    // window is a stride deep, then on the stride.
    if (m_window.Count() < kPercentileStride || ++m_sinceStats >= kPercentileStride) {
        RefreshStatistics();
        m_sinceStats = 0;
    }
}

void FrameTelemetry::UpdateChart() noexcept {
    // The newest kChartFrames samples, oldest first, with the newest at the
    // right: the window copies them into the end of the strip and says how many
    // it wrote. A window that has not filled leaves the bars before it at zero.
    m_chart.fill(0.0f);
    m_chartShown = m_window.CopyNewest(std::span<float>(m_chart));
}

void FrameTelemetry::RefreshStatistics() noexcept {
    const size_t count = m_window.Count();
    if (count == 0) {
        return;
    }

    // One pass over the window, oldest first: the wall time it covers, and its
    // fastest and slowest frame.
    double seconds = 0.0;
    float  minMs   = m_window.Newest();
    float  maxMs   = minMs;
    m_window.ForEachOldest([&](float sample) -> void {
        seconds += static_cast<double>(sample) * 0.001;
        minMs = std::min(minMs, sample);
        maxMs = std::max(maxMs, sample);
    });

    m_snapshot.windowSeconds = seconds;
    m_snapshot.avgFps        = seconds > 0.0 ? static_cast<float>(static_cast<double>(count) / seconds) : 0.0f;
    m_snapshot.frameMsAvg    = static_cast<float>(seconds * 1000.0 / static_cast<double>(count));
    m_snapshot.frameMsMin    = minMs;
    m_snapshot.frameMsMax    = maxMs;
    m_snapshot.frameMsLow1   = SlowestMean(0.01f);
    m_snapshot.frameMsLow01  = SlowestMean(0.001f);
}

auto FrameTelemetry::SlowestMean(float fraction) -> float {
    const size_t count = m_window.Count();
    if (count == 0) {
        return 0.0f;
    }

    // 1% of a 10 000-frame window is its 100 worst frames. Early in a run, when
    // the window holds fewer than 100 samples, the low is the single worst
    // frame -- the same rule, applied to a smaller set.
    size_t tail = static_cast<size_t>(static_cast<double>(fraction) * static_cast<double>(count));
    tail        = std::clamp<size_t>(tail, 1, count);

    m_window.CopyOldest(std::span<float>(m_scratch.data(), count));

    // nth_element leaves exactly the `tail` slowest samples in front of the cut,
    // unordered: the set is all a mean needs, and a partition costs one pass
    // where a sort of the whole window would cost log(n) of them.
    const auto begin = m_scratch.begin();
    const auto cut   = begin + static_cast<std::ptrdiff_t>(tail);
    const auto end   = begin + static_cast<std::ptrdiff_t>(count);
    std::nth_element(begin, cut, end, std::greater<float> {});

    double sum = 0.0;
    for (auto it = begin; it != cut; ++it) {
        sum += static_cast<double>(*it);
    }
    return static_cast<float>(sum / static_cast<double>(tail));
}

void FrameTelemetry::SampleStages() noexcept {
    // Every row ages by this tick, and the metrics that took a new sample clear
    // their own age as they are read (see AccumulateStage). That is what tells a
    // stage that is steady -- a sample every frame, all of them equal -- from one
    // that has stopped being submitted: only the second stops clearing it.
    for (FrameStageRow& row: m_stages) {
        ++row.quietTicks;
    }

    CPUProfiler::IterateMetrics(&FrameTelemetry::StageCallback, this);

    // The split and the GPU total come from the whole table rather than from
    // this frame's callbacks: rows survive ticks, so a stage that did not report
    // this frame keeps its place in the panel -- with its last measurement --
    // instead of vanishing from it.
    //
    // What a row may not keep is its claim to be current. The profiler holds
    // every metric it has ever seen, so a stage the frame graph has stopped
    // including would otherwise sit in the table with a frozen number and go on
    // adding that number to a total the panel calls this frame's cost. A quiet
    // stage is not part of what the frame is paying.
    float total = 0.0f;
    for (size_t i = 0; i < m_gpuCount; ++i) {
        if (!m_stages[i].IsStale()) {
            total += m_stages[i].lastMs;
        }
    }
    m_snapshot.gpuTotalMs = total;
}

void FrameTelemetry::StageCallback(
    const char* name, float lastMs, float, const float* history, size_t historyCount, uint64_t sampleCount, void* userData
) noexcept {
    if (userData == nullptr || name == nullptr || name[0] == '\0') {
        return;
    }
    static_cast<FrameTelemetry*>(userData)->AccumulateStage(name, lastMs, history, historyCount, sampleCount);
}

void FrameTelemetry::AccumulateStage(std::string_view name, float lastMs, const float* history, size_t historyCount, uint64_t sampleCount) noexcept {
    // The table is tens of rows and this runs once per metric per frame, so the
    // lookup is a binary search inside the metric's own run of rows: GPU stages
    // first, then CPU scopes, each alphabetical.
    const bool gpu        = IsGpuStageName(name);
    const auto start      = m_stages.begin();
    const auto blockBegin = gpu ? start : start + static_cast<std::ptrdiff_t>(m_gpuCount);
    const auto blockEnd   = gpu ? start + static_cast<std::ptrdiff_t>(m_gpuCount) : m_stages.end();
    const auto found      = std::lower_bound(blockBegin, blockEnd, name, ByName {});

    if (found != blockEnd && found->name == name) {
        FrameStageRow& row = *found;

        // The metric reported since the last tick -- its count grew -- which is
        // what keeps the row current and what makes its measurement the one to
        // draw. A metric that did not is left as it is, age and all.
        if (row.samples != sampleCount) {
            row.samples    = sampleCount;
            row.quietTicks = 0;
            row.lastMs     = lastMs;
        }
        FillSpark(row, history, historyCount);
        return;
    }

    FrameStageRow& row = *m_stages.emplace(
        found, FrameStageRow {.name = std::string(name), .lastMs = lastMs, .gpuStage = gpu, .spark = {}, .samples = sampleCount, .quietTicks = 0}
    );
    FillSpark(row, history, historyCount);

    if (gpu) {
        ++m_gpuCount;
    }
}

auto FrameTelemetry::StartCapture(const std::filesystem::path& path) -> bool {
    // One file at a time: a second start replaces the first rather than
    // interleaving two logs.
    StopCapture();

    const std::filesystem::path target = path.empty() ? DefaultCapturePath() : path;
    m_capturePath                      = target;

    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    m_captureFile.open(target, std::ios::out | std::ios::trunc);
    if (!m_captureFile) {
        m_captureFile.clear();
        return false;
    }

    // The columns are the table as it stands. A metric that first reports
    // mid-capture has no column: the alternative is a file whose header depends
    // on when the first frame landed in it.
    if (m_stages.empty()) {
        SampleStages();
    }
    m_captureColumns.clear();
    m_captureColumns.reserve(m_stages.size());
    for (const FrameStageRow& row: m_stages) {
        m_captureColumns.push_back(row.name);
    }

    // The core columns are named for the cores the machine reports right now; a
    // machine whose core count changes mid-run does not grow columns.
    m_captureCores.clear();
    for (const CoreTelemetry& core: m_system.Cores()) {
        m_captureCores.push_back(core.index);
    }

    m_captureStart      = std::chrono::steady_clock::now();
    m_captureFrames     = 0;
    m_captureSinceFlush = 0;

    m_captureFile << "# Zahlen frame telemetry capture\n"
                     "# frame: index within this capture. time_s: seconds since it started.\n"
                     "# frame_ms: the host's frame interval. fps: 1000 / frame_ms.\n"
                     "# gpu_total_ms: this frame's GPU stage timings summed -- passes the graph submits\n"
                     "# concurrently overlap on the device, so it is a sum of spans, not a span of the frame.\n"
                     "# cpu_* / gpu_* : the machine's counters, read four times a second, so these repeat the\n"
                     "# last hardware sample between reads. _p1 / _p99 are the tails of the hardware window (a\n"
                     "# value at that percentile, not a mean); clocks are MHz.\n"
                     "# cNN_pct / cNN_mhz: one logical CPU's last usage and clock, 0 when the platform does not\n"
                     "# expose that core's clock.\n"
                     "# Every column after that is one profiler metric's most recent measurement, in ms: a metric\n"
                     "# that did not run this frame repeats its last value rather than dropping the cell.\n";
    m_captureFile << "frame,time_s,frame_ms,fps,gpu_total_ms,cpu_pct,cpu_p1,cpu_p99,cpu_clk_mhz,cpu_clk_p1,cpu_clk_p99,gpu_pct,gpu_p1,"
                     "gpu_p99,gpu_clk_mhz,gpu_clk_p1,gpu_clk_p99";
    for (const unsigned core: m_captureCores) {
        m_captureFile << std::format(",c{:02}_pct,c{:02}_mhz", core, core);
    }
    for (const std::string& column: m_captureColumns) {
        m_captureFile << ',' << Quoted(column);
    }
    m_captureFile << '\n';
    return true;
}

void FrameTelemetry::StopCapture() {
    if (!m_captureFile.is_open()) {
        return;
    }

    // The one line a reader wants before the rows: the numbers the panel was
    // showing when the capture stopped.
    const FrameTelemetrySnapshot& snapshot = m_snapshot;
    m_captureFile << std::format(
        "# summary: {} frames recorded, {:.2f} s window, avg {:.2f} fps, frame ms avg {:.3f} min {:.3f} max {:.3f}, "
        "1% low {:.3f}, 0.1% low {:.3f}, {} frames over {:.0f} ms counted apart\n",
        snapshot.frames, snapshot.windowSeconds, snapshot.avgFps, snapshot.frameMsAvg, snapshot.frameMsMin, snapshot.frameMsMax, snapshot.frameMsLow1,
        snapshot.frameMsLow01, snapshot.clippedSamples, kSampleCeilingMs
    );

    // The machine, when it answered: the same numbers the panel draws, so a
    // capture read months later does not need the panel to be interpreted.
    const SystemSnapshot& system = m_system.Snapshot();
    if (const std::string_view backend = m_system.BackendName(); !backend.empty()) {
        m_captureFile << std::format("# hardware: {} ({} samples, {:.0f} s window)\n", backend, system.samples, system.windowSeconds);
    }
    if (system.cpuUsagePct) {
        m_captureFile << std::format(
            "# cpu: usage avg {:.1f}% p1 {:.1f}% p99 {:.1f}%   clock avg {:.0f} MHz p1 {:.0f} p99 {:.0f}\n",
            Reading(system.cpuUsagePct, &HardwareSeries::avg), Reading(system.cpuUsagePct, &HardwareSeries::p1), Reading(system.cpuUsagePct, &HardwareSeries::p99),
            Reading(system.cpuClockMHz, &HardwareSeries::avg), Reading(system.cpuClockMHz, &HardwareSeries::p1), Reading(system.cpuClockMHz, &HardwareSeries::p99)
        );
    }
    if (system.gpuUsagePct || system.gpuClockMHz) {
        m_captureFile << std::format(
            "# gpu: {}  usage avg {:.1f}% p1 {:.1f}% p99 {:.1f}%   clock avg {:.0f} MHz p1 {:.0f} p99 {:.0f}\n", system.gpuName,
            Reading(system.gpuUsagePct, &HardwareSeries::avg), Reading(system.gpuUsagePct, &HardwareSeries::p1), Reading(system.gpuUsagePct, &HardwareSeries::p99),
            Reading(system.gpuClockMHz, &HardwareSeries::avg), Reading(system.gpuClockMHz, &HardwareSeries::p1), Reading(system.gpuClockMHz, &HardwareSeries::p99)
        );
    } else if (!system.gpuUnavailableReason.empty()) {
        m_captureFile << std::format("# hardware note: {}\n", system.gpuUnavailableReason);
    }
    m_captureFile.flush();
    m_captureFile.close();
    m_captureColumns.clear();
    m_captureCores.clear();
}

void FrameTelemetry::WriteCaptureRow(float frameMs) noexcept {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_captureStart).count();
    const float  fps     = frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;

    const SystemSnapshot& system = m_system.Snapshot();
    std::string           line   = std::format(
        "{},{:.6f},{:.4f},{:.2f},{:.4f},{:.2f},{:.2f},{:.2f},{:.0f},{:.0f},{:.0f},{:.2f},{:.2f},{:.2f},{:.0f},{:.0f},{:.0f}", m_captureFrames, seconds,
        frameMs, fps, m_snapshot.gpuTotalMs, Reading(system.cpuUsagePct, &HardwareSeries::last), Reading(system.cpuUsagePct, &HardwareSeries::p1),
        Reading(system.cpuUsagePct, &HardwareSeries::p99), Reading(system.cpuClockMHz, &HardwareSeries::last), Reading(system.cpuClockMHz, &HardwareSeries::p1),
        Reading(system.cpuClockMHz, &HardwareSeries::p99), Reading(system.gpuUsagePct, &HardwareSeries::last), Reading(system.gpuUsagePct, &HardwareSeries::p1),
        Reading(system.gpuUsagePct, &HardwareSeries::p99), Reading(system.gpuClockMHz, &HardwareSeries::last), Reading(system.gpuClockMHz, &HardwareSeries::p1),
        Reading(system.gpuClockMHz, &HardwareSeries::p99)
    );

    // One pair of cells per core column, in the header's order.
    const std::span<const CoreTelemetry> cores = m_system.Cores();
    for (const unsigned core: m_captureCores) {
        const CoreTelemetry* row = (core < cores.size() && cores[core].index == core) ? &cores[core] : nullptr;
        if (row == nullptr) {
            line.append(",0,0");
            continue;
        }
        std::format_to(std::back_inserter(line), ",{:.2f},{:.0f}", LastReading(row->usagePct), LastReading(row->clockMHz));
    }

    // One cell per column, in the header's order. A column whose metric has left
    // the table leaves an empty cell rather than shifting the rest of the row.
    //
    // The table is two sorted runs -- GPU stages first, CPU scopes after them --
    // and is only sorted by name *inside* each run. Searching the whole vector
    // assumes a single sorted range and silently misses every metric whose name
    // would sort into the other run, which is how the capture came to write
    // blank cells for every CPU scope. Search the run the name belongs to, as
    // AccumulateStage already does.
    const auto gpuBoundary = m_stages.begin() + static_cast<std::ptrdiff_t>(m_gpuCount);
    for (const std::string& column: m_captureColumns) {
        line.push_back(',');
        const bool gpu        = IsGpuStageName(column);
        const auto runBegin   = gpu ? m_stages.begin() : gpuBoundary;
        const auto runEnd     = gpu ? gpuBoundary : m_stages.end();
        const auto found      = std::lower_bound(runBegin, runEnd, std::string_view(column), ByName {});
        if (found != runEnd && found->name == column) {
            std::format_to(std::back_inserter(line), "{:.4f}", found->lastMs);
        }
    }
    line.push_back('\n');

    m_captureFile << line;
    ++m_captureFrames;
    if (++m_captureSinceFlush >= kCaptureFlushEvery) {
        m_captureFile.flush();
        m_captureSinceFlush = 0;
    }
}

} // namespace ZHLN::Bench
