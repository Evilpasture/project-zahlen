// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/FrameTelemetry.cpp
#include <Benchmark/Viewer/FrameTelemetry.hpp>
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
// being invented, which is why a freshly seen stage's line starts on the right.
void FillSpark(FrameStageRow& row, const float* history, size_t historyCount) noexcept {
    const size_t bars = kStageSparkBars;
    row.spark.fill(0.0f);
    if (history == nullptr || historyCount == 0) {
        return;
    }

    const size_t covered  = std::min(historyCount, bars);
    const size_t firstBar = bars - covered;
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
    m_frames.resize(kWindowFrames);
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

    // After the stages: the row carries this frame's GPU total.
    if (IsCapturing()) {
        WriteCaptureRow(frameMs);
    }
}

void FrameTelemetry::Reset() {
    m_write        = 0;
    m_filled       = 0;
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
    m_frames[m_write] = frameMs;
    m_write           = (m_write + 1) % kWindowFrames;
    if (m_filled < kWindowFrames) {
        ++m_filled;
    }
    ++m_snapshot.frames;

    UpdateChart();

    // A window shorter than the stride has nothing to partition, and the numbers
    // are wanted in the first second of a run: refresh every frame until the
    // window is a stride deep, then on the stride.
    if (m_filled < kPercentileStride || ++m_sinceStats >= kPercentileStride) {
        RefreshStatistics();
        m_sinceStats = 0;
    }
}

void FrameTelemetry::UpdateChart() noexcept {
    // The newest kChartFrames samples, oldest first: the rightmost bar is the
    // frame that just happened.
    const size_t shown = std::min(m_filled, kChartFrames);
    m_chartShown       = shown;
    for (size_t age = 0; age < shown; ++age) {
        m_chart[shown - 1 - age] = m_frames[RingIndex(age)];
    }
    for (size_t i = shown; i < kChartFrames; ++i) {
        m_chart[i] = 0.0f;
    }
}

void FrameTelemetry::RefreshStatistics() noexcept {
    if (m_filled == 0) {
        return;
    }

    double seconds = 0.0;
    float  minMs   = m_frames[0];
    float  maxMs   = m_frames[0];
    for (size_t i = 0; i < m_filled; ++i) {
        const float sample = m_frames[i];
        seconds += static_cast<double>(sample) * 0.001;
        minMs = std::min(minMs, sample);
        maxMs = std::max(maxMs, sample);
    }

    m_snapshot.windowSeconds = seconds;
    m_snapshot.avgFps        = seconds > 0.0 ? static_cast<float>(static_cast<double>(m_filled) / seconds) : 0.0f;
    m_snapshot.frameMsAvg    = static_cast<float>(seconds * 1000.0 / static_cast<double>(m_filled));
    m_snapshot.frameMsMin    = minMs;
    m_snapshot.frameMsMax    = maxMs;
    m_snapshot.frameMsLow1   = SlowestMean(0.01f);
    m_snapshot.frameMsLow01  = SlowestMean(0.001f);
}

auto FrameTelemetry::SlowestMean(float fraction) -> float {
    const size_t count = m_filled;
    if (count == 0) {
        return 0.0f;
    }

    // 1% of a 10 000-frame window is its 100 worst frames. Early in a run, when
    // the window holds fewer than 100 samples, the low is the single worst
    // frame -- the same rule, applied to a smaller set.
    size_t tail = static_cast<size_t>(static_cast<double>(fraction) * static_cast<double>(count));
    tail        = std::clamp<size_t>(tail, 1, count);

    std::copy_n(m_frames.begin(), count, m_scratch.begin());

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

auto FrameTelemetry::RingIndex(size_t age) const noexcept -> size_t {
    const size_t oldest = (m_write + kWindowFrames - m_filled) % kWindowFrames;
    return (oldest + age) % kWindowFrames;
}

void FrameTelemetry::SampleStages() noexcept {
    CPUProfiler::IterateMetrics(&FrameTelemetry::StageCallback, this);

    // The split and the GPU total come from the whole table rather than from
    // this frame's callbacks: rows survive ticks, so a stage that did not report
    // this frame keeps its place in the panel -- with its last measurement --
    // instead of vanishing from it.
    float total = 0.0f;
    for (size_t i = 0; i < m_gpuCount; ++i) {
        total += m_stages[i].lastMs;
    }
    m_snapshot.gpuTotalMs = total;
}

void FrameTelemetry::StageCallback(const char* name, float lastMs, float, const float* history, size_t historyCount, void* userData) noexcept {
    if (userData == nullptr || name == nullptr || name[0] == '\0') {
        return;
    }
    static_cast<FrameTelemetry*>(userData)->AccumulateStage(name, lastMs, history, historyCount);
}

void FrameTelemetry::AccumulateStage(std::string_view name, float lastMs, const float* history, size_t historyCount) noexcept {
    // The table is tens of rows and this runs once per metric per frame, so the
    // lookup is a binary search inside the metric's own run of rows: GPU stages
    // first, then CPU scopes, each alphabetical.
    const bool gpu        = IsGpuStageName(name);
    const auto start      = m_stages.begin();
    const auto blockBegin = gpu ? start : start + static_cast<std::ptrdiff_t>(m_gpuCount);
    const auto blockEnd   = gpu ? start + static_cast<std::ptrdiff_t>(m_gpuCount) : m_stages.end();
    const auto found      = std::lower_bound(blockBegin, blockEnd, name, ByName {});

    if (found != blockEnd && found->name == name) {
        found->lastMs = lastMs;
        FillSpark(*found, history, historyCount);
        return;
    }

    FrameStageRow& row = *m_stages.emplace(found, FrameStageRow {.name = std::string(name), .lastMs = lastMs, .gpuStage = gpu, .spark = {}});
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

    m_captureStart      = std::chrono::steady_clock::now();
    m_captureFrames     = 0;
    m_captureSinceFlush = 0;

    m_captureFile << "# Zahlen frame telemetry capture\n"
                     "# frame: index within this capture. time_s: seconds since it started.\n"
                     "# frame_ms: the host's frame interval. fps: 1000 / frame_ms.\n"
                     "# gpu_total_ms: this frame's GPU stage timings summed -- passes the graph submits\n"
                     "# concurrently overlap on the device, so it is a sum of spans, not a span of the frame.\n"
                     "# Every column after that is one profiler metric's most recent measurement, in ms: a metric\n"
                     "# that did not run this frame repeats its last value rather than dropping the cell.\n";
    m_captureFile << "frame,time_s,frame_ms,fps,gpu_total_ms";
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
    m_captureFile.flush();
    m_captureFile.close();
    m_captureColumns.clear();
}

void FrameTelemetry::WriteCaptureRow(float frameMs) noexcept {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_captureStart).count();
    const float  fps     = frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;

    std::string line = std::format("{},{:.6f},{:.4f},{:.2f},{:.4f}", m_captureFrames, seconds, frameMs, fps, m_snapshot.gpuTotalMs);

    // One cell per column, in the header's order. A column whose metric has left
    // the table leaves an empty cell rather than shifting the rest of the row.
    for (const std::string& column: m_captureColumns) {
        line.push_back(',');
        const auto found = std::lower_bound(m_stages.begin(), m_stages.end(), std::string_view(column), ByName {});
        if (found != m_stages.end() && found->name == column) {
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
