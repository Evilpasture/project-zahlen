// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestLightingRayTraced.cpp
//
// Verification for the lighting + raytracing pipeline:
//
//   1. CPU: the sun-light resolution logic (which sun wins, direction
//      normalisation, SunTag fallback) -- the input to every lighting pass.
//   2. Flicker: a fully static, fully lit scene must produce temporally
//      stable frames. A light popping in/out of a cluster, a reflection
//      cache missing for one frame or a TLAS rebuild hitch shows up here.
//   3. Accidental light culling: a single point light glides across the
//      screen, crossing cluster-cell and z-slice boundaries, while its red
//      signature on the ground/box must never vanish or collapse. After the
//      sweep the light is frozen in place and 8 consecutive frames must be
//      identical (no oscillation, no step).
//   3b. Bisect for the above: the same static light on a FRESH engine with no
//      motion history pins whether an observed instability is carried over
//      from the sweep (stale double-buffered state) or is a pure per-frame
//      race. Both scenarios are HARD gates: if an identical light position
//      renders differently on adjacent frames the suite fails (this is the
//      regression guard for the cluster light-list inclusion race).
//   4. Ray-traced shadows: an occluder between the sun and the ground must
//      carve a real, stable shadow (not a full-scene blackout, not nothing),
//      and removing it must restore a near-uniformly lit floor.
//   5. Reflections: a polished plane must mirror a bright emissive object
//      with the engine's DEFAULT reflection path (SSR), stable frame to frame
//      (flicker), no blowout and no isolated ray-debris speckles. When the
//      device supports raytracing, the opt-in RTR reflection path is probed
//      for the same signature and reported as a diagnostic (RT ray-query
//      health itself is hard-verified by the RT shadow scenario).
//
// Configuration policy: the hard assertions run on the engine's default
// (enableSSR=1, enableRTR=0) so the suite verifies the shipped path. RTR is
// opt-in, and its pixel path being broken on a given driver must show up as a
// loud INFO/WARN without masking the default-path verification.
//
// Both the engine's device-lost hot-rebuild and the test's own RenderContext
// references are handled deliberately:
//
//   * Engine::HandleDeviceLost() destroys and RECREATES the RenderContext.
//     A RenderContext& cached at test start therefore dangles after a
//     hot-rebuild, so no reference is ever held across a Tick and every
//     capture re-fetches through Engine::GetRenderContext().
//   * Each GPU scenario runs through RunStableScene(), which detects the
//     hot-rebuild by comparing the context address, re-warms the fresh
//     context, discards any assertions raised by the aborted attempt, and
//     retries. Repeated losses are reported as DeviceLostDuringTest instead
//     of crashing the process with a stale reference.
//   * Assertions are RELATIVE (contrast/dip based) rather than absolute
//     pixel-count cutoffs, because the ACES/tonemap mapping is
//     exposure-dependent and differs between scenes.
//   * Every captured frame is written as PPM (engine-native, used by the
//     metrics) AND as a .png twin via stb_image_write.h, so diagnostics can
//     be attached to issues directly. Run
//       TestLightingRayTraced --convert-ppm old_capture_*.ppm
//     to convert captures from a previous run without re-running the suite.

#include "TestsFramework.hpp"
#include "engine/system/LightingSystem.hpp"
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

// stb_image_write is the only image writer the diagnostics need; the header
// is C and self-contained (no dynamic dependency). Defining the
// implementation here means the exported PNG helper lives only in this test
// binary and cannot collide with the engine's stb_image (read-only)
// implementation in libzahlen_engine.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// ============================================================================
// Test Error Types
// ============================================================================

enum class LightingRTTestError : uint8_t {
    Success = 0,
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine context for the lighting/raytracing test.")]],
    SunResolutionFailed[[= ZHLN::Reflect::Description("LightingSystem::GetSunDirectionAndIntensity resolved the wrong sun or a non-normalized direction.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is blank or could not be captured.")]],
    TemporalFlickerDetected[[= ZHLN::Reflect::Description("A static fully-lit scene changed more frame-to-frame than the engine's own noise floor.")]],
    LightCullingPopDetected[[= ZHLN::Reflect::Description(
        "A point light inside the frustum/range lost its lighting contribution for a frame (cluster culling)."
    )]],
    RayTracedShadowFailed[[= ZHLN::Reflect::Description("The ray-traced sun shadow did not appear, disappeared, or took out the whole frame.")]],
    ReflectionMissing[[= ZHLN::Reflect::Description("The polished surface shows no reflection of the emissive object (RTR/SSR fell back to IBL).")]],
    ReflectionArtifacts[[= ZHLN::Reflect::Description("The reflected region contains blowout, ray-debris speckles, or flicker.")]],
    DeviceLostDuringTest[[= ZHLN::Reflect::Description(
        "The Vulkan device was lost repeatedly during the scenario; the engine hot-rebuild recovered, but the GPU was not stable."
    )]],
    ValidationErrorsRaised[[= ZHLN::Reflect::Description("The validation layer reported errors while rendering the lighting/raytracing frames.")]],
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

/// Replaces a trailing ".ppm"/".PPM" with ".png"; appends ".png" otherwise.
[[nodiscard]] std::string PngPathOf(std::string_view ppmPath) {
    std::string png(ppmPath);
    if (png.size() >= 4 && (png.ends_with(".ppm") || png.ends_with(".PPM"))) {
        png.resize(png.size() - 4);
    }
    png += ".png";
    return png;
}

/// Writes an RGB image as PNG via stb_image_write. Every captured frame is
/// exported as both PPM (engine-native, kept for the test metrics) and PNG
/// (browser/mail friendly, for attaching to bug reports).
[[nodiscard]] bool SavePNG(const std::string& path, const RgbImage& img) {
    if (!img.Valid()) {
        return false;
    }
    return stbi_write_png(path.c_str(), img.width, img.height, 3, img.rgb.data(), img.width * 3) != 0;
}

inline double Luma(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) + 0.0722 * static_cast<double>(b);
}

/// Per-frame aggregate statistics. `minRowFraction` restricts the analysis to
/// the lower part of the framebuffer (rows >= fraction * height). Used both
/// for plane/reflection regions (so the emissive source itself is excluded
/// from the reflection signature) and for foreground floor-only regions (so
/// the occluder's own dark silhouette is excluded from the shadow signature).
struct FrameMetrics {
    uint32_t total       = 0; // Pixels considered
    uint32_t lit         = 0; // luma > 40 (air/exposure dependent; never assume a >96 cutoff)
    uint32_t dark        = 0; // luma < 24
    uint32_t saturated   = 0; // r,g,b all >= 250 (blowout)
    uint32_t red         = 0; // r >= 60 and clearly red-dominant (light/reflection signature)
    uint32_t redPeak     = 0; // brightest red-dominant pixel
    uint32_t redIsolated = 0; // red pixels with 3+ of 4 neighbours black: speckles/ray debris
    double   meanLuma    = 0.0;
};

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

            // Reflection coherence: an honest mirror image is a contiguous
            // patch. Single-pixel red islands surrounded by black are the
            // signature of ray-query debris / TLAS garbage (artifacts).
            const int x = static_cast<int>(pixel % static_cast<size_t>(img.width));
            uint32_t blackNeighbours = 0;
            const auto isBlackAt = [&](int nx, int ny) -> bool {
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
    }

    if (m.total > 0) {
        m.meanLuma = lumaSum / static_cast<double>(m.total);
    }
    return m;
}

struct FrameDiff {
    uint32_t over12  = 0;
    uint32_t over32  = 0;
    double   meanAbs = 0.0;
    double   frac12  = 0.0;
    double   frac32  = 0.0;
};

/// Where the changed pixels live. `count==0` and 0/0 bounds mean no change.
/// Used to attribute a frame-parity oscillation: whole-frame (target/tone-map
/// parity), a specific object (geometry/culling parity) or the light patch
/// (cluster/light-buffer parity).
struct ChangedRegion {
    uint32_t count = 0;
    int      minX  = 0;
    int      maxX  = 0;
    int      minY  = 0;
    int      maxY  = 0;
    int      maxDelta = 0;
    double   meanDelta = 0.0;
    // Mean channel values of the changed pixels in each state, so the region's
    // identity is unambiguous: gray floor vs red glow vs sky vs box silhouette.
    double aR = 0.0, aG = 0.0, aB = 0.0;
    double bR = 0.0, bG = 0.0, bB = 0.0;
};

[[nodiscard]] ChangedRegion DiffRegion(const RgbImage& a, const RgbImage& b, int threshold = 32) {
    ChangedRegion r;
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return r;
    }

    r.minX = a.width;
    r.minY = a.height;
    r.maxX = -1;
    r.maxY = -1;
    uint64_t sum = 0, sumAr = 0, sumAg = 0, sumAb = 0, sumBr = 0, sumBg = 0, sumBb = 0;

    for (size_t i = 0; i < a.rgb.size(); i += 3) {
        const int dr    = std::abs(static_cast<int>(a.rgb[i + 0]) - static_cast<int>(b.rgb[i + 0]));
        const int dg    = std::abs(static_cast<int>(a.rgb[i + 1]) - static_cast<int>(b.rgb[i + 1]));
        const int db    = std::abs(static_cast<int>(a.rgb[i + 2]) - static_cast<int>(b.rgb[i + 2]));
        const int worst = std::max({dr, dg, db});
        r.maxDelta = std::max(r.maxDelta, worst);
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
        r.aR = static_cast<double>(sumAr) / r.count;
        r.aG = static_cast<double>(sumAg) / r.count;
        r.aB = static_cast<double>(sumAb) / r.count;
        r.bR = static_cast<double>(sumBr) / r.count;
        r.bG = static_cast<double>(sumBg) / r.count;
        r.bB = static_cast<double>(sumBb) / r.count;
    }
    return r;
}

/// Writes a small PPM crop of `region` from `img` (clamped to bounds) so the
/// toggling region can be inspected directly without full-frame files.
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

    const int    w = x1 - x0 + 1;
    const int    h = y1 - y0 + 1;
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
            crop[dst + 0] = img.rgb[src + 0];
            crop[dst + 1] = img.rgb[src + 1];
            crop[dst + 2] = img.rgb[src + 2];
        }
    }
    out.write(reinterpret_cast<const char*>(crop.data()), static_cast<std::streamsize>(crop.size()));

    // PNG twin (browser/mail friendly) for attaching to bug reports.
    RgbImage pngCrop {.width = w, .height = h, .rgb = crop};
    (void) SavePNG(PngPathOf(path), pngCrop);
}

/// Writes an amplified absolute-difference image so the parity region is
/// inspectable: `headless_lighting_rt_cull_parity_diff.ppm` (plus .png twin).
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
        const int  d        = std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
        amplified[i]        = static_cast<uint8_t>(std::min(255, d * 4));
    }
    out.write(reinterpret_cast<const char*>(amplified.data()), static_cast<std::streamsize>(amplified.size()));

    // PNG twin.
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

/// Coefficient of variation: relative temporal jitter of a metric.
/// 0 for a constant signal, NAN/0-guarded for a zero-valued signal.
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

constexpr uint32_t kMaxDeviceLostRecoveries = 2; // 3 attempts total

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
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
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

    /// TAA must be disabled at the component level: RenderSystem re-pushes the
    /// camera's AA state into the RenderContext every tick, so a SetAAState
    /// call alone gets overwritten, and TAA jitter would dominate the
    /// frame-to-frame comparison.
    ///
    /// Stability guard: an identical light position must render identically on
    /// every frame. This is the hard gate for the clustered light-list
    /// inclusion race (a point light flickering in/out at its range boundary),
    /// which is fixed by:
    ///   * SetFrameData mirroring the light/uniform data into BOTH parity slots
    ///     each frame (after the previous frame is retired), so no consumer can
    ///     ever observe a stale or empty alternate slot,
    ///   * a compute-queue memory barrier after cluster culling so
    ///     volumetric injection cannot read the previous frame's lists,
    ///   * the graphics submission waiting on the compute timeline across
    ///     ALL_COMMANDS (previously FRAGMENT_SHADER only), so graphics-side
    ///     compute/vertex stages cannot race the async compute queue.
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

    /// Captures through Engine::GetRenderContext() so the call always uses the
    /// CURRENT context, never a reference that may dangle after a hot-rebuild.
    /// Captures through Engine::GetRenderContext() so the call always uses the
    /// CURRENT context, never a reference that may dangle after a hot-rebuild.
    /// Every capture is exported as PPM (engine-native, used by the metrics)
    /// plus a PNG twin for viewing/attaching (browsers and most apps reject
    /// PPM; see stb_image_write above).
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

    /// Runs `sceneFn` after `warmupFrames` (to warm Hi-Z history, TLAS, cluster
    /// buffers etc.) and tolerates the engine's device-lost hot-rebuild:
    ///
    ///  * Engine::HandleDeviceLost() destroys and recreates the RenderContext, so
    ///    any RenderContext reference cached by the caller is dangling afterwards.
    ///    Callers must therefore never cache such a reference across Ticks; this
    ///    runner detects the replacement by comparing the context address.
    ///  * If the context is replaced (during warm-up or the scenario), the
    ///    scenario is retried on the freshly rebuilt context after re-warming,
    ///    and any assertion failures raised by the aborted attempt are discarded
    ///    (they describe the crashed attempt, not the retried one).
    ///  * `sceneFn` returning false with NO context replacement means the image
    ///    assertions themselves failed; that is reported without a retry so real
    ///    regressions are never masked by the recovery logic.
    template <typename SceneFn>
    [[nodiscard]] static StableRunResult RunStableScene(
        ZHLN::Engine& engine, uint32_t warmupFrames, const char* label, SceneFn&& sceneFn, uint32_t* outValidationDelta = nullptr
    ) {
        auto&        ctx         = ZHLN::Test::GetThreadLocalContext();
        const size_t failureMark = ctx.failures.size();

        for (uint32_t attempt = 0; attempt <= kMaxDeviceLostRecoveries; ++attempt) {
            if (attempt > 0) {
                // The previous attempt was aborted by a device loss; its assertion
                // failures describe the dead context, not the retry.
                ctx.failures.resize(failureMark);
                ZHLN::Println(
                    "    [WARN] {}: Vulkan device lost; engine hot-rebuilt. Re-warming and retrying (attempt {}/{}).", label, attempt,
                    kMaxDeviceLostRecoveries
                );
            }

            // Validation count is snapshotted per attempt: VUIDs raised by the
            // aborted attempt belong to the lost device and must not fail the
            // retried attempt.
            const uint32_t validationBefore = ZHLN::RenderContext::ValidationErrorCount();

            // Warm-up on the current context. If the device is lost here the loop
            // simply re-warms the fresh context.
            ZHLN::RenderContext* const preWarmup = &engine.GetRenderContext();
            TickFrames(engine, warmupFrames);
            if (&engine.GetRenderContext() != preWarmup) {
                continue;
            }

            // Run the scenario and detect a hot-rebuild that happened mid-scenario.
            ZHLN::RenderContext* const preWork = &engine.GetRenderContext();
            const bool                  ok      = sceneFn(engine);
            if (ok && &engine.GetRenderContext() == preWork) {
                if (outValidationDelta != nullptr) {
                    *outValidationDelta = ZHLN::RenderContext::ValidationErrorCount() - validationBefore;
                }
                return StableRunResult::Ok;
            }
            if (&engine.GetRenderContext() == preWork) {
                return StableRunResult::AssertionsFailed;
            }
            // Context replaced mid-scenario: retry.
        }

        return StableRunResult::PersistentDeviceLost;
    }

    struct Tests {
        // ====================================================================
        // 1. CPU: Sun-light resolution & normalisation
        // ====================================================================
        //
        // This is the exact code path every lighting variant consumes, so a
        // regression here (wrong sun picked when several exist, direction not
        // normalized, SunTag fallback broken) silently changes every frame.
        std::expected<void, ZHLN::Error> lighting_sun_resolution_and_normalization() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponents<
                ZHLN::Components::LightComponent, ZHLN::Components::SunTagComponent, ZHLN::Components::TransformComponent,
                ZHLN::Components::WorldTransformComponent>();

            // --- a) Explicit direction: must be normalised and preserved ---
            const ZHLN::Entity sunA = reg.Create();
            reg.Add(
                sunA, ZHLN::Components::LightComponent {
                          .type      = ZHLN::LightType::Sun,
                          .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                          .intensity = 111.0f,
                          .direction = JPH::Vec3(0.0f, 3.0f, 4.0f)
                      }
            );

            // --- b) A second sun must NOT silently override the first one ---
            const ZHLN::Entity sunB = reg.Create();
            reg.Add(
                sunB, ZHLN::Components::LightComponent {
                          .type      = ZHLN::LightType::Sun,
                          .color     = JPH::Vec3(1.0f, 0.5f, 0.5f),
                          .intensity = 222.0f,
                          .direction = JPH::Vec3(0.0f, -1.0f, 1.0f)
                      }
            );

            const auto [dir, intensity] = ZHLN::LightingSystem::GetSunDirectionAndIntensity(reg);

            ZHLN::Test::ExpectTrue(std::abs(dir.GetX() - 0.0f) < 1e-5f);
            ZHLN::Test::ExpectTrue(std::abs(dir.GetY() - 0.6f) < 1e-5f);
            ZHLN::Test::ExpectTrue(std::abs(dir.GetZ() - 0.8f) < 1e-5f);
            ZHLN::Test::ExpectTrue(std::abs(dir.Length() - 1.0f) < 1e-5f);
            ZHLN::Test::ExpectEq(intensity, 111.0f);
            if (std::abs(dir.GetY() - 0.6f) > 1e-5f || std::abs(dir.Length() - 1.0f) > 1e-5f || intensity != 111.0f) {
                return std::unexpected(LightingRTTestError::SunResolutionFailed);
            }

            // --- c) SunTag fallback: no LightType::Sun -> transform Z axis ---
            ZHLN::ECS::Registry fallbackReg;
            fallbackReg.RegisterComponents<
                ZHLN::Components::LightComponent, ZHLN::Components::SunTagComponent, ZHLN::Components::TransformComponent,
                ZHLN::Components::WorldTransformComponent>();

            const ZHLN::Entity tagged = fallbackReg.Create();
            const JPH::Quat   yaw30   = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), JPH::DegreesToRadians(30.0f));
            fallbackReg.Add(
                tagged,
                ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 10.0f, 0.0f), .rotation = yaw30},
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Point,
                    .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                    .intensity = 77.0f,
                    .direction = JPH::Vec3::sZero()
                },
                ZHLN::Components::SunTagComponent {}
            );

            const auto [fallbackDir, fallbackIntensity] = ZHLN::LightingSystem::GetSunDirectionAndIntensity(fallbackReg);

            // Yaw 30deg about Y: local Z axis (world transform column 2) = (sin30, 0, cos30).
            ZHLN::Test::ExpectTrue(std::abs(fallbackDir.GetX() - 0.5f) < 1e-4f);
            ZHLN::Test::ExpectTrue(std::abs(fallbackDir.GetZ() - 0.8660254f) < 1e-4f);
            ZHLN::Test::ExpectTrue(std::abs(fallbackDir.Length() - 1.0f) < 1e-5f);
            ZHLN::Test::ExpectEq(fallbackIntensity, 77.0f);
            if (std::abs(fallbackDir.GetX() - 0.5f) > 1e-4f || std::abs(fallbackDir.Length() - 1.0f) > 1e-5f || fallbackIntensity != 77.0f) {
                return std::unexpected(LightingRTTestError::SunResolutionFailed);
            }

            ZHLN::Println(
                "    [PASS] Sun resolution: explicit dir normalized {:.4f}/{:.4f}/{:.4f}, first-sun wins, SunTag fallback ok.", dir.GetX(),
                dir.GetY(), dir.GetZ()
            );
            return {};
        }

        // ====================================================================
        // 2. GPU: static scene must not flicker
        // ====================================================================
        std::expected<void, ZHLN::Error> lit_scene_static_frame_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            // Scene setup happens BEFORE any Tick, so the RenderContext
            // references below cannot dangle. The scenario body re-fetches the
            // context through Engine::GetRenderContext() instead.
            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                // Full PBR lighting: no fullbright override, moderate exposure.
                // The flicker scenario deliberately uses the engine's DEFAULT
                // configuration (SSR on, RTR off): RTR is opt-in and its pixel
                // path is covered separately (RT shadow + reflection diagnostics).
                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                ZHLN::Test::ExpectTrue(!settingsEnts.empty());
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 10.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                // Ground + three contrasting surfaces (mid gray, diffuse red,
                // rough blue). All are kept above the reflection threshold
                // (roughness > 0.4) so the flicker measurement isolates lighting
                // and cluster stability rather than screen-space noise.
                // Note: SpawnParams.roughness/metallic are ignored by the
                // factory spawners -- materials below control the shading.
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

                // Sun plus two punctual lights for a realistic multi-source scene.
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt, ZHLN::Components::TransformComponent {
                                .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                            },
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 220.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                auto addPointLight = [&](const JPH::Vec3& pos, const JPH::Vec3& color, float intensity) {
                    const ZHLN::Entity e = reg.Create();
                    reg.Add(
                        e,
                        ZHLN::Components::TransformComponent {.position = pos},
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

            const auto stable = RunStableScene(*engine, 14, "lit_scene_static_frame_stability", [](ZHLN::Engine& eng) -> bool {
                // Repeat-capture control: two captures of the SAME frame tell us
                // what the engine's own readback noise floor is.
                const RgbImage repeatA = Capture(eng, "headless_lighting_rt_static_r0.ppm");
                const RgbImage repeatB = Capture(eng, "headless_lighting_rt_static_r1.ppm");
                const FrameDiff repeatDiff = CompareFrames(repeatA, repeatB);

                std::vector<double>       litSeries;
                std::vector<double>       lumaSeries;
                std::vector<double>       redSeries;
                std::vector<double>       satSeries;
                std::vector<FrameDiff>    temporalDiffs;

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

                // Blank-frame guard: a black capture means the renderer itself
                // produced nothing (e.g. broken post-rebuild state), which is a
                // different failure than flicker.
                const bool frameProduced = ZHLN::Test::ExpectTrue(Mean(lumaSeries) > 1.0);
                const bool geometryVisible = ZHLN::Test::ExpectTrue(Mean(litSeries) > 500.0);
                if (!frameProduced || !geometryVisible) {
                    return false;
                }

                const double litCV    = CoefficientOfVariation(litSeries);
                const double lumaCV   = CoefficientOfVariation(lumaSeries);
                const double redCV    = CoefficientOfVariation(redSeries);
                const double satCV    = CoefficientOfVariation(satSeries);
                const double litMean  = Mean(litSeries);

                double worstFrac32 = 0.0;
                double maxJump     = 0.0;
                for (size_t i = 0; i < temporalDiffs.size(); ++i) {
                    worstFrac32 = std::max(worstFrac32, temporalDiffs[i].frac32);
                    if (i + 1 < litSeries.size() && litMean > 1.0) {
                        const double jump = std::abs(litSeries[i + 1] - litSeries[i]) / litMean;
                        maxJump          = std::max(maxJump, jump);
                    }
                }

                ZHLN::Println(
                    "    [INFO] static scene: lit={:.0f} (cv {:.5f}), luma={:.2f} (cv {:.5f}), red={:.0f} (cv {:.5f}), "
                    "repeat-capture mean|d|={:.5f}, inter-frame |d|>32 frac={:.6f}, max jump={:.4f}",
                    litMean, litCV, Mean(lumaSeries), lumaCV, Mean(redSeries), redCV, repeatDiff.meanAbs, worstFrac32, maxJump
                );

                // Thresholds are generous enough for dither/ACES rounding but a
                // light pop, a dropped reflection or a TLAS hitch moves far more.
                // Tiny saturated counts (a few pixels of sun glint) legitimately
                // jitter by a couple of pixels, so only enforce their CV when a
                // meaningful blowout region exists.
                const double meanSaturated = Mean(satSeries);
                const bool stableLit   = ZHLN::Test::ExpectTrue(litCV < 0.03);
                const bool stableLuma  = ZHLN::Test::ExpectTrue(lumaCV < 0.01);
                const bool stableRed   = ZHLN::Test::ExpectTrue(redCV < 0.05);
                const bool stableSat   = ZHLN::Test::ExpectTrue(satCV < 0.25 || meanSaturated < 100.0);
                const bool noPixelPop  = ZHLN::Test::ExpectTrue(worstFrac32 < 0.01);
                const bool noJump      = ZHLN::Test::ExpectTrue(maxJump < 0.08);
                const bool noBlowout   = ZHLN::Test::ExpectTrue(meanSaturated < 0.02 * static_cast<double>(640 * 480));

                return stableLit && stableLuma && stableRed && stableSat && noPixelPop && noJump && noBlowout;
            }, &validationRaised);

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
        // 3. GPU: point light crossing cluster boundaries must never pop
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_cluster_culling_sweep() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            ZHLN::Entity redLight = ZHLN::NullEntity;
            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();
                // Dim ambient + dim sun so the red point light owns the frame.
                // GI/AO are disabled (giMode=0) and RTR is disabled (enableRTR=0)
                // so the sweep isolates CLUSTERED DIRECT LIGHTING -- the only
                // pass under test. ambient.slang otherwise rotates its AO/GI
                // pattern by a per-frame time offset and lighting.slang switches
                // the sun onto the ray-traced shadow path (CalculateShadowRay
                // Traced); both are covered by the static flicker and RT shadow
                // scenarios respectively and add unrelated per-frame variation
                // to a pixel-count metric.
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

                // NOTE: SpawnParams.roughness/metallic are ignored by factory
                // spawners (they hard-code material factors); an explicit material
                // is required to keep the target surface diffuse. A sharp specular
                // highlight would dominate the sweep metric with angle-dependent
                // spikes instead of the broad diffuse patch that reveals culling.
                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes
                    }
                );
                // Diffuse gray target box: the red light's signature is unambiguous.
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes
                    }
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );

                redLight = reg.Create();
                reg.Add(
                    redLight,
                    ZHLN::Components::TransformComponent {.position = JPH::Vec3(-6.4f, 2.4f, 5.5f)},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f; // Look along +Z toward the target box
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(*engine, 8, "point_light_cluster_culling_sweep", [&](ZHLN::Engine& eng) -> bool {
                auto& reg = eng.GetRegistry();

                // The registry survives a hot-rebuild, so the light handle is
                // still valid and is patched directly.
                ZHLN::Test::ExpectTrue(reg.IsAlive(redLight));

                std::vector<double>   redCounts;
                std::vector<uint32_t> redPeaks;
                uint32_t              isolatedDips = 0;

                constexpr int   kSteps  = 21; // x = -6.4 .. +6.4, stays inside the frustum
                constexpr float kStepX  = 0.64f;
                for (int step = 0; step < kSteps; ++step) {
                    const float x = -6.4f + static_cast<float>(step) * kStepX;
                    // Slight diagonal drift crosses multiple z-slices while the
                    // light stays inside the horizontal frustum half-width.
                    const float z = 5.5f - 0.078125f * x;

                    reg.Patch<ZHLN::Components::TransformComponent>(redLight, [&](auto& t) { t.position = JPH::Vec3(x, 2.4f, z); });

                    // Two ticks settle the transform + cluster rebuild before capture.
                    TickFrames(eng, 2);

                    const RgbImage frame = Capture(eng, "headless_lighting_rt_cull_" + std::to_string(step) + ".ppm");
                    auto checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    const FrameMetrics m = MeasureImage(frame);
                    redCounts.push_back(static_cast<double>(m.red));
                    redPeaks.push_back(m.redPeak);
                }

                // A light-culling pop is an ISOLATED V-shaped dip between two
                // healthy neighbours (light present on both sides, missing for
                // one frame). A smooth sweep has a monotone-ish gradient, so
                // large consecutive deltas from geometric falloff are not pops.
                for (size_t i = 1; i + 1 < redCounts.size(); ++i) {
                    const double left  = redCounts[i - 1];
                    const double right = redCounts[i + 1];
                    const double base  = std::min(left, right);
                    if (base > 64.0 && redCounts[i] < 0.5 * base) {
                        ++isolatedDips;
                    }
                }

                // Same-position stability: eight captures at the FIXED final
                // position, one frame apart, after an explicit settling tick.
                // A genuine frame-alternating artifact (cluster buffer
                // ping-pong, double-buffered resource parity) shows up as a
                // persistent A/B/A/B oscillation; a longer-period artifact
                // (e.g. a culling-state or history step a few frames after the
                // light stops) shows up as a one-way jump. Both are failures:
                // an identical light position must render identically. Every
                // adjacent pair is compared and the worst pair's changed-pixel
                // region is reported with an amplified diff image.
                TickFrames(eng, 1); // settle after the sweep
                constexpr uint32_t kStableFrames = 8;
                std::array<RgbImage, kStableFrames> stableFrames {};
                std::vector<double>                 stableCounts;
                std::vector<double>                 stableLuma;
                std::vector<double>                 stableLit;
                std::vector<uint64_t>               stableFrameIdx;
                for (uint32_t r = 0; r < kStableFrames; ++r) {
                    stableFrameIdx.push_back(eng.GetCurrentFrame());
                    stableFrames[r] = Capture(eng, "headless_lighting_rt_cull_stable_" + std::to_string(r) + ".ppm");
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

                // Attribute the change: diff every adjacent pair and keep the
                // worst one, plus its changed-pixel bounding box so the failing
                // region is identifiable in the log and as an amplified diff
                // image (whole frame vs target box vs floor light patch).
                double            worstPairFrac = 0.0;
                size_t            worstPair     = 0;
                FrameDiff         worstDiffs[kStableFrames - 1] {};
                for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                    worstDiffs[i] = CompareFrames(stableFrames[i], stableFrames[i + 1]);
                    if (worstDiffs[i].frac32 > worstPairFrac) {
                        worstPairFrac = worstDiffs[i].frac32;
                        worstPair     = i;
                    }
                }
                const ChangedRegion region = DiffRegion(stableFrames[worstPair], stableFrames[worstPair + 1]);
                WriteAmplifiedDiff(
                    "headless_lighting_rt_cull_parity_diff.ppm", stableFrames[worstPair], stableFrames[worstPair + 1]
                );
                WriteRegionCrop("headless_lighting_rt_cull_region_a.ppm", stableFrames[worstPair], region);
                WriteRegionCrop("headless_lighting_rt_cull_region_b.ppm", stableFrames[worstPair + 1], region);

                const double minRed = *std::ranges::min_element(redCounts);
                const double maxRed = *std::ranges::max_element(redCounts);
                const uint32_t minPeak = *std::ranges::min_element(redPeaks);

                // Print every frame's frame-index, red, lit and luma so the
                // period of any step/oscillation is visible in the log.
                ZHLN::Println("    [INFO] light sweep: red pixels min={:.0f} max={:.0f} across {} samples, red peak floor={}, isolated dips={}", minRed,
                              maxRed, redCounts.size(), minPeak, isolatedDips);
                for (uint32_t i = 0; i < kStableFrames; ++i) {
                    ZHLN::Println(
                        "      stable[{}] frame={} red={:.0f} lit={:.0f} luma={:.4f} | prev-pair |d|>32={:.6f} (mean|d|={:.5f})",
                        i, stableFrameIdx[i], stableCounts[i], stableLit[i], stableLuma[i],
                        (i > 0) ? worstDiffs[i - 1].frac32 : 0.0, (i > 0) ? worstDiffs[i - 1].meanAbs : 0.0
                    );
                }
                ZHLN::Println(
                    "      worst adjacent pair {}-{}: |d|>32={:.6f} mean|d|={:.5f} | changed region [{},{}]-[{},{}] px={} mean|d|={:.1f} max|d|={} | "
                    "stateA rgb=({:.1f},{:.1f},{:.1f}) stateB rgb=({:.1f},{:.1f},{:.1f})",
                    worstPair, worstPair + 1, worstPairFrac, worstDiffs[worstPair].meanAbs, region.minX, region.minY, region.maxX, region.maxY,
                    region.count, region.meanDelta, region.maxDelta, region.aR, region.aG, region.aB, region.bR, region.bG, region.bB
                );

                // Invariant 1: the light stays within range and on screen for the
                // whole sweep, so its signature must never collapse to zero.
                const bool neverCulled      = ZHLN::Test::ExpectTrue(minRed > 16.0);
                const bool brightEverywhere = ZHLN::Test::ExpectTrue(minPeak > 60u);
                // Invariant 2: no single-frame culling holes between healthy
                // neighbours.
                const bool noIsolatedCull = ZHLN::Test::ExpectTrue(isolatedDips == 0u);
                // Invariant 3: once settled, an identical light position must
                // render identically. Any adjacent-pair change above the
                // engine's noise floor is a hard failure -- this is what
                // catches the clustered light-list inclusion race (light
                // flickering in/out at its range boundary). Diagnostics
                // (parity diff + region crops) are always written so a failing
                // run is immediately inspectable.
                const bool noStaticStep = ZHLN::Test::ExpectTrue(worstPairFrac < 0.005);

                return neverCulled && brightEverywhere && noIsolatedCull && noStaticStep;
            }, &validationRaised);

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
        // 3b. GPU: same static light with NO motion history (bisect)
        // ====================================================================
        //
        // The sweep scenario reproduced a bistable light patch (1601 vs 2260
        // red pixels) that flips between runs or a few frames after the sweep
        // ends. This scenario creates the SAME scene on a FRESH engine with the
        // light already at the final position -- no sweep, no motion history.
        //   * If this is STABLE and the sweep is not, the artifact is carried
        //     over by history (a double-buffered cluster/light slot left stale
        //     by the motion) -> engine-side stale-state bug.
        //   * If this ALSO flips, the artifact is a per-frame race independent
        //     of motion -> engine-side sync bug.
        //   * If this flips only sometimes, it is race-dependent and the
        //     repeat count decides.
        std::expected<void, ZHLN::Error> point_light_static_reference_no_history() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            // Bisect knobs (test-only): the strip that flips is exactly at the
            // light's range cutoff (~40m), so raising the range moves the
            // marginal cells. If the flapping disappears (or moves with the
            // glow edge), range-boundary inclusion is confirmed as the flip
            // point of the race.
            //   ZHLN_TEST_LIGHT_RANGE=60        (point light range)
            //   ZHLN_TEST_LIGHT_POS="4,2.4,5"   (point light position)
            const char* rangeEnv = std::getenv("ZHLN_TEST_LIGHT_RANGE");
            const float lightRange = rangeEnv != nullptr ? std::strtof(rangeEnv, nullptr) : 40.0f;
            const char* posEnv     = std::getenv("ZHLN_TEST_LIGHT_POS");
            float       posX = 6.4f, posY = 2.4f, posZ = 5.0f;
            if (posEnv != nullptr) {
                char* end    = nullptr;
                posX         = std::strtof(posEnv, &end);
                posY         = std::strtof(end, &end);
                posZ         = std::strtof(end, nullptr);
                // Guard against a malformed variable (e.g. "4,2.4,15" with a
                // locale that stops at the first non-digit): fall back to the
                // default position so a bad env var can never silently place
                // the light at the world origin.
                if (end == posEnv || posX == 0.0f && posY == 0.0f && posZ == 0.0f) {
                    posX = 6.4f;
                    posY = 2.4f;
                    posZ = 5.0f;
                }
            }
            const JPH::Vec3 lightPos(posX, posY, posZ);

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

                // Diffuse surface + static red light at the sweep's FINAL
                // position (6.4, 2.4, 5.0). No movement ever happens.
                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes
                    }
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );
                const ZHLN::Entity redLight = reg.Create();
                reg.Add(
                    redLight,
                    ZHLN::Components::TransformComponent {.position = lightPos},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f),
                        .intensity = 1600.0f, .range = lightRange
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
                    constexpr uint32_t kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> frames {};
                    std::vector<double>                 counts;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                    frames[r] = Capture(eng, "headless_lighting_rt_ref_stable_" + std::to_string(r) + ".ppm");
                    auto checkFrame = ZHLN::Test::AssertTrue(frames[r].Valid());
                    if (!checkFrame) {
                        return false;
                    }
                    counts.push_back(static_cast<double>(MeasureImage(frames[r]).red));

                    TickFrames(eng, 1);
                }

                double worstFrac  = 0.0;
                size_t worstPair  = 0;
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

                ZHLN::Println(
                    "    [INFO] static reference (no history): light range={:.1f} pos=({:.1f},{:.1f},{:.1f}) | red {:.0f} {:.0f} {:.0f} {:.0f} "
                    "{:.0f} {:.0f} {:.0f} {:.0f}, worst adj |d|>32={:.6f} | changed region [{},{}]-[{},{}] px={} mean|d|={:.1f} max|d|={} | "
                    "stateA rgb=({:.1f},{:.1f},{:.1f}) stateB rgb=({:.1f},{:.1f},{:.1f})",
                    lightRange, lightPos.GetX(), lightPos.GetY(), lightPos.GetZ(), counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
                    counts[6], counts[7], worstFrac, region.minX, region.minY, region.maxX, region.maxY, region.count, region.meanDelta,
                    region.maxDelta, region.aR, region.aG, region.aB, region.bR, region.bG, region.bB
                );

                // Hard invariant: a no-history static light must render
                // identically on every frame. This is the bisect that pins the
                // light-list inclusion race (see sweep scenario).
                const bool noStaticStep  = ZHLN::Test::ExpectTrue(worstFrac < 0.005);
                const bool lightVisible  = ZHLN::Test::ExpectTrue(Mean(counts) > 16.0);

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
        // 4. GPU: ray-traced sun shadow must exist and be stable
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
                        pp.enableRTR       = 1; // Switches the lighting pass onto CalculateShadowRayTraced.
                    });
                }

                // Explicit diffuse floor material (factory spawners hard-code
                // material factors; see CreatePlane in CreativeWorksFactory.cpp).
                auto floorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.55f, 0.55f, 0.55f, 1.0f}}
                );
                auto checkFloorMat = ZHLN::Test::AssertTrue(floorMatRes.has_value());
                if (!checkFloorMat) {
                    return checkFloorMat;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 400.0f, {0.55f, 0.55f, 0.55f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMatRes
                    }
                );

                // Sun from the +X side at ~37 degrees elevation. The occluder
                // shadow then falls onto the OPEN floor between the wall and the
                // camera, so the ray-traced shadow is fully visible instead of
                // hiding behind the occluder's own silhouette.
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(60.0f, 45.0f, 0.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({0.0f, 90.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 240.0f,
                        .direction = JPH::Vec3(0.8f, 0.6f, 0.0f).Normalized()
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(-10.0f, 6.0f, -8.0f);
                cam.yaw      = 0.0f; // Look along +X toward the occluder wall
                cam.pitch    = -20.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(*engine, 8, "raytraced_shadow_occlusion_and_stability", [](ZHLN::Engine& eng) -> bool {
                auto& reg = eng.GetRegistry();

                // Spawn the occluder fresh on every attempt: a retried attempt
                // may start with it already destroyed by the previous attempt.
                const ZHLN::Entity occluder = ZHLN::CreativeWorksFactory::CreateBox(
                    eng, JPH::Vec3(0.5f, 3.0f, 4.0f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 3.0, -8.0), .createPhysics = false, .color = {0.7f, 0.7f, 0.7f, 1.0f}
                    }
                );

                // Let the new entity enter the TLAS/draw queue before capture.
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

                // Two ticks: the removed instance must leave the TLAS before capture.
                TickFrames(eng, 2);
                const RgbImage shadowClear = Capture(eng, "headless_lighting_rt_shadow_clear.ppm");
                checkFrame = ZHLN::Test::AssertTrue(shadowClear.Valid());
                if (!checkFrame) {
                    return false;
                }

                // Analyze only the foreground floor rows (>= 72% height): the
                // occluder's own dark silhouette tops out at ~67% height, and the
                // sky band (if any) sits at the very top, so this region isolates
                // the cast shadow on the floor.
                constexpr double kFloorRowFraction = 0.72;
                const FrameMetrics mA        = MeasureImage(shadowA, kFloorRowFraction);
                const FrameMetrics mA2       = MeasureImage(shadowARepeat, kFloorRowFraction);
                const FrameMetrics mB        = MeasureImage(shadowB, kFloorRowFraction);
                const FrameMetrics mClear    = MeasureImage(shadowClear, kFloorRowFraction);

                const uint32_t darkA      = mA.dark;
                const uint32_t darkA2     = mA2.dark;
                const uint32_t darkB      = mB.dark;
                const uint32_t darkClear  = mClear.dark;
                const uint32_t litA       = mA.lit;
                const uint32_t litClear   = mClear.lit;

                const FrameDiff repeatDiff   = CompareFrames(shadowA, shadowARepeat);
                const FrameDiff temporalDiff = CompareFrames(shadowA, shadowB);

                const double darkJump = (darkA > 0) ? static_cast<double>(std::abs(static_cast<int64_t>(darkB) - static_cast<int64_t>(darkA))) /
                                                          static_cast<double>(darkA)
                                                    : 0.0;

                ZHLN::Println(
                    "    [INFO] RT shadow: occluded dark={} lit={} | repeat dark={} | cleared dark={} lit={} | "
                    "temporal mean|d|={:.4f} |d|>32={:.6f}, repeat mean|d|={:.5f}, dark jump {:.3f}",
                    darkA, litA, darkA2, darkClear, litClear, temporalDiff.meanAbs, temporalDiff.frac32, repeatDiff.meanAbs, darkJump
                );

                // 1. The shadow must exist: removing the occluder removes at least
                //    a substantial dark region.
                const bool shadowExist   = ZHLN::Test::ExpectTrue(darkA > darkClear + 1500u);
                // 2. Light is restored without the occluder: darkness collapses
                //    and the average floor luminance rises. Absolute 'lit' counts
                //    depend on exposure/ACES, so relative contrast is the
                //    trustworthy signal.
                const bool lightRestored = ZHLN::Test::ExpectTrue(darkClear < darkA / 3u);
                const bool meanBrightens =
                    ZHLN::Test::ExpectTrue(mClear.meanLuma > mA.meanLuma * 1.25 + 1.0);
                // 3. No blackout: the shadowed frame must retain a large lit
                //    remainder (a full-scene blackout is not a shadow).
                const bool notBlackout  = ZHLN::Test::ExpectTrue(darkA < 0.85 * static_cast<double>(mA.total));
                // 4. Flicker guard: shadow region must not pulse frame to frame.
                const bool shadowStable    = ZHLN::Test::ExpectTrue(darkJump < 0.15);
                const bool noShadowFlicker = ZHLN::Test::ExpectTrue(temporalDiff.frac32 < 0.015);
                // 5. Repeat capture must be identical (readback noise control).
                const bool repeatClean     = ZHLN::Test::ExpectTrue(repeatDiff.frac32 == 0.0);

                return shadowExist && lightRestored && meanBrightens && notBlackout && shadowStable && noShadowFlicker && repeatClean;
            }, &validationRaised);

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
        // 5. GPU: ray-traced reflection coverage, flicker & artifacts
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

                // Reflection coverage/artifacts are asserted on the DEFAULT
                // reflection path (SSR). RTR is opt-in; its pixel path is probed
                // separately below as a diagnostic so a broken optional path
                // cannot mask the reference-path verification.
                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                // Polished mirror floor.
                auto mirrorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.03f, .baseColor = {0.85f, 0.85f, 0.88f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMatRes.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *mirrorMatRes
                    }
                );

                // Bright emissive red object to mirror. It renders in the upper half
                // of the frame; its mirror image appears in the lower half.
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
                        .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 140.0f,
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

            const auto stable = RunStableScene(*engine, 10, "raytraced_reflection_coverage_and_artifacts", [](ZHLN::Engine& eng) -> bool {
                std::vector<double> reflectionSeries;
                std::vector<double> saturationSeries;
                std::vector<double> isolatedSeries;
                for (uint32_t f = 0; f < 4; ++f) {
                    TickFrames(eng, 1);
                    const RgbImage frame = Capture(eng, "headless_lighting_rt_reflect_f" + std::to_string(f) + ".ppm");
                    auto checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    // Lower half only: excludes the physical object, so every red
                    // pixel here comes from the polished floor's reflection.
                    const FrameMetrics m = MeasureImage(frame, 0.5);
                    reflectionSeries.push_back(static_cast<double>(m.red));
                    saturationSeries.push_back(static_cast<double>(m.saturated));
                    isolatedSeries.push_back(m.red > 0 ? static_cast<double>(m.redIsolated) / static_cast<double>(m.red) : 0.0);
                }

                const double   meanReflection = Mean(reflectionSeries);
                const double   reflectionCV   = CoefficientOfVariation(reflectionSeries);
                const double   saturationCV   = CoefficientOfVariation(saturationSeries);
                const double   meanSaturation = Mean(saturationSeries);
                const double   isolatedRatio  = Mean(isolatedSeries);
                const uint32_t lowerHalfPixels = static_cast<uint32_t>(640 * 480 / 2);

                ZHLN::Println(
                    "    [INFO] reflection: red={:.0f} (cv {:.4f}), isolated-red ratio={:.4f}, satur={:.0f} (cv {:.4f}), lower-half px={}",
                    meanReflection, reflectionCV, isolatedRatio, meanSaturation, saturationCV, lowerHalfPixels
                );

                // The mirror floor itself is intentionally dark (metallic=1 with
                // only a dark sky to reflect), so overall darkness is not an
                // artifact. The artifact guards are: reflection present, no
                // blowout, no speckled / isolated red debris, and stability. A
                // modest sun glint is legitimate, so blowout is 4% and the
                // saturated-count CV only bites once the region is meaningful.
                const bool reflectionPresent = ZHLN::Test::ExpectTrue(meanReflection > 24.0);
                const bool reflectionStable  = ZHLN::Test::ExpectTrue(reflectionCV < 0.15);
                const bool noBlowout         = ZHLN::Test::ExpectTrue(meanSaturation < 0.04 * static_cast<double>(lowerHalfPixels));
                const bool noRayDebris       = ZHLN::Test::ExpectTrue(isolatedRatio < 0.35);
                const bool saturationStable  = ZHLN::Test::ExpectTrue(saturationCV < 0.25 || meanSaturation < 100.0);

                if (!reflectionPresent || !reflectionStable || !noBlowout || !noRayDebris || !saturationStable) {
                    return false;
                }
                return true;
            }, &validationRaised);

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

            // --- RTR reflection probe (diagnostic, opt-in path) ---------------
            // Runs OUTSIDE RunStableScene: the opt-in probe must not trigger the
            // device-loss retry loop. The engine handles a loss internally; the
            // probe simply reports what the optional path produced.
            if (engine->GetRenderContext().RayTracingSupported()) {
                auto& reg = engine->GetRegistry();

                auto probePath = [&](int enableSSR, int enableRTR, const std::string& tag) -> double {
                    const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                    if (!settingsEnts.empty()) {
                        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [&](auto& pp) {
                            pp.enableSSR = enableSSR;
                            pp.enableRTR = enableRTR;
                        });
                    }
                    // Deliberately does NOT use TickFrames: a device loss during
                    // the diagnostic is handled by the engine and must not add
                    // assertion failures to the already-verified scenario.
                    for (uint32_t i = 0; i < 8; ++i) {
                        engine->ProcessEvents();
                        engine->Tick(1.0f / 60.0f, ZHLN::GameplayDriver::Cpp);
                    }
                    const RgbImage frame = Capture(*engine, "headless_lighting_rt_reflect_" + tag + ".ppm");
                    if (!frame.Valid()) {
                        return -1.0;
                    }
                    return static_cast<double>(MeasureImage(frame, 0.5).red);
                };

                const double rtrRed = probePath(0, 1, "rtr_only");
                const double ssrRed = probePath(1, 0, "ssr_only");

                ZHLN::Println("    [INFO] RTR probe: RTR red={:.0f} vs SSR red={:.0f}", rtrRed, ssrRed);

                // Soft diagnostic: if the optional RTR pixel path produces no
                // reflection while SSR clearly does, surface it prominently
                // without failing the default-path assertions above.
                if (rtrRed >= 0.0 && rtrRed < 8.0 && ssrRed > 24.0) {
                    ZHLN::Println(
                        "    [WARN] RTR reflection path produced no object reflection on this device "
                        "(red={:.0f}) while SSR sees it (red={:.0f}). RT ray queries still work (see the RT "
                        "shadow test); investigate the RTR reflection pipeline separately.",
                        rtrRed, ssrRed
                    );
                }
            }

            return {};
        }
    };
};

int main(int argc, char** argv) {
    // --convert-ppm FILE...  : convert already-captured PPM frames to PNG
    // without running the suite (handy for attaching the existing debug
    // captures from a prior failing run).
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
