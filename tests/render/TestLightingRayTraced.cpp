// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestLightingRayTraced.cpp
//
// Verification for the lighting + raytracing pipeline:
//
//   1. Flicker: a fully static, fully lit scene must produce temporally
//      stable frames. A light popping in/out of a cluster, a reflection
//      cache missing for one frame or a TLAS rebuild hitch shows up here.
//   2. Accidental light culling: a single point light glides across the
//      screen, crossing cluster-cell and z-slice boundaries, while its red
//      signature on the ground/box must never vanish or collapse. After the
//      sweep the light is frozen in place and 8 consecutive frames must be
//      identical (no oscillation, no step).
//   3. Bisect for the above: the same static light on a FRESH engine with no
//      motion history pins whether an observed instability is carried over
//      from the sweep (stale double-buffered state) or is a pure per-frame
//      race. Both scenarios are HARD gates.
//   4. Ray-traced shadows: an occluder between the sun and the ground must
//      carve a real, stable shadow (not a full-scene blackout, not nothing),
//      and removing it must restore a near-uniformly lit floor.
//   5. Reflections: a polished plane must mirror a bright emissive object
//      with the engine's DEFAULT reflection path (SSR), stable frame to frame.
//   6. Multi-Light Clustered Illumination & Chromatic Blending: 64 point lights
//      in 4 color quadrants; tests cluster accumulation & additive mixing in .ppm.
//   7. Multi-Emissive Sources & Reflection Mapping: Multiple distinct emissive
//      objects mirrored on a polished plane with spatial .ppm column analysis.
//   8. Dense Multi-Light & Emissive Cross-Interaction: 32 dynamic point lights
//      interacting with an emissive monolith over a diverse PBR material grid.

#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// ============================================================================
// Test Error Types
// ============================================================================

enum class LightingRTTestError : uint8_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for the lighting/raytracing test."> {}]] = 1,
    RenderOutputBlank[[= ZHLN::Description<"Rendered frame is blank or could not be captured."> {}]],
    TemporalFlickerDetected[[= ZHLN::Description<"A static fully-lit scene changed more frame-to-frame than the engine's own noise floor."> {}]],
    LightCullingPopDetected[[= ZHLN::Description<"A point light inside the frustum/range lost its lighting contribution for a frame (cluster culling)."> {}]],
    RayTracedShadowFailed[[= ZHLN::Description<"The ray-traced sun shadow did not appear, disappeared, or took out the whole frame."> {}]],
    ReflectionMissing[[= ZHLN::Description<"The polished surface shows no reflection of the emissive object (RTR/SSR fell back to IBL)."> {}]],
    ReflectionArtifacts[[= ZHLN::Description<"The reflected region contains blowout, ray-debris speckles, or flicker."> {}]],
    MultiLightClusteringFailed[[= ZHLN::Description<"Multi-light clustered accumulation or chromatic blending mismatch detected."> {}]],
    MultiEmissiveReflectionFailed[[= ZHLN::Description<"Multi-emissive source reflection analysis failed: missing spatial mirror correspondence or color "
                                                       "fidelity."> {}]],
    DenseCrossInteractionFailed[[= ZHLN::Description<"Dense multi-light & emissive interaction produced blowout, NaN/Inf, or lighting failure."> {}]],
    DeviceLostDuringTest[[= ZHLN::Description<"The Vulkan device was lost repeatedly during the scenario; the engine hot-rebuild recovered, but the GPU was "
                                              "not stable."> {}]],
    ValidationErrorsRaised[[= ZHLN::Description<"The validation layer reported errors while rendering the lighting/raytracing frames."> {}]],
};

// ============================================================================
// Image & Metric Helpers
// ============================================================================

namespace {

struct RgbImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 && rgb.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    }
};

[[nodiscard]] RgbImage LoadPPM(const std::string& path) {
    RgbImage      img;
    std::ifstream ppm(path, std::ios::binary);
    if (!ppm.is_open()) {
        return img;
    }

    std::string header;
    int         maxColor = 0;
    ppm >> header >> img.width >> img.height >> maxColor;
    ppm.get();

    if (img.width <= 0 || img.height <= 0) {
        return {};
    }

    img.rgb.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3u);
    ppm.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    return img;
}

[[nodiscard]] std::string PngPathOf(std::string_view ppmPath) {
    std::string png(ppmPath);
    if (png.size() >= 4 && (png.ends_with(".ppm") || png.ends_with(".PPM"))) {
        png.resize(png.size() - 4);
    }
    png += ".png";
    return png;
}

[[nodiscard]] bool SavePNG(const std::string& path, const RgbImage& img) {
    if (!img.Valid()) {
        return false;
    }
    return stbi_write_png(path.c_str(), img.width, img.height, 3, img.rgb.data(), img.width * 3) != 0;
}

inline double Luma(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) + 0.0722 * static_cast<double>(b);
}

struct FrameMetrics {
    uint32_t total       = 0;
    uint32_t lit         = 0;
    uint32_t dark        = 0;
    uint32_t saturated   = 0;
    uint32_t red         = 0;
    uint32_t green       = 0;
    uint32_t blue        = 0;
    uint32_t yellow      = 0;
    uint32_t cyan        = 0;
    uint32_t redPeak     = 0;
    uint32_t redIsolated = 0;
    double   meanLuma    = 0.0;
};

struct NormalizedRect {
    double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;
};

struct SubRegionStats {
    uint32_t pixels      = 0;
    double   meanR       = 0.0;
    double   meanG       = 0.0;
    double   meanB       = 0.0;
    double   meanLuma    = 0.0;
    double   maxLuma     = 0.0;
    uint32_t dominantRed = 0;
    uint32_t dominantGrn = 0;
    uint32_t dominantBlu = 0;
    uint32_t yellowMix   = 0;
    uint32_t cyanMix     = 0;
    uint32_t saturated   = 0;
};

[[nodiscard]] SubRegionStats MeasureSubRegion(const RgbImage& img, const NormalizedRect& rect) {
    SubRegionStats stats;
    if (!img.Valid()) {
        return stats;
    }

    const int x0 = std::clamp(static_cast<int>(rect.x0 * img.width), 0, img.width - 1);
    const int y0 = std::clamp(static_cast<int>(rect.y0 * img.height), 0, img.height - 1);
    const int x1 = std::clamp(static_cast<int>(rect.x1 * img.width), x0 + 1, img.width);
    const int y1 = std::clamp(static_cast<int>(rect.y1 * img.height), y0 + 1, img.height);

    double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumL = 0.0;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t  i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
            const uint8_t r = img.rgb[i + 0];
            const uint8_t g = img.rgb[i + 1];
            const uint8_t b = img.rgb[i + 2];
            const double  l = Luma(r, g, b);

            sumR += r;
            sumG += g;
            sumB += b;
            sumL += l;
            stats.maxLuma = std::max(stats.maxLuma, l);
            ++stats.pixels;

            if (r >= 45 && r >= 1.35 * g && r >= 1.35 * b) {
                ++stats.dominantRed;
            }
            if (g >= 45 && g >= 1.35 * r && g >= 1.35 * b) {
                ++stats.dominantGrn;
            }
            if (b >= 45 && b >= 1.35 * r && b >= 1.35 * g) {
                ++stats.dominantBlu;
            }
            if (r >= 45 && g >= 45 && b <= 0.60 * std::min(r, g)) {
                ++stats.yellowMix;
            }
            if (g >= 45 && b >= 45 && r <= 0.60 * std::min(g, b)) {
                ++stats.cyanMix;
            }
            if (r >= 250 && g >= 250 && b >= 250) {
                ++stats.saturated;
            }
        }
    }

    if (stats.pixels > 0) {
        const double n = static_cast<double>(stats.pixels);
        stats.meanR    = sumR / n;
        stats.meanG    = sumG / n;
        stats.meanB    = sumB / n;
        stats.meanLuma = sumL / n;
    }

    return stats;
}

[[nodiscard]] FrameMetrics MeasureImage(const RgbImage& img, double minRowFraction = 0.0) {
    FrameMetrics m;
    if (!img.Valid()) {
        return m;
    }

    const int minRow = static_cast<int>(std::ceil(minRowFraction * static_cast<double>(img.height)));

    double lumaSum = 0.0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        const size_t pixel = i / 3;
        const int    y     = static_cast<int>(pixel / static_cast<size_t>(img.width));
        if (y < minRow) {
            continue;
        }

        const uint8_t r = img.rgb[i + 0];
        const uint8_t g = img.rgb[i + 1];
        const uint8_t b = img.rgb[i + 2];
        const double  l = Luma(r, g, b);

        ++m.total;
        lumaSum += l;

        if (l > 40.0) {
            ++m.lit;
        }
        if (l < 24.0) {
            ++m.dark;
        }
        if (r >= 250 && g >= 250 && b >= 250) {
            ++m.saturated;
        }
        if (r >= 60 && r >= 1.6 * static_cast<double>(g) && r >= 1.6 * static_cast<double>(b)) {
            ++m.red;
            m.redPeak = std::max(m.redPeak, static_cast<uint32_t>(r));

            const int  x               = static_cast<int>(pixel % static_cast<size_t>(img.width));
            uint32_t   blackNeighbours = 0;
            const auto isBlackAt       = [&](int nx, int ny) -> bool {
                if (nx < 0 || ny < 0 || nx >= img.width || ny >= img.height) {
                    return true;
                }
                const size_t ni = (static_cast<size_t>(ny) * static_cast<size_t>(img.width) + static_cast<size_t>(nx)) * 3u;
                return static_cast<int>(img.rgb[ni + 0]) + static_cast<int>(img.rgb[ni + 1]) + static_cast<int>(img.rgb[ni + 2]) <= 6;
            };
            blackNeighbours += isBlackAt(x - 1, y) ? 1u : 0u;
            blackNeighbours += isBlackAt(x + 1, y) ? 1u : 0u;
            blackNeighbours += isBlackAt(x, y - 1) ? 1u : 0u;
            blackNeighbours += isBlackAt(x, y + 1) ? 1u : 0u;
            if (blackNeighbours >= 3) {
                ++m.redIsolated;
            }
        }
        if (g >= 60 && g >= 1.6 * static_cast<double>(r) && g >= 1.6 * static_cast<double>(b)) {
            ++m.green;
        }
        if (b >= 60 && b >= 1.6 * static_cast<double>(r) && b >= 1.6 * static_cast<double>(g)) {
            ++m.blue;
        }
        if (r >= 60 && g >= 60 && b <= 50) {
            ++m.yellow;
        }
        if (g >= 60 && b >= 60 && r <= 50) {
            ++m.cyan;
        }
    }

    if (m.total > 0) {
        m.meanLuma = lumaSum / static_cast<double>(m.total);
    }
    return m;
}

// ============================================================================
// Named Expectations
// ============================================================================

// ExpectTrue files the failure against its file:line, which is all the summary
// prints -- enough to locate the statement, not enough to tell which operand
// missed or by how much. This wraps it and echoes the label plus the measured
// operands, so a red run names the failed check and the frame statistics behind
// it instead of a bare "Expected condition to be: true".
template <typename... Args>
[[nodiscard]] bool CheckCondition(bool condition, std::string_view label, std::string_view fmt, Args&&... args) {
    if (ZHLN::Test::ExpectTrue(condition)) {
        return true;
    }
    ZHLN::Println("      {}[CHECK FAILED]{} {}", ZHLN::Color::Red, ZHLN::Color::Reset, label);
    ZHLN::Println("        {}", ZHLN::Format(fmt, std::forward<Args>(args)...).string_view());
    return false;
}

struct FrameDiff {
    uint32_t over12  = 0;
    uint32_t over32  = 0;
    double   meanAbs = 0.0;
    double   frac12  = 0.0;
    double   frac32  = 0.0;
};

struct ChangedRegion {
    uint32_t count     = 0;
    int      minX      = 0;
    int      maxX      = 0;
    int      minY      = 0;
    int      maxY      = 0;
    int      maxDelta  = 0;
    double   meanDelta = 0.0;
    double   aR = 0.0, aG = 0.0, aB = 0.0;
    double   bR = 0.0, bG = 0.0, bB = 0.0;
};

[[nodiscard]] ChangedRegion DiffRegion(const RgbImage& a, const RgbImage& b, int threshold = 32) {
    ChangedRegion r;
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return r;
    }

    r.minX       = a.width;
    r.minY       = a.height;
    r.maxX       = -1;
    r.maxY       = -1;
    uint64_t sum = 0, sumAr = 0, sumAg = 0, sumAb = 0, sumBr = 0, sumBg = 0, sumBb = 0;

    for (size_t i = 0; i < a.rgb.size(); i += 3) {
        const int dr    = std::abs(static_cast<int>(a.rgb[i + 0]) - static_cast<int>(b.rgb[i + 0]));
        const int dg    = std::abs(static_cast<int>(a.rgb[i + 1]) - static_cast<int>(b.rgb[i + 1]));
        const int db    = std::abs(static_cast<int>(a.rgb[i + 2]) - static_cast<int>(b.rgb[i + 2]));
        const int worst = std::max({dr, dg, db});
        r.maxDelta      = std::max(r.maxDelta, worst);
        if (worst > threshold) {
            const size_t pixel = i / 3;
            const int    x     = static_cast<int>(pixel % static_cast<size_t>(a.width));
            const int    y     = static_cast<int>(pixel / static_cast<size_t>(a.width));
            ++r.count;
            r.minX = std::min(r.minX, x);
            r.maxX = std::max(r.maxX, x);
            r.minY = std::min(r.minY, y);
            r.maxY = std::max(r.maxY, y);
            sum += static_cast<uint64_t>(worst);
            sumAr += a.rgb[i + 0];
            sumAg += a.rgb[i + 1];
            sumAb += a.rgb[i + 2];
            sumBr += b.rgb[i + 0];
            sumBg += b.rgb[i + 1];
            sumBb += b.rgb[i + 2];
        }
    }

    if (r.count == 0) {
        r.minX = r.maxX = r.minY = r.maxY = 0;
    } else {
        r.meanDelta = static_cast<double>(sum) / static_cast<double>(r.count);
        r.aR        = static_cast<double>(sumAr) / r.count;
        r.aG        = static_cast<double>(sumAg) / r.count;
        r.aB        = static_cast<double>(sumAb) / r.count;
        r.bR        = static_cast<double>(sumBr) / r.count;
        r.bG        = static_cast<double>(sumBg) / r.count;
        r.bB        = static_cast<double>(sumBb) / r.count;
    }
    return r;
}

void WriteRegionCrop(const std::string& path, const RgbImage& img, const ChangedRegion& region) {
    if (!img.Valid() || region.count == 0) {
        return;
    }
    const int x0 = std::max(0, region.minX);
    const int y0 = std::max(0, region.minY);
    const int x1 = std::min(img.width - 1, region.maxX);
    const int y1 = std::min(img.height - 1, region.maxY);
    if (x1 < x0 || y1 < y0) {
        return;
    }

    const int     w = x1 - x0 + 1;
    const int     h = y1 - y0 + 1;
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return;
    }
    out << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> crop(static_cast<size_t>(w) * h * 3);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t src = (static_cast<size_t>(y) * img.width + static_cast<size_t>(x)) * 3u;
            const size_t dst = (static_cast<size_t>(y - y0) * w + static_cast<size_t>(x - x0)) * 3u;
            crop[dst + 0]    = img.rgb[src + 0];
            crop[dst + 1]    = img.rgb[src + 1];
            crop[dst + 2]    = img.rgb[src + 2];
        }
    }
    out.write(reinterpret_cast<const char*>(crop.data()), static_cast<std::streamsize>(crop.size()));

    RgbImage pngCrop {.width = w, .height = h, .rgb = crop};
    (void) SavePNG(PngPathOf(path), pngCrop);
}

void WriteAmplifiedDiff(const std::string& path, const RgbImage& a, const RgbImage& b) {
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return;
    }
    out << "P6\n" << a.width << " " << a.height << "\n255\n";

    std::vector<uint8_t> amplified(a.rgb.size());
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const int d  = std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
        amplified[i] = static_cast<uint8_t>(std::min(255, d * 4));
    }
    out.write(reinterpret_cast<const char*>(amplified.data()), static_cast<std::streamsize>(amplified.size()));

    RgbImage pngDiff {.width = a.width, .height = a.height, .rgb = amplified};
    (void) SavePNG(PngPathOf(path), pngDiff);
}

[[nodiscard]] FrameDiff CompareFrames(const RgbImage& a, const RgbImage& b) {
    FrameDiff d;
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return d;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < a.rgb.size(); i += 3) {
        const int dr    = std::abs(static_cast<int>(a.rgb[i + 0]) - static_cast<int>(b.rgb[i + 0]));
        const int dg    = std::abs(static_cast<int>(a.rgb[i + 1]) - static_cast<int>(b.rgb[i + 1]));
        const int db    = std::abs(static_cast<int>(a.rgb[i + 2]) - static_cast<int>(b.rgb[i + 2]));
        const int worst = std::max({dr, dg, db});
        sum += static_cast<uint64_t>(dr + dg + db);
        if (worst > 12) {
            ++d.over12;
        }
        if (worst > 32) {
            ++d.over32;
        }
    }

    const size_t pixels = a.rgb.size() / 3;
    if (pixels > 0) {
        d.meanAbs = static_cast<double>(sum) / (static_cast<double>(pixels) * 3.0);
        d.frac12  = static_cast<double>(d.over12) / static_cast<double>(pixels);
        d.frac32  = static_cast<double>(d.over32) / static_cast<double>(pixels);
    }
    return d;
}

[[nodiscard]] double Mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double v: values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

[[nodiscard]] double StdDev(const std::vector<double>& values, double mean) {
    if (values.size() < 2) {
        return 0.0;
    }
    double sumSq = 0.0;
    for (double v: values) {
        const double d = v - mean;
        sumSq += d * d;
    }
    return std::sqrt(sumSq / static_cast<double>(values.size() - 1));
}

[[nodiscard]] double CoefficientOfVariation(const std::vector<double>& values) {
    const double mean = Mean(values);
    if (mean <= 1e-9) {
        return 0.0;
    }
    return StdDev(values, mean) / mean;
}

} // namespace

// ============================================================================
// Device-Lost-Aware Scenario Runner
// ============================================================================

namespace {

enum class StableRunResult : uint8_t { Ok, AssertionsFailed, PersistentDeviceLost };

constexpr uint32_t kMaxDeviceLostRecoveries = 2;

} // namespace

// ============================================================================
// Test Suite
// ============================================================================

struct LightingRTTestSuite {
    LightingRTTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~LightingRTTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 512, .maxBodyPairs = 1024, .maxContactConstraints = 1024, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Lighting RT Test",
                .width          = width,
                .height         = height,
                .vsync          = false,
                .fullscreen     = false,
                .validationMode = ZHLN::ValidationMode::On,
                .headless       = true
            }
        };

        auto engineRes = ZHLN::Engine::Create(cfg);
        if (!engineRes) {
            return nullptr;
        }

        auto engine = std::move(engineRes.value());
        engine->InitializeDefaultScene();
        return engine;
    }

    static void DisableTAA(ZHLN::Engine& engine) {
        auto& reg = engine.GetRegistry();
        for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::AASettingsComponent>()) {
            reg.Patch<ZHLN::Components::AASettingsComponent>(e, [](auto& aa) {
                aa.state.mode        = ZHLN::AAMode::None;
                aa.state.jitterX     = 0.0f;
                aa.state.jitterY     = 0.0f;
                aa.state.prevJitterX = 0.0f;
                aa.state.prevJitterY = 0.0f;
                aa.state.frameIndex  = 0;
            });
        }
        engine.GetRenderContext().SetAAState(ZHLN::AAState {.mode = ZHLN::AAMode::None});
    }

    static void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt = 1.0f / 60.0f) {
        for (uint32_t i = 0; i < frames; ++i) {
            engine.ProcessEvents();
            const auto status = engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
            ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
        }
    }

    static auto Capture(ZHLN::Engine& engine, const std::string& path) -> RgbImage {
        if (!engine.GetRenderContext().CaptureScreenshotPPM(path)) {
            return {};
        }
        const RgbImage img = LoadPPM(path);
        if (img.Valid()) {
            (void) SavePNG(PngPathOf(path), img);
        }
        return img;
    }

    template <typename SceneFn>
    [[nodiscard]] static StableRunResult
        RunStableScene(ZHLN::Engine& engine, uint32_t warmupFrames, const char* label, SceneFn&& sceneFn, uint32_t* outValidationDelta = nullptr) {
        auto&        ctx         = ZHLN::Test::GetThreadLocalContext();
        const size_t failureMark = ctx.failures.size();

        for (uint32_t attempt = 0; attempt <= kMaxDeviceLostRecoveries; ++attempt) {
            if (attempt > 0) {
                ctx.failures.resize(failureMark);
                ZHLN::Println(
                    "    [WARN] {}: Vulkan device lost; engine hot-rebuilt. Re-warming and retrying (attempt {}/{}).", label, attempt, kMaxDeviceLostRecoveries
                );
            }

            const uint32_t validationBefore = ZHLN::RenderContext::ValidationErrorCount();

            ZHLN::RenderContext* const preWarmup = &engine.GetRenderContext();
            TickFrames(engine, warmupFrames);
            if (&engine.GetRenderContext() != preWarmup) {
                continue;
            }

            ZHLN::RenderContext* const preWork = &engine.GetRenderContext();
            const bool                 ok      = sceneFn(engine);
            if (ok && &engine.GetRenderContext() == preWork) {
                if (outValidationDelta != nullptr) {
                    *outValidationDelta = ZHLN::RenderContext::ValidationErrorCount() - validationBefore;
                }
                return StableRunResult::Ok;
            }
            if (&engine.GetRenderContext() == preWork) {
                return StableRunResult::AssertionsFailed;
            }
        }

        return StableRunResult::PersistentDeviceLost;
    }

    struct Tests {
        // ====================================================================
        // 1. Lit Scene Static Frame Stability
        // ====================================================================
        std::expected<void, ZHLN::Error> lit_scene_static_frame_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 10.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.55f, 0.55f, 0.58f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false}
                );

                auto grayMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.65f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.7f, .baseColor = {0.9f, 0.1f, 0.1f, 1.0f}}
                );
                auto blueMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.1f, 0.2f, 0.9f, 1.0f}}
                );

                auto checkMaterials = ZHLN::Test::AssertTrue(grayMatRes && redMatRes && blueMatRes);
                if (!checkMaterials) {
                    return checkMaterials;
                }

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-2.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *grayMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, -2.0), .createPhysics = false, .materialOverride = *redMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(2.2, 1.0, 1.0), .createPhysics = false, .materialOverride = *blueMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 220.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                auto addPointLight = [&](const JPH::Vec3& pos, const JPH::Vec3& color, float intensity) {
                    const ZHLN::Entity e = reg.Create();
                    reg.Add(
                        e, ZHLN::Components::TransformComponent {.position = pos},
                        ZHLN::Components::LightComponent {.type = ZHLN::LightType::Point, .color = color, .intensity = intensity, .range = 30.0f}
                    );
                };
                addPointLight(JPH::Vec3(-4.0f, 3.0f, 2.0f), JPH::Vec3(1.0f, 0.55f, 0.3f), 800.0f);
                addPointLight(JPH::Vec3(4.0f, 3.0f, -3.0f), JPH::Vec3(0.3f, 0.5f, 1.0f), 800.0f);

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.5f, 8.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -12.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 14, "lit_scene_static_frame_stability",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage  repeatA    = Capture(eng, "headless_lighting_rt_static_r0.ppm");
                    const RgbImage  repeatB    = Capture(eng, "headless_lighting_rt_static_r1.ppm");
                    const FrameDiff repeatDiff = CompareFrames(repeatA, repeatB);

                    std::vector<double>    litSeries;
                    std::vector<double>    lumaSeries;
                    std::vector<double>    redSeries;
                    std::vector<double>    satSeries;
                    std::vector<FrameDiff> temporalDiffs;

                    RgbImage prev;
                    for (uint32_t f = 0; f < 4; ++f) {
                        TickFrames(eng, 1);
                        const RgbImage frame = Capture(eng, "headless_lighting_rt_static_f" + std::to_string(f) + ".ppm");

                        auto checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame);
                        litSeries.push_back(static_cast<double>(m.lit));
                        lumaSeries.push_back(m.meanLuma);
                        redSeries.push_back(static_cast<double>(m.red));
                        satSeries.push_back(static_cast<double>(m.saturated));

                        if (prev.Valid()) {
                            temporalDiffs.push_back(CompareFrames(prev, frame));
                        }
                        prev = frame;
                    }

                    ZHLN::Test::ExpectFalse(eng.GetVisibleEntities().empty());
                    ZHLN::Test::ExpectTrue(ZHLN::CullingStats::TotalTriangles > 0);

                    const bool frameProduced   = ZHLN::Test::ExpectTrue(Mean(lumaSeries) > 1.0);
                    const bool geometryVisible = ZHLN::Test::ExpectTrue(Mean(litSeries) > 500.0);
                    if (!frameProduced || !geometryVisible) {
                        return false;
                    }

                    const double litCV   = CoefficientOfVariation(litSeries);
                    const double lumaCV  = CoefficientOfVariation(lumaSeries);
                    const double redCV   = CoefficientOfVariation(redSeries);
                    const double satCV   = CoefficientOfVariation(satSeries);
                    const double litMean = Mean(litSeries);

                    double worstFrac32 = 0.0;
                    double maxJump     = 0.0;
                    for (size_t i = 0; i < temporalDiffs.size(); ++i) {
                        worstFrac32 = std::max(worstFrac32, temporalDiffs[i].frac32);
                        if (i + 1 < litSeries.size() && litMean > 1.0) {
                            const double jump = std::abs(litSeries[i + 1] - litSeries[i]) / litMean;
                            maxJump           = std::max(maxJump, jump);
                        }
                    }

                    const double meanSaturated = Mean(satSeries);
                    const bool   stableLit     = ZHLN::Test::ExpectTrue(litCV < 0.03);
                    const bool   stableLuma    = ZHLN::Test::ExpectTrue(lumaCV < 0.01);
                    const bool   stableRed     = ZHLN::Test::ExpectTrue(redCV < 0.05);
                    const bool   stableSat     = ZHLN::Test::ExpectTrue(satCV < 0.25 || meanSaturated < 100.0);
                    const bool   noPixelPop    = ZHLN::Test::ExpectTrue(worstFrac32 < 0.01);
                    const bool   noJump        = ZHLN::Test::ExpectTrue(maxJump < 0.08);
                    const bool   noBlowout     = ZHLN::Test::ExpectTrue(meanSaturated < 0.02 * static_cast<double>(640 * 480));

                    return stableLit && stableLuma && stableRed && stableSat && noPixelPop && noJump && noBlowout;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 2. Point Light Cluster Culling Sweep
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_cluster_culling_sweep() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            ZHLN::Entity redLight = ZHLN::Entity::Null();
            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );

                redLight = reg.Create();
                reg.Add(
                    redLight, ZHLN::Components::TransformComponent {.position = JPH::Vec3(-6.4f, 2.4f, 5.5f)},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "point_light_cluster_culling_sweep",
                [&](ZHLN::Engine& eng) -> bool {
                    auto& reg = eng.GetRegistry();
                    ZHLN::Test::ExpectTrue(reg.IsAlive(redLight));

                    std::vector<double>   redCounts;
                    std::vector<uint32_t> redPeaks;
                    uint32_t              isolatedDips = 0;

                    constexpr int   kSteps = 21;
                    constexpr float kStepX = 0.64f;
                    for (int step = 0; step < kSteps; ++step) {
                        const float x = -6.4f + static_cast<float>(step) * kStepX;
                        const float z = 5.5f - 0.078125f * x;

                        reg.Patch<ZHLN::Components::TransformComponent>(redLight, [&](auto& t) { t.position = JPH::Vec3(x, 2.4f, z); });

                        TickFrames(eng, 2);

                        const RgbImage frame      = Capture(eng, "headless_lighting_rt_cull_" + std::to_string(step) + ".ppm");
                        auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame);
                        redCounts.push_back(static_cast<double>(m.red));
                        redPeaks.push_back(m.redPeak);
                    }

                    for (size_t i = 1; i + 1 < redCounts.size(); ++i) {
                        const double left  = redCounts[i - 1];
                        const double right = redCounts[i + 1];
                        const double base  = std::min(left, right);
                        if (base > 64.0 && redCounts[i] < 0.5 * base) {
                            ++isolatedDips;
                        }
                    }

                    TickFrames(eng, 1);
                    constexpr uint32_t                  kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> stableFrames {};
                    std::vector<double>                 stableCounts;
                    std::vector<double>                 stableLuma;
                    std::vector<double>                 stableLit;
                    std::vector<uint64_t>               stableFrameIdx;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                        stableFrameIdx.push_back(eng.GetCurrentFrame());
                        stableFrames[r]  = Capture(eng, "headless_lighting_rt_cull_stable_" + std::to_string(r) + ".ppm");
                        auto checkStable = ZHLN::Test::AssertTrue(stableFrames[r].Valid());
                        if (!checkStable) {
                            return false;
                        }
                        const FrameMetrics m = MeasureImage(stableFrames[r]);
                        stableCounts.push_back(static_cast<double>(m.red));
                        stableLuma.push_back(m.meanLuma);
                        stableLit.push_back(static_cast<double>(m.lit));

                        TickFrames(eng, 1);
                    }

                    double    worstPairFrac = 0.0;
                    size_t    worstPair     = 0;
                    FrameDiff worstDiffs[kStableFrames - 1] {};
                    for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                        worstDiffs[i] = CompareFrames(stableFrames[i], stableFrames[i + 1]);
                        if (worstDiffs[i].frac32 > worstPairFrac) {
                            worstPairFrac = worstDiffs[i].frac32;
                            worstPair     = i;
                        }
                    }
                    const ChangedRegion region = DiffRegion(stableFrames[worstPair], stableFrames[worstPair + 1]);
                    WriteAmplifiedDiff("headless_lighting_rt_cull_parity_diff.ppm", stableFrames[worstPair], stableFrames[worstPair + 1]);
                    WriteRegionCrop("headless_lighting_rt_cull_region_a.ppm", stableFrames[worstPair], region);
                    WriteRegionCrop("headless_lighting_rt_cull_region_b.ppm", stableFrames[worstPair + 1], region);

                    const double   minRed  = *std::ranges::min_element(redCounts);
                    const uint32_t minPeak = *std::ranges::min_element(redPeaks);

                    const bool neverCulled      = ZHLN::Test::ExpectTrue(minRed > 16.0);
                    const bool brightEverywhere = ZHLN::Test::ExpectTrue(minPeak > 60u);
                    const bool noIsolatedCull   = ZHLN::Test::ExpectTrue(isolatedDips == 0u);
                    const bool noStaticStep     = ZHLN::Test::ExpectTrue(worstPairFrac < 0.005);

                    return neverCulled && brightEverywhere && noIsolatedCull && noStaticStep;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 3. Point Light Static Reference (No History)
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_static_reference_no_history() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            const JPH::Vec3 lightPos(6.4f, 2.4f, 5.0f);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );
                const ZHLN::Entity redLight = reg.Create();
                reg.Add(
                    redLight, ZHLN::Components::TransformComponent {.position = lightPos},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );
                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "point_light_static_reference_no_history",
                [&](ZHLN::Engine& eng) -> bool {
                    constexpr uint32_t                  kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> frames {};
                    std::vector<double>                 counts;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                        frames[r]       = Capture(eng, "headless_lighting_rt_ref_stable_" + std::to_string(r) + ".ppm");
                        auto checkFrame = ZHLN::Test::AssertTrue(frames[r].Valid());
                        if (!checkFrame) {
                            return false;
                        }
                        counts.push_back(static_cast<double>(MeasureImage(frames[r]).red));

                        TickFrames(eng, 1);
                    }

                    double worstFrac = 0.0;
                    size_t worstPair = 0;
                    for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                        const double f = CompareFrames(frames[i], frames[i + 1]).frac32;
                        if (f > worstFrac) {
                            worstFrac = f;
                            worstPair = i;
                        }
                    }

                    const ChangedRegion region = DiffRegion(frames[worstPair], frames[worstPair + 1]);
                    WriteAmplifiedDiff("headless_lighting_rt_ref_parity_diff.ppm", frames[worstPair], frames[worstPair + 1]);
                    WriteRegionCrop("headless_lighting_rt_ref_region_a.ppm", frames[worstPair], region);
                    WriteRegionCrop("headless_lighting_rt_ref_region_b.ppm", frames[worstPair + 1], region);

                    const bool noStaticStep = ZHLN::Test::ExpectTrue(worstFrac < 0.005);
                    const bool lightVisible = ZHLN::Test::ExpectTrue(Mean(counts) > 16.0);

                    return noStaticStep && lightVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 4. Ray-Traced Shadow Occlusion & Stability
        // ====================================================================
        std::expected<void, ZHLN::Error> raytraced_shadow_occlusion_and_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] No raytracing support on this device; nothing to verify for RT shadows.");
                return {};
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 1.5f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 1;
                    });
                }

                auto floorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.55f, 0.55f, 0.55f, 1.0f}}
                );
                auto checkFloorMat = ZHLN::Test::AssertTrue(floorMatRes.has_value());
                if (!checkFloorMat) {
                    return checkFloorMat;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 400.0f, {0.55f, 0.55f, 0.55f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(60.0f, 45.0f, 0.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({0.0f, 90.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 240.0f,
                        .direction = JPH::Vec3(0.8f, 0.6f, 0.0f).Normalized()
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(-10.0f, 6.0f, -8.0f);
                cam.yaw      = 0.0f;
                cam.pitch    = -20.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "raytraced_shadow_occlusion_and_stability",
                [](ZHLN::Engine& eng) -> bool {
                    auto& reg = eng.GetRegistry();

                    const ZHLN::Entity occluder = ZHLN::CreativeWorksFactory::CreateBox(
                        eng, JPH::Vec3(0.5f, 3.0f, 4.0f),
                        ZHLN::CreativeWorksFactory::SpawnParams {
                            .position = JPH::RVec3(0.0, 3.0, -8.0), .createPhysics = false, .color = {0.7f, 0.7f, 0.7f, 1.0f}
                        }
                    );

                    TickFrames(eng, 2);

                    const RgbImage shadowA       = Capture(eng, "headless_lighting_rt_shadow_a.ppm");
                    const RgbImage shadowARepeat = Capture(eng, "headless_lighting_rt_shadow_a_repeat.ppm");
                    TickFrames(eng, 1);
                    const RgbImage shadowB = Capture(eng, "headless_lighting_rt_shadow_b.ppm");

                    auto checkFrame = ZHLN::Test::AssertTrue(shadowA.Valid() && shadowARepeat.Valid() && shadowB.Valid());
                    if (!checkFrame) {
                        reg.Destroy(occluder);
                        return false;
                    }

                    reg.Destroy(occluder);
                    ZHLN::Test::ExpectFalse(reg.IsAlive(occluder));

                    TickFrames(eng, 2);
                    const RgbImage shadowClear = Capture(eng, "headless_lighting_rt_shadow_clear.ppm");
                    checkFrame                 = ZHLN::Test::AssertTrue(shadowClear.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    constexpr double   kFloorRowFraction = 0.72;
                    const FrameMetrics mA                = MeasureImage(shadowA, kFloorRowFraction);
                    const FrameMetrics mA2               = MeasureImage(shadowARepeat, kFloorRowFraction);
                    const FrameMetrics mB                = MeasureImage(shadowB, kFloorRowFraction);
                    const FrameMetrics mClear            = MeasureImage(shadowClear, kFloorRowFraction);

                    const uint32_t darkA     = mA.dark;
                    const uint32_t darkA2    = mA2.dark;
                    const uint32_t darkB     = mB.dark;
                    const uint32_t darkClear = mClear.dark;
                    const uint32_t litA      = mA.lit;
                    const uint32_t litClear  = mClear.lit;

                    const FrameDiff repeatDiff   = CompareFrames(shadowA, shadowARepeat);
                    const FrameDiff temporalDiff = CompareFrames(shadowA, shadowB);

                    const double darkJump = (darkA > 0) ? static_cast<double>(std::abs(static_cast<int64_t>(darkB) - static_cast<int64_t>(darkA))) /
                                                              static_cast<double>(darkA) :
                                                          0.0;

                    const bool shadowExist     = ZHLN::Test::ExpectTrue(darkA > darkClear + 1500u);
                    const bool lightRestored   = ZHLN::Test::ExpectTrue(darkClear < darkA / 3u);
                    const bool meanBrightens   = ZHLN::Test::ExpectTrue(mClear.meanLuma > mA.meanLuma * 1.25 + 1.0);
                    const bool notBlackout     = ZHLN::Test::ExpectTrue(darkA < 0.85 * static_cast<double>(mA.total));
                    const bool shadowStable    = ZHLN::Test::ExpectTrue(darkJump < 0.15);
                    const bool noShadowFlicker = ZHLN::Test::ExpectTrue(temporalDiff.frac32 < 0.015);
                    const bool repeatClean     = ZHLN::Test::ExpectTrue(repeatDiff.frac32 == 0.0);

                    return shadowExist && lightRestored && meanBrightens && notBlackout && shadowStable && noShadowFlicker && repeatClean;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::RayTracedShadowFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 5. Ray-Traced Reflection Coverage & Artifacts
        // ====================================================================
        std::expected<void, ZHLN::Error> raytraced_reflection_coverage_and_artifacts() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                auto mirrorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.03f, .baseColor = {0.85f, 0.85f, 0.88f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMatRes.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *mirrorMatRes}
                );

                auto emissiveMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.55f, .baseColor = {1.0f, 0.06f, 0.04f, 1.0f}, .emissive = {1.0f, 0.0f, 0.0f, 1.0f}
                        }
                );
                auto checkEmissive = ZHLN::Test::AssertTrue(emissiveMatRes.has_value());
                if (!checkEmissive) {
                    return checkEmissive;
                }
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 3.0, 0.0), .createPhysics = false, .materialOverride = *emissiveMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 140.0f,
                        .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 10, "raytraced_reflection_coverage_and_artifacts",
                [](ZHLN::Engine& eng) -> bool {
                    std::vector<double> reflectionSeries;
                    std::vector<double> saturationSeries;
                    std::vector<double> isolatedSeries;
                    for (uint32_t f = 0; f < 4; ++f) {
                        TickFrames(eng, 1);
                        const RgbImage frame      = Capture(eng, "headless_lighting_rt_reflect_f" + std::to_string(f) + ".ppm");
                        auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame, 0.5);
                        reflectionSeries.push_back(static_cast<double>(m.red));
                        saturationSeries.push_back(static_cast<double>(m.saturated));
                        isolatedSeries.push_back(m.red > 0 ? static_cast<double>(m.redIsolated) / static_cast<double>(m.red) : 0.0);
                    }

                    const double   meanReflection  = Mean(reflectionSeries);
                    const double   reflectionCV    = CoefficientOfVariation(reflectionSeries);
                    const double   saturationCV    = CoefficientOfVariation(saturationSeries);
                    const double   meanSaturation  = Mean(saturationSeries);
                    const double   isolatedRatio   = Mean(isolatedSeries);
                    const uint32_t lowerHalfPixels = static_cast<uint32_t>(640 * 480 / 2);

                    const bool reflectionPresent = ZHLN::Test::ExpectTrue(meanReflection > 24.0);
                    const bool reflectionStable  = ZHLN::Test::ExpectTrue(reflectionCV < 0.15);
                    const bool noBlowout         = ZHLN::Test::ExpectTrue(meanSaturation < 0.04 * static_cast<double>(lowerHalfPixels));
                    const bool noRayDebris       = ZHLN::Test::ExpectTrue(isolatedRatio < 0.35);
                    const bool saturationStable  = ZHLN::Test::ExpectTrue(saturationCV < 0.25 || meanSaturation < 100.0);

                    return reflectionPresent && reflectionStable && noBlowout && noRayDebris && saturationStable;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::ReflectionArtifacts);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 6. Multi-Light Clustered Accumulation & Chromatic Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> multi_light_cluster_accumulation_and_chromatic_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 1.0f;
                        pp.enableSSR       = 0;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                // Explicit 0-intensity Sun to suppress the default 180-nit white sun injection
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 0.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                // Neutral diffuse gray floor
                auto neutralMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto checkNeutral = ZHLN::Test::AssertTrue(neutralMat.has_value());
                if (!checkNeutral) {
                    return checkNeutral;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 80.0f, {0.8f, 0.8f, 0.8f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *neutralMat}
                );

                // Central pedestal at quadrant boundary to test additive color mixing
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(1.0f, 0.6f, 1.0f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.6, 0.0), .createPhysics = false, .materialOverride = *neutralMat}
                );

                // 64 Point Lights partitioned into 4 chromatic quadrants (perspective-aligned for yaw=+90 look along +Z):
                //   World X > 0, Z > 0 -> Screen Top-Left:     Pure Red   (1.0, 0.02, 0.02)
                //   World X <= 0, Z > 0 -> Screen Top-Right:   Pure Green (0.02, 1.0, 0.02)
                //   World X > 0, Z < 0 -> Screen Bottom-Left:  Pure Blue  (0.02, 0.02, 1.0)
                //   World X <= 0, Z < 0 -> Screen Bottom-Right: Amber     (1.0, 0.85, 0.15)
                constexpr int kGridDim = 8;
                for (int gx = 0; gx < kGridDim; ++gx) {
                    for (int gz = 0; gz < kGridDim; ++gz) {
                        float posX = -14.0f + static_cast<float>(gx) * 4.0f;
                        float posZ = -14.0f + static_cast<float>(gz) * 4.0f;
                        float posY = 2.0f;

                        JPH::Vec3 lightColor(1.0f, 1.0f, 1.0f);
                        if (posX > 0.0f && posZ >= 0.0f) {
                            lightColor = JPH::Vec3(1.0f, 0.02f, 0.02f); // Red -> Screen Top-Left
                        } else if (posX <= 0.0f && posZ >= 0.0f) {
                            lightColor = JPH::Vec3(0.02f, 1.0f, 0.02f); // Green -> Screen Top-Right
                        } else if (posX > 0.0f && posZ < 0.0f) {
                            lightColor = JPH::Vec3(0.02f, 0.02f, 1.0f); // Blue -> Screen Bottom-Left
                        } else {
                            lightColor = JPH::Vec3(1.0f, 0.85f, 0.15f); // Amber -> Screen Bottom-Right
                        }

                        const ZHLN::Entity lt = reg.Create();
                        reg.Add(
                            lt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(posX, posY, posZ)},
                            ZHLN::Components::LightComponent {
                                .type      = ZHLN::LightType::Point,
                                .color     = lightColor,
                                .intensity = 250.0f,
                                .range     = 14.0f,
                            }
                        );
                    }
                }

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 22.0f, -24.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -42.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "multi_light_cluster_accumulation_and_chromatic_interaction",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage frame      = Capture(eng, "headless_lighting_multi_cluster.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    // Normalized Quadrant Sampling:
                    const auto quadTL    = MeasureSubRegion(frame, {.x0 = 0.05, .y0 = 0.05, .x1 = 0.40, .y1 = 0.45});
                    const auto quadTR    = MeasureSubRegion(frame, {.x0 = 0.60, .y0 = 0.05, .x1 = 0.95, .y1 = 0.45});
                    const auto quadBL    = MeasureSubRegion(frame, {.x0 = 0.05, .y0 = 0.55, .x1 = 0.40, .y1 = 0.95});
                    const auto centerMix = MeasureSubRegion(frame, {.x0 = 0.45, .y0 = 0.35, .x1 = 0.55, .y1 = 0.65});

                    ZHLN::Println("    [INFO] Multi-light 64-Light Clustered Grid:");
                    ZHLN::Println(
                        "      Top-Left Quad (Red):    MeanRGB=({:.1f},{:.1f},{:.1f}), DominantRed={}/{}", quadTL.meanR, quadTL.meanG, quadTL.meanB,
                        quadTL.dominantRed, quadTL.pixels
                    );
                    ZHLN::Println(
                        "      Top-Right Quad (Green): MeanRGB=({:.1f},{:.1f},{:.1f}), DominantGreen={}/{}", quadTR.meanR, quadTR.meanG, quadTR.meanB,
                        quadTR.dominantGrn, quadTR.pixels
                    );
                    ZHLN::Println(
                        "      Bottom-Left Quad (Blue):MeanRGB=({:.1f},{:.1f},{:.1f}), DominantBlue={}/{}", quadBL.meanR, quadBL.meanG, quadBL.meanB,
                        quadBL.dominantBlu, quadBL.pixels
                    );
                    ZHLN::Println(
                        "      Center Mixing (R+G->Y): MeanRGB=({:.1f},{:.1f},{:.1f}), YellowMixPixels={}/{}", centerMix.meanR, centerMix.meanG,
                        centerMix.meanB, centerMix.yellowMix, centerMix.pixels
                    );

                    // Every gate below is a ratio -- channel against channel, or
                    // saturated pixels as a share of their region -- so the scene can be
                    // re-exposed without the assertions moving. Absolute means are
                    // exposure/tone-map outputs, not lighting behaviour.

                    // 1. Quadrant Chromatic Purity
                    const bool redDominant = CheckCondition(
                        quadTL.meanR > 1.3 * quadTL.meanG && quadTL.meanR > 1.3 * quadTL.meanB && quadTL.dominantRed * 100 > quadTL.pixels,
                        "top-left quadrant is red-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantRed={}/{} px (need >1% of the quad)", quadTL.meanR, quadTL.meanG, quadTL.meanB,
                        quadTL.dominantRed, quadTL.pixels
                    );
                    const bool greenDominant = CheckCondition(
                        quadTR.meanG > 1.3 * quadTR.meanR && quadTR.meanG > 1.3 * quadTR.meanB && quadTR.dominantGrn * 100 > quadTR.pixels,
                        "top-right quadrant is green-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantGreen={}/{} px (need >1% of the quad)", quadTR.meanR, quadTR.meanG, quadTR.meanB,
                        quadTR.dominantGrn, quadTR.pixels
                    );
                    const bool blueDominant = CheckCondition(
                        quadBL.meanB > 1.3 * quadBL.meanR && quadBL.meanB > 1.3 * quadBL.meanG && quadBL.dominantBlu * 100 > quadBL.pixels,
                        "bottom-left quadrant is blue-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantBlue={}/{} px (need >1% of the quad)", quadBL.meanR, quadBL.meanG, quadBL.meanB,
                        quadBL.dominantBlu, quadBL.pixels
                    );

                    // 2. Additive Color Superposition at boundary (Red + Green -> Yellow).
                    // The pedestal sits under all four quadrants, so it must out-shine
                    // each pure quadrant in its own channel -- lights accumulating rather
                    // than the nearest one winning.
                    const bool additiveMixing = CheckCondition(
                        centerMix.meanR > 1.3 * centerMix.meanB && centerMix.meanG > 1.3 * centerMix.meanB && centerMix.meanR > 0.6 * centerMix.meanG &&
                            centerMix.meanG > 0.6 * centerMix.meanR && centerMix.meanR > 0.5 * quadTL.meanR && centerMix.meanG > 0.5 * quadTR.meanG &&
                            centerMix.yellowMix * 100 > centerMix.pixels,
                        "quadrant boundary mixes red + green into yellow",
                        "centerMeanRGB=({:.1f},{:.1f},{:.1f}) vs redQuad.meanR={:.1f} greenQuad.meanG={:.1f}, yellowMix={}/{} px (need >1%)", centerMix.meanR,
                        centerMix.meanG, centerMix.meanB, quadTL.meanR, quadTR.meanG, centerMix.yellowMix, centerMix.pixels
                    );

                    // 3. Coverage & headroom. "lit" counts Luma > 40, which a pure blue
                    // pixel can never reach (0.0722 * 255 = 18.4), so in a scene that is
                    // a quarter blue by construction that metric grades the palette
                    // instead of the lighting. Count saturated chroma instead: it is
                    // hue-aware and, as a share of the sampled area, exposure-relative.
                    const uint32_t chromaticPixels = quadTL.dominantRed + quadTR.dominantGrn + quadBL.dominantBlu + centerMix.yellowMix;
                    const uint32_t sampledPixels   = quadTL.pixels + quadTR.pixels + quadBL.pixels + centerMix.pixels;
                    const bool     lightCovered    = CheckCondition(
                        chromaticPixels * 10 > sampledPixels, "clustered lights cover a meaningful share of the frame",
                        "chromaticPixels={}/{} sampled px (need >10%)", chromaticPixels, sampledPixels
                    );

                    const FrameMetrics fullFrame         = MeasureImage(frame);
                    const bool         noBlackout        = CheckCondition(
                        fullFrame.meanLuma > 1.0, "frame is not blacked out", "meanLuma={:.2f} over {} px", fullFrame.meanLuma, fullFrame.total
                    );
                    const bool         noExtremeOverflow = CheckCondition(
                        fullFrame.saturated * 20 < fullFrame.total, "frame is not blown out", "saturated={}/{} px (need <5%)", fullFrame.saturated,
                        fullFrame.total
                    );

                    return redDominant && greenDominant && blueDominant && additiveMixing && lightCovered && noBlackout && noExtremeOverflow;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::MultiLightClusteringFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] 64 Clustered lights correctly accumulated with clean chromatic superposition.");
            return {};
        }

        // ====================================================================
        // 7. Multi-Emissive Sources & Surface Reflection Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> multi_emissive_sources_and_surface_reflection_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                // Sun illumination to support scene depth and HDR tone mapping
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 100.0f,
                        .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
                    }
                );

                // Polished metallic mirror floor
                auto mirrorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.02f, .baseColor = {0.9f, 0.9f, 0.95f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMat.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.9f, 0.9f, 0.95f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *mirrorMat}
                );

                // 4 Distinct High-Luminance Emissive Geometric Emitters at Y = 3.0, Z = 0.0:
                //   Emitter 1: X = -4.5 (Pure Red)
                //   Emitter 2: X = -1.5 (Pure Green)
                //   Emitter 3: X = +1.5 (Pure Blue)
                //   Emitter 4: X = +4.5 (Golden Yellow)
                auto matEmissiveRed = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}, .emissive = {6.0f, 0.0f, 0.0f, 1.0f}
                        }
                );
                auto matEmissiveGrn = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 1.0f, 0.05f, 1.0f}, .emissive = {0.0f, 6.0f, 0.0f, 1.0f}
                        }
                );
                auto matEmissiveBlu = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 0.05f, 1.0f, 1.0f}, .emissive = {0.0f, 0.0f, 6.0f, 1.0f}
                        }
                );
                auto matEmissiveYel = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.9f, 0.05f, 1.0f}, .emissive = {5.0f, 4.5f, 0.0f, 1.0f}
                        }
                );

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(-4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveRed
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(-1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveGrn
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveBlu}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveYel}
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "multi_emissive_sources_and_surface_reflection_interaction",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage frame      = Capture(eng, "headless_lighting_multi_emissive.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    // Slicing lower reflection half into 4 horizontal column bands centered on planar reflection centroids:
                    //   Strip 1 (Red Refl Centroid   ~0.30): X in [0.20, 0.36], Y in [0.52, 0.95]
                    //   Strip 2 (Green Refl Centroid ~0.43): X in [0.38, 0.48], Y in [0.52, 0.95]
                    //   Strip 3 (Blue Refl Centroid  ~0.57): X in [0.52, 0.62], Y in [0.52, 0.95]
                    //   Strip 4 (Yellow Refl Centroid~0.70): X in [0.64, 0.80], Y in [0.52, 0.95]
                    const auto reflStripRed = MeasureSubRegion(frame, {.x0 = 0.20, .y0 = 0.52, .x1 = 0.36, .y1 = 0.95});
                    const auto reflStripGrn = MeasureSubRegion(frame, {.x0 = 0.38, .y0 = 0.52, .x1 = 0.48, .y1 = 0.95});
                    const auto reflStripBlu = MeasureSubRegion(frame, {.x0 = 0.52, .y0 = 0.52, .x1 = 0.62, .y1 = 0.95});
                    const auto reflStripYel = MeasureSubRegion(frame, {.x0 = 0.64, .y0 = 0.52, .x1 = 0.80, .y1 = 0.95});

                    ZHLN::Println("    [INFO] Multi-Emissive Planar Mirror Reflection Slices:");
                    ZHLN::Println(
                        "      Strip 1 (Refl Red):    MeanRGB=({:.1f},{:.1f},{:.1f}), DominantRed={}", reflStripRed.meanR, reflStripRed.meanG,
                        reflStripRed.meanB, reflStripRed.dominantRed
                    );
                    ZHLN::Println(
                        "      Strip 2 (Refl Green):  MeanRGB=({:.1f},{:.1f},{:.1f}), DominantGreen={}", reflStripGrn.meanR, reflStripGrn.meanG,
                        reflStripGrn.meanB, reflStripGrn.dominantGrn
                    );
                    ZHLN::Println(
                        "      Strip 3 (Refl Blue):   MeanRGB=({:.1f},{:.1f},{:.1f}), DominantBlue={}", reflStripBlu.meanR, reflStripBlu.meanG,
                        reflStripBlu.meanB, reflStripBlu.dominantBlu
                    );
                    ZHLN::Println(
                        "      Strip 4 (Refl Yellow): MeanRGB=({:.1f},{:.1f},{:.1f}), YellowMixPixels={}", reflStripYel.meanR, reflStripYel.meanG,
                        reflStripYel.meanB, reflStripYel.yellowMix
                    );

                    // 1. Spatial mirror correspondence. Each gate is a ratio: channel
                    // against channel inside the strip, or bright pixels as a share of
                    // the strip. Absolute means are exposure outputs -- the same correct
                    // reflection measures 5.9 or 59.0 depending on ambientExposure -- and
                    // the strips are not even the same width, so a shared absolute floor
                    // grades geometry rather than the reflection.

                    const bool reflRedOk = CheckCondition(
                        reflStripRed.dominantRed * 200 > reflStripRed.pixels && reflStripRed.meanR > 1.3 * reflStripRed.meanG &&
                            reflStripRed.meanR > 1.3 * reflStripRed.meanB,
                        "strip 1 mirrors the red emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantRed={}/{} px (need >0.5% of the strip)", reflStripRed.meanR, reflStripRed.meanG,
                        reflStripRed.meanB, reflStripRed.dominantRed, reflStripRed.pixels
                    );
                    const bool reflGrnOk = CheckCondition(
                        reflStripGrn.dominantGrn * 200 > reflStripGrn.pixels && reflStripGrn.meanG > 1.3 * reflStripGrn.meanR &&
                            reflStripGrn.meanG > 1.3 * reflStripGrn.meanB,
                        "strip 2 mirrors the green emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantGreen={}/{} px (need >0.5% of the strip)", reflStripGrn.meanR, reflStripGrn.meanG,
                        reflStripGrn.meanB, reflStripGrn.dominantGrn, reflStripGrn.pixels
                    );
                    const bool reflBluOk = CheckCondition(
                        reflStripBlu.dominantBlu * 200 > reflStripBlu.pixels && reflStripBlu.meanB > 1.3 * reflStripBlu.meanR &&
                            reflStripBlu.meanB > 1.3 * reflStripBlu.meanG,
                        "strip 3 mirrors the blue emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantBlue={}/{} px (need >0.5% of the strip)", reflStripBlu.meanR, reflStripBlu.meanG,
                        reflStripBlu.meanB, reflStripBlu.dominantBlu, reflStripBlu.pixels
                    );
                    // Yellow has no single dominant channel to lean on, so its signature is
                    // the R+G mix count plus R and G clearing B by the same 1.3x the pure
                    // strips use and staying within 0.6x of each other (yellow, not amber).
                    const bool reflYelOk = CheckCondition(
                        reflStripYel.yellowMix * 200 > reflStripYel.pixels && reflStripYel.meanR > 1.3 * reflStripYel.meanB &&
                            reflStripYel.meanG > 1.3 * reflStripYel.meanB && reflStripYel.meanR > 0.6 * reflStripYel.meanG &&
                            reflStripYel.meanG > 0.6 * reflStripYel.meanR,
                        "strip 4 mirrors the yellow emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), yellowMix={}/{} px (need >0.5% of the strip)", reflStripYel.meanR, reflStripYel.meanG,
                        reflStripYel.meanB, reflStripYel.yellowMix, reflStripYel.pixels
                    );

                    // 2. Validate Upper Direct Emission visibility. The peak-luma guard
                    // stays absolute: it only asserts the emitters are directly visible
                    // somewhere in the upper frame, not that the scene is bright.
                    const auto     upperDirect      = MeasureSubRegion(frame, {.x0 = 0.0, .y0 = 0.05, .x1 = 1.0, .y1 = 0.45});
                    const uint32_t directChroma     = upperDirect.dominantRed + upperDirect.dominantGrn + upperDirect.dominantBlu + upperDirect.yellowMix;
                    const bool     directVisible    = CheckCondition(
                        upperDirect.maxLuma > 60.0 && directChroma * 1000 > upperDirect.pixels, "emitters are directly visible in the upper frame",
                        "maxLuma={:.1f} (need >60), chromaticPixels={}/{} px (need >0.1%)", upperDirect.maxLuma, directChroma, upperDirect.pixels
                    );

                    return reflRedOk && reflGrnOk && reflBluOk && reflYelOk && directVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::MultiEmissiveReflectionFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] Multi-emissive objects correctly mapped to their respective mirror reflections.");
            return {};
        }

        // ====================================================================
        // 8. Dense Multi-Light & Emissive Materials Cross-Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> dense_multi_light_emissive_materials_cross_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto floorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.5f, .roughness = 0.25f, .baseColor = {0.6f, 0.6f, 0.65f, 1.0f}}
                );
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 80.0f, {0.6f, 0.6f, 0.65f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *floorMat}
                );

                auto monolithMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.2f, .baseColor = {0.0f, 1.0f, 1.0f, 1.0f}, .emissive = {0.0f, 20.0f, 20.0f, 1.0f}
                        }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 2.5f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 2.5, 0.0), .createPhysics = false, .materialOverride = *monolithMat}
                );

                auto goldMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.15f, .baseColor = {1.0f, 0.76f, 0.14f, 1.0f}}
                );
                auto roughPlastic = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.8f, .baseColor = {0.8f, 0.2f, 0.2f, 1.0f}}
                );

                for (int x = -2; x <= 2; ++x) {
                    for (int z = -2; z <= 2; ++z) {
                        if (x == 0 && z == 0)
                            continue;
                        float px = static_cast<float>(x) * 3.5f;
                        float pz = static_cast<float>(z) * 3.5f;
                        ZHLN::CreativeWorksFactory::CreateBox(
                            *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                            ZHLN::CreativeWorksFactory::SpawnParams {
                                .position = JPH::RVec3(px, 0.5, pz), .createPhysics = false, .materialOverride = ((x + z) % 2 == 0) ? *goldMat : *roughPlastic
                            }
                        );
                    }
                }

                constexpr size_t kLightCount = 32;
                for (size_t i = 0; i < kLightCount; ++i) {
                    float angle  = (static_cast<float>(i) / static_cast<float>(kLightCount)) * 6.283185f;
                    float radius = 5.0f + (static_cast<float>(i % 3) * 2.0f);
                    float lx     = std::sin(angle) * radius;
                    float lz     = std::cos(angle) * radius;
                    float ly     = 1.0f + static_cast<float>(i % 4) * 0.8f;

                    JPH::Vec3 lightCol(std::sin(angle) * 0.5f + 0.5f, std::cos(angle * 0.5f) * 0.5f + 0.5f, std::sin(angle * 1.5f + 1.0f) * 0.5f + 0.5f);

                    const ZHLN::Entity lt = reg.Create();
                    reg.Add(
                        lt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(lx, ly, lz)},
                        ZHLN::Components::LightComponent {
                            .type      = ZHLN::LightType::Point,
                            .color     = lightCol,
                            .intensity = 220.0f,
                            .range     = 12.0f,
                        }
                    );
                }

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 8.0f, -18.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 10, "dense_multi_light_emissive_materials_cross_interaction",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage frame      = Capture(eng, "headless_lighting_dense_interaction.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    const FrameMetrics m = MeasureImage(frame);

                    ZHLN::Println("    [INFO] Dense 32-Light + Emissive Monolith Scene Metrics:");
                    ZHLN::Println("      Lit Pixels: {}, Dark Pixels: {}, Saturated Pixels: {}, Mean Luma: {:.2f}", m.lit, m.dark, m.saturated, m.meanLuma);
                    ZHLN::Println(
                        "      Cyan (Monolith Reflection) Pixels: {}, Red Pixels: {}, Green Pixels: {}, Blue Pixels: {}", m.cyan, m.red, m.green, m.blue
                    );

                    const bool wellLit          = ZHLN::Test::ExpectTrue(m.lit > (m.total * 0.35));
                    const bool limitedBlowout   = ZHLN::Test::ExpectTrue(m.saturated < (m.total * 0.05));
                    const bool cyanObserved     = ZHLN::Test::ExpectTrue(m.cyan > 200u);
                    const bool multiColorActive = ZHLN::Test::ExpectTrue(m.red > 200u && m.green > 200u && m.blue > 200u);

                    return wellLit && limitedBlowout && cyanObserved && multiColorActive;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::DenseCrossInteractionFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] 32 Dynamic point lights and emissive monolith cross-interaction verified.");
            return {};
        }
    };
};

int main(int argc, char** argv) {
    if (argc >= 3 && std::string_view(argv[1]) == "--convert-ppm") {
        bool allOk = true;
        for (int i = 2; i < argc; ++i) {
            const RgbImage img = LoadPPM(argv[i]);
            if (!img.Valid()) {
                std::fprintf(stderr, "Failed to read: %s\n", argv[i]);
                allOk = false;
                continue;
            }
            const std::string png = PngPathOf(argv[i]);
            if (!SavePNG(png, img)) {
                std::fprintf(stderr, "Failed to write: %s\n", png.c_str());
                allOk = false;
                continue;
            }
            std::printf("converted %s -> %s\n", argv[i], png.c_str());
        }
        return allOk ? 0 : 1;
    }

    return ZHLN::Test::Runner::Run<LightingRTTestSuite>();
}
