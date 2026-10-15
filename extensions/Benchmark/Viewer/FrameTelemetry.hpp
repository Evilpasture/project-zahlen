// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/FrameTelemetry.hpp
//
// Host-frame telemetry for the benchmark overlay: what the host's frames cost,
// next to what the benchmark's ticks cost.
//
// Two sources, one panel:
//
//   * The frame interval itself. Tick() reads steady_clock -- not ZHLN::Clock,
//     whose high_resolution_clock is libstdc++'s system_clock, because a
//     percentile an NTP step can move is not a measurement -- and keeps the last
//     kWindowFrames of them. Average FPS, average/min/max frame time and the
//     1% / 0.1% lows all come out of that ring. The lows are the mean of the
//     slowest 1% / 0.1% of the window, in milliseconds, which is the convention
//     PresentMon and CapFrameX report: the number a rolling average cannot give
//     you, because the worst frames are exactly the ones it averages away.
//
//   * The engine's own profiler. CPUProfiler already holds every ScopedTimer in
//     the frame -- physics, culling, gameplay, transform history, animation,
//     the network step -- and GpuProfiler pushes a timestamp pair per stage per
//     frame under the stage's reflected enumerator name (MainPass1, TAA,
//     SmaaBlend, ...), so all 27 stage timings and their histories are measured
//     every frame by the renderer already. Nothing in the tree reads them:
//     IterateMetrics had no caller at all. This is that reader -- one row per
//     metric, classified as a GPU stage or a CPU scope from its name.
//
//   * The machine under the frames. A SystemTelemetry inside this one samples
//     CPU usage and clocks -- machine-wide and per core -- and the GPU's
//     utilization and clock from whatever the platform exposes, on its own
//     four-per-second cadence. Its header names no platform and neither does
//     this one: the counters arrive through a probe whose only platform files
//     are translation units of this target.
//
// Capture writes one CSV row per frame (frame interval, throughput, GPU total,
// the machine's counters, every metric's value) under
// FS::Paths::CacheDir()/captures, so a run can be analysed after the fact with
// the numbers the panel was showing while it ran. Hardware rows repeat the last
// sample between hardware ticks -- the counters are read four times a second and
// frames arrive faster -- which is what the time_s column is for.
//
// This measures host frames; LogicBenchmark measures the ticks the host asks
// for. The split is deliberate -- see BenchmarkViewer.hpp for why the two share
// a panel anyway.
#pragma once

#include <Benchmark/Viewer/SystemTelemetry.hpp>
#include <Zahlen/Common.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Bench {

// Bars a stage row's sparkline keeps, sampled from the metric's own history
// (CPUProfiler holds 100 samples per metric; a bar is the worst sample of its
// slice, so a spike survives the downsample).
inline constexpr size_t kStageSparkBars = 12;

// Host frames a metric may go without reporting before its row counts as quiet.
// Thirty frames is a second and a half at twenty fps: long enough that a stage
// skipping a frame or two -- or a pass that runs on an alternate cadence -- does
// not flicker between current and quiet, short enough that a stage the frame
// graph has dropped stops being drawn as though it were still running.
inline constexpr uint64_t kStageStaleTicks = 30;

// One row of the profiler table: a GPU stage or a CPU scope, as the engine
// last measured it.
struct FrameStageRow {
    // The metric's name as the profiler knows it -- a stage's reflected
    // enumerator or a scope's source name -- copied at first sight, so a row
    // outlives any reallocation of the table it lives in.
    std::string name;

    // The most recent measurement the engine recorded for the metric, in ms.
    // A metric that did not run this frame still carries its last one: the
    // profiler is a set of counters, not a per-frame report.
    float lastMs = 0.0f;

    // True for a GPU stage, false for a CPU scope; from the name, see
    // IsGpuStageName in the .cpp.
    bool gpuStage = true;

    // The metric's own recent history, oldest first, downsampled to
    // kStageSparkBars bars, newest on the right. `filled` counts the bars that
    // came from the metric itself: the ones before it are an absence, which is
    // not the same thing as a sample that measured zero, and the two are drawn
    // as what they are rather than as the same bar.
    std::array<float, kStageSparkBars> spark {};
    size_t                             filled = 0;

    // The metric's own sample count as last seen, and how many ticks have gone
    // by without it growing. The profiler holds every metric it has ever seen,
    // so the row outlives the stage it describes and a metric that has stopped
    // reporting still arrives here -- with the same samples, the same
    // measurement, the same count. That count not moving is what says the row is
    // history rather than a reading (see kStageStaleTicks), and it is the only
    // thing that can: a steady stage takes a new sample every frame and its
    // numbers never change, while a stage that has gone quiet takes none.
    uint64_t samples    = 0;
    uint64_t quietTicks = 0;

    // True once the metric has been silent long enough that its number is
    // history rather than a reading (see kStageStaleTicks).
    [[nodiscard]] auto IsStale() const noexcept -> bool {
        return quietTicks > kStageStaleTicks;
    }
};

// One window's numbers, as the panel draws them.
struct FrameTelemetrySnapshot {
    uint64_t frames        = 0;    // frames recorded since construction or Reset()
    double   windowSeconds = 0.0;  // wall time the held samples cover
    float    avgFps        = 0.0f; // frames / windowSeconds -- the window's throughput
    float    instantFps    = 0.0f; // 1000 / frameMsLast
    float    frameMsLast   = 0.0f; // the most recent interval, clipped samples included
    float    frameMsAvg    = 0.0f; // mean of the window
    float    frameMsMin    = 0.0f;
    float    frameMsMax    = 0.0f;
    float    frameMsLow1   = 0.0f; // mean of the window's slowest 1%, in ms
    float    frameMsLow01  = 0.0f; // mean of the window's slowest 0.1%, in ms
    // The GPU stages of the frame just sampled, summed. Passes the graph
    // submits concurrently overlap on the device, so this is a sum of stage
    // spans rather than a span of the frame; see the note in the .cpp. Quiet
    // stages are not part of it: a pass that stopped running is not part of
    // what the device is being asked to do now.
    float    gpuTotalMs     = 0.0f;
    uint64_t clippedSamples = 0; // frames longer than kSampleCeilingMs, kept out of the window
};

class ZHLN_STATIC_API FrameTelemetry {
  public:
    // Frames the window holds: 10 000 is about 2.8 minutes at 60 Hz, and the
    // 1% / 0.1% lows are then the mean of its worst 100 / 10 frames.
    static constexpr size_t kWindowFrames = 10000;

    // Bars in the frame-time chart.
    static constexpr size_t kChartFrames = 128;

    // Frames between percentile recomputes. The recompute copies the window and
    // partitions it, so it runs on a stride instead of every frame: a measuring
    // tool that stalls the thing it measures has stopped measuring it.
    static constexpr size_t kPercentileStride = 30;

    // A frame longer than this is counted apart rather than folded into the
    // window. It is a breakpoint, a window switch or a shader compile, and one
    // of them in the chart would flatten the other 9 999 samples against the
    // top of the scale. The count is reported, so nothing is hidden.
    static constexpr float kSampleCeilingMs = 1000.0f;

    // Frames between flushes while capturing, so a crash costs seconds of log
    // rather than the whole run.
    static constexpr uint64_t kCaptureFlushEvery = 240;

    FrameTelemetry();
    ~FrameTelemetry();

    FrameTelemetry(const FrameTelemetry&)                    = delete;
    auto operator=(const FrameTelemetry&) -> FrameTelemetry& = delete;

    // One host frame: reads the clock, records the interval, re-reads the
    // engine's profiler and appends a capture row if one is running. Call it
    // once per engine frame; the viewer's drawing entry points call it
    // themselves, so a host only needs this to keep the window warm while both
    // panels are hidden.
    void Tick();

    // Drops the frame window and starts a new one. The profiler rows stay --
    // they are the profiler's state, not the window's -- and a running capture
    // is unaffected: it is a file on disk with its own frame counter.
    void Reset();

    [[nodiscard]] auto Snapshot() const noexcept -> const FrameTelemetrySnapshot&;

    // The machine's own counters, sampled by the same Tick() on their own
    // cadence. Exposed so a host can read them without the panel.
    [[nodiscard]] auto System() noexcept -> SystemTelemetry&;
    [[nodiscard]] auto System() const noexcept -> const SystemTelemetry&;

    // The device the renderer draws on, as Vulkan names it, so the platform
    // probe can match its GPU counters to that device rather than to whichever
    // one it finds first. Forwarded to SystemTelemetry; the first non-empty name
    // is the one that sticks.
    void SetGpuNameHint(std::string_view vulkanDeviceName);

    // The last kChartFrames intervals, oldest first. Shorter than kChartFrames
    // until that many frames have been recorded.
    [[nodiscard]] auto ChartSamples() const noexcept -> std::span<const float>;

    // Every metric the profiler knows: GPU stages first, then CPU scopes, each
    // run alphabetical -- the order the panel and the CSV both read. Refreshed
    // by Tick(); a caller that never ticks gets an empty table.
    [[nodiscard]] auto Stages() const noexcept -> std::span<const FrameStageRow>;

    // How many of Stages()'s leading rows are GPU stages.
    [[nodiscard]] auto GpuStageCount() const noexcept -> size_t;

    // Starts the CSV: one row per frame, from the next Tick() on. An empty path
    // means FS::Paths::CacheDir()/captures/frame-telemetry-<stamp>.csv, which is
    // where the engine keeps derived files a host may delete. False when the
    // file could not be opened; CapturePath() still says what the path was.
    // Starting a capture while one runs restarts it -- one file at a time.
    auto StartCapture(const std::filesystem::path& path = {}) -> bool;

    // Writes the summary line, flushes and closes. Safe to call when nothing is
    // capturing. The path and frame count stay readable afterwards, so a host
    // can report what the last capture produced.
    void StopCapture();

    [[nodiscard]] auto IsCapturing() const noexcept -> bool {
        return m_captureFile.is_open();
    }

    // The capture being written, or the last one written.
    [[nodiscard]] auto CapturePath() const noexcept -> const std::filesystem::path& {
        return m_capturePath;
    }

    [[nodiscard]] auto CapturedFrames() const noexcept -> uint64_t {
        return m_captureFrames;
    }

  private:
    // The profiler callback: a static member, because CPUProfiler::IterateMetrics
    // takes a plain function pointer and this object arrives as its userData.
    static void
        StageCallback(const char* name, float lastMs, float, const float* history, size_t historyCount, uint64_t sampleCount, void* userData) noexcept;

    // Appends one recovered metric to the table, or refreshes the row it
    // already has.
    void AccumulateStage(std::string_view name, float lastMs, const float* history, size_t historyCount, uint64_t sampleCount) noexcept;

    void SampleStages() noexcept;
    void RecordSample(float frameMs) noexcept;
    void UpdateChart() noexcept;
    void RefreshStatistics() noexcept;
    void WriteCaptureRow(float frameMs) noexcept;

    // The mean of the window's slowest `fraction` of frames, in ms.
    [[nodiscard]] auto SlowestMean(float fraction) -> float;

    // The ring's slot for the sample `age` frames back (0 = newest).
    [[nodiscard]] auto RingIndex(size_t age) const noexcept -> size_t;

    // The window: a ring of frame intervals, a scratch copy for the percentile
    // partition, and how much of the ring is live.
    std::vector<float> m_frames;
    std::vector<float> m_scratch;
    size_t             m_write        = 0;
    size_t             m_filled       = 0;
    size_t             m_sinceStats   = 0;
    bool               m_ticksStarted = false;

    std::chrono::steady_clock::time_point m_lastTick {};

    // The fields RefreshStatistics owns, cached between its strides.
    FrameTelemetrySnapshot m_snapshot {};

    // The chart's bars and how many of them are live. Written every frame:
    // unlike the percentile partition, this is 128 copies.
    std::array<float, kChartFrames> m_chart {};
    size_t                          m_chartShown = 0;

    // The profiler table, sorted by (GPU first, then CPU, each alphabetical) so
    // the panel and the CSV header read GPU-first without a second list.
    std::vector<FrameStageRow> m_stages;
    size_t                     m_gpuCount = 0;



    // The machine's counters: CPU and per-core usage and clocks, the GPU's
    // utilization and clock. Fed by Tick(), read by the panel and the capture.
    SystemTelemetry m_system;

    // Capture state. m_captureColumns is the table's order at StartCapture:
    // columns are fixed for the file, so a metric that first reports mid-run is
    // not in it (see the .cpp).
    std::ofstream                         m_captureFile;
    std::filesystem::path                 m_capturePath;
    std::vector<std::string>              m_captureColumns;
    std::vector<unsigned>                 m_captureCores; // the core columns of the file
    std::chrono::steady_clock::time_point m_captureStart {};
    uint64_t                              m_captureFrames     = 0;
    uint64_t                              m_captureSinceFlush = 0;
};

} // namespace ZHLN::Bench
