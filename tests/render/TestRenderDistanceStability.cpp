// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRenderDistanceStability.cpp
//
// PBR distance robustness: surfaces rendered across two orders of magnitude
// of viewer distance must shade, stay visible and stay temporally stable.
// "Flicker" at distance is how real engine defects surface:
//
//   * a cascade shadow boundary crossing during motion (shadow popping),
//   * a cluster z-slice boundary dropping a light at depth,
//   * Hi-Z / culling popping far geometry in and out,
//   * TAA / SSR / GI history buffers never converging on a static view,
//   * float precision artifacts (distance attenuation, depth reconstruction).
//
// Scenario layout (camera at the origin, yaw 90 => forward +Z, pitch 0):
//
//   ring:    0     1     2     3     4     5
//   dist:    3     6    12    24    48    96   (metres)
//   hue:   red  green  blue yellow cyan magenta
//
//   Each ring is a box whose world size grows with distance (constant
//   ~20px footprint), fanned laterally in NDC so every ring is visible at
//   once, with alternating metallic/dielectric PBR branches so both BRDF
//   paths are exercised at every depth.
//
// Phases (all inside the device-lost-retry runner):
//   A. Coverage   - every ring renders a measurable pixel signature.
//   B. Stability  - static camera: repeat-capture noise floor, then 6
//                   captured frames; per-ring signature CV and whole-frame
//                   pixel-diff gates (identical philosophy to
//                   TestLightingRayTraced's static flicker scenario).
//   C. Sweep      - the camera dollies laterally (sinusoidal, there and
//                   back), so every ring continuously changes its viewer
//                   distance and crosses shadow-cascade / cluster / culling
//                   boundaries. Any in-frustum ring whose signature
//                   collapses (< 2 px) is a pop. (Pixel wobble during
//                   deliberate motion is not flicker, so only disappearance
//                   is gated here.)
//   D. Parity     - motion stops back at the origin: 8 captured frames must
//                   return to the static noise floor. Catches stale
//                   double-buffered history that survives the sweep.
//
// Diagnostics: every captured frame is written as PPM (engine-native) plus a
// PNG twin; a failing run leaves the whole series on disk for inspection.

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
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
#include <expected>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// ============================================================================
// Test Error Types
// ============================================================================

enum class DistanceStabilityTestError : uint8_t {
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine context for the distance-stability test.")]] = 1,
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is blank or could not be captured.")]],
    DistanceSurfaceMissing[[= ZHLN::Reflect::Description("A PBR surface at some distance never produced a visible signature (culled, unshaded or shadowed out).")]],
    TemporalFlickerDetected[[= ZHLN::Reflect::Description("A static scene with PBR surfaces at varying distances changed more frame-to-frame than the noise floor allows.")]],
    SweepPopDetected[[= ZHLN::Reflect::Description("During the camera sweep an in-frustum PBR surface vanished for a frame (cascade/cluster/culling pop).")]],
    PostSweepParityDetected[[= ZHLN::Reflect::Description("After motion stopped the frames kept changing (stale history / double-buffer state).")]],
    DeviceLostDuringTest[[= ZHLN::Reflect::Description(
        "The Vulkan device was lost repeatedly during the scenario; the engine hot-rebuild recovered, but the GPU was not stable."
    )]],
    ValidationErrorsRaised[[= ZHLN::Reflect::Description("The validation layer reported errors while rendering the distance-stability frames.")]],
};

// ============================================================================
// Scene Layout Constants
// ============================================================================

namespace {

constexpr int      kWidth            = 1280;
constexpr int      kHeight           = 720;
constexpr float    kVerticalFovDeg   = 60.0f;
constexpr uint32_t kRingCount        = 6;
// Viewer distance of each ring, in metres.
constexpr std::array<float, kRingCount> kRingDistances = {3.0f, 6.0f, 12.0f, 24.0f, 48.0f, 96.0f};
// Lateral position of each ring in NDC x ([-1, 1]); fans the rings across the
// screen so none of them occludes another.
constexpr std::array<float, kRingCount> kRingNdcX      = {-0.72f, -0.44f, -0.16f, 0.16f, 0.44f, 0.72f};
// World size per metre of distance => constant projected footprint.
constexpr float    kRingSizeOverDistance = 0.035f;
constexpr float    kMinRingSize          = 0.06f;
// A ring counts as "visible" when at least this many pixels carry its hue.
constexpr uint32_t kMinRingPixels        = 8;
// During the sweep a ring further out than this half-angle (from the camera
// forward axis) is legitimately outside the frustum and not counted.
constexpr float    kSweepConeDeg         = 40.0f;
// Camera height / object height (boxes sit mid-screen).
constexpr float    kEyeHeight            = 1.5f;

// ============================================================================
// Image Helpers (same capture/diagnose pattern as TestLightingRayTraced)
// ============================================================================

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

[[nodiscard]] bool SavePNG(const std::string& path, const RgbImage& img) {
    if (!img.Valid()) {
        return false;
    }
    return stbi_write_png(path.c_str(), img.width, img.height, 3, img.rgb.data(), img.width * 3) != 0;
}

struct FrameDiff {
    uint32_t over12  = 0;
    uint32_t over32  = 0;
    double   meanAbs = 0.0;
    double   frac12  = 0.0;
    double   frac32  = 0.0;
};

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

// ============================================================================
// Hue Signature Classification
// ============================================================================
//
// Each ring owns a unique hue family. After PBR shading + ACES tonemap the
// albedo hue survives on the lit faces (white light sources preserve channel
// ratios until clipping), so dominance-classifying every pixel recovers a
// per-ring pixel count without any projection math. Pure families require one
// channel to dominate; pair families require two channels strong and the
// complementary one weak. Evaluated in priority order => exclusive.

enum class HueClass : uint8_t { Red, Green, Blue, Yellow, Cyan, Magenta, None };

[[nodiscard]] HueClass ClassifyPixel(uint8_t r8, uint8_t g8, uint8_t b8) noexcept {
    const double r = static_cast<double>(r8);
    const double g = static_cast<double>(g8);
    const double b = static_cast<double>(b8);
    if (r < 100.0 && g < 100.0 && b < 100.0) {
        return HueClass::None; // Shadow / background floor.
    }

    if (r >= 100.0 && r >= 1.6 * g && r >= 1.6 * b) {
        return HueClass::Red;
    }
    if (g >= 100.0 && g >= 1.6 * r && g >= 1.6 * b) {
        return HueClass::Green;
    }
    if (b >= 100.0 && b >= 1.6 * r && b >= 1.6 * g) {
        return HueClass::Blue;
    }
    if (r >= 100.0 && g >= 100.0 && r >= 0.55 * g && g >= 0.55 * r && (r + g) >= 2.6 * b) {
        return HueClass::Yellow;
    }
    if (g >= 100.0 && b >= 100.0 && g >= 0.55 * b && b >= 0.55 * g && (g + b) >= 2.6 * r) {
        return HueClass::Cyan;
    }
    if (r >= 100.0 && b >= 100.0 && r >= 0.55 * b && b >= 0.55 * r && (r + b) >= 2.6 * g) {
        return HueClass::Magenta;
    }
    return HueClass::None;
}

[[nodiscard]] uint32_t CountHue(const RgbImage& img, HueClass hue) {
    if (!img.Valid()) {
        return 0;
    }
    uint32_t count = 0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        if (ClassifyPixel(img.rgb[i + 0], img.rgb[i + 1], img.rgb[i + 2]) == hue) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t CountLitPixels(const RgbImage& img, uint8_t threshold = 15) {
    if (!img.Valid()) {
        return 0;
    }
    uint32_t lit = 0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        if (img.rgb[i + 0] > threshold || img.rgb[i + 1] > threshold || img.rgb[i + 2] > threshold) {
            lit++;
        }
    }
    return lit;
}

[[nodiscard]] double MeanLuma(const RgbImage& img) {
    if (!img.Valid()) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        sum += 0.2126 * img.rgb[i + 0] + 0.7152 * img.rgb[i + 1] + 0.0722 * img.rgb[i + 2];
    }
    return sum / static_cast<double>(img.rgb.size() / 3);
}

// ============================================================================
// Engine Harness
// ============================================================================

void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt = 1.0f / 60.0f) {
    for (uint32_t i = 0; i < frames; ++i) {
        engine.ProcessEvents();
        const auto status = engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
        ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
    }
}

/// TAA must be disabled at the component level: RenderSystem re-pushes the
/// camera's AA state into the RenderContext every tick, so a SetAAState call
/// alone gets overwritten, and TAA jitter would dominate the frame-to-frame
/// comparison. (Same rationale as TestLightingRayTraced::DisableTAA.)
void DisableTAA(ZHLN::Engine& engine) {
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

/// Captures through Engine::GetRenderContext() so the call always uses the
/// CURRENT context, never a reference that may dangle after a hot-rebuild.
/// Every capture is exported as PPM (engine-native) plus a PNG twin.
[[nodiscard]] RgbImage Capture(ZHLN::Engine& engine, const std::string& path) {
    if (!engine.GetRenderContext().CaptureScreenshotPPM(path)) {
        return {};
    }
    const RgbImage img = LoadPPM(path);
    if (img.Valid()) {
        (void) SavePNG(PngPathOf(path), img);
    }
    return img;
}

// ============================================================================
// Device-Lost-Aware Scenario Runner
// ============================================================================

enum class StableRunResult : uint8_t { Ok, AssertionsFailed, PersistentDeviceLost };

constexpr uint32_t kMaxDeviceLostRecoveries = 2; // 3 attempts total

template <typename SceneFn>
[[nodiscard]] StableRunResult RunStableScene(ZHLN::Engine& engine, uint32_t warmupFrames, const char* label, SceneFn&& sceneFn, uint32_t* outValidationDelta = nullptr) {
    auto&        ctx         = ZHLN::Test::GetThreadLocalContext();
    const size_t failureMark = ctx.failures.size();

    for (uint32_t attempt = 0; attempt <= kMaxDeviceLostRecoveries; ++attempt) {
        if (attempt > 0) {
            // The previous attempt was aborted by a device loss; its assertion
            // failures describe the dead context, not the retry.
            ctx.failures.resize(failureMark);
            ZHLN::Println("    [WARN] {}: Vulkan device lost; engine hot-rebuilt. Re-warming and retrying (attempt {}/{}).", label, attempt, kMaxDeviceLostRecoveries);
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

// ============================================================================
// Ring Geometry
// ============================================================================

struct RingLayout {
    float distance = 0.0f;
    float x        = 0.0f; // world lateral offset
    float size     = 0.0f; // box edge length
};

/// Horizontal half-tangent of the projection; with a fixed vertical FOV and
/// 16:9 the horizontal FOV is ~91.8 degrees (half-tan ~1.026).
[[nodiscard]] float HorizontalHalfTan() noexcept {
    const float tanV = std::tan(JPH::DegreesToRadians(kVerticalFovDeg) * 0.5f);
    return tanV * (static_cast<float>(kWidth) / static_cast<float>(kHeight));
}

[[nodiscard]] std::array<RingLayout, kRingCount> BuildRingLayout() noexcept {
    const float tanH = HorizontalHalfTan();
    std::array<RingLayout, kRingCount> rings {};
    for (uint32_t i = 0; i < kRingCount; ++i) {
        rings[i].distance = kRingDistances[i];
        rings[i].x        = kRingNdcX[i] * kRingDistances[i] * tanH;
        rings[i].size     = std::max(kMinRingSize, kRingSizeOverDistance * kRingDistances[i]);
    }
    return rings;
}

} // namespace

// ============================================================================
// Test Suite
// ============================================================================

struct DistanceStabilitySuite {
    DistanceStabilitySuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~DistanceStabilitySuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine() -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Distance Stability Test",
                .width          = kWidth,
                .height         = kHeight,
                .vsync          = false,
                .fullscreen     = false,
                .validationMode = ZHLN::ValidationMode::On, // Robustness test: VUIDs ARE failures.
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

    // ------------------------------------------------------------------------
    // The scenario: PBR rings at 3..96 m, static stability, dolly sweep,
    // post-sweep parity.
    // ------------------------------------------------------------------------
    static std::expected<void, ZHLN::Error> pbr_distance_stability() {
        ZHLN::Test::SetTimeout(55);

        auto engine      = CreateTestEngine();
        auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
        if (!checkEngine) {
            return std::unexpected(DistanceStabilityTestError::EngineInitFailed);
        }

        // Scene setup happens BEFORE any Tick, so RenderContext references
        // cannot dangle; the scenario body re-fetches through the engine.
        {
            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Deterministic frame-to-frame comparison needs the per-frame
            // noise sources off (TAA jitter, rotating AO/GI pattern). SSR
            // stays on (shipped default, converges on static views); RTR is
            // opt-in and covered by its own suites.
            DisableTAA(*engine);

            const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            ZHLN::Test::ExpectTrue(!settingsEnts.empty());
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                    pp.fullBright      = 0;
                    pp.ambientExposure = 10.0f;
                    pp.enableSSR       = 1;
                    pp.enableRTR       = 0;
                    pp.giMode          = 0;
                });
            }

            // Ground plane gives the shadow catch + distance reference.
            ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 220.0f, {0.55f, 0.55f, 0.58f, 1.0f}, ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false}
            );

            // One material per ring: alternating dielectric / metallic branches
            // of the PBR model, all hue-distinct after shading.
            struct RingMaterial {
                float        baseColor[4];
                float        metallic;
                float        roughness;
                HueClass     hue;
                const char*  name;
            };
            static constexpr std::array<RingMaterial, kRingCount> kMaterials = {{
                {{0.90f, 0.08f, 0.06f, 1.0f}, 0.0f, 0.65f, HueClass::Red,     "red@3m"},
                {{0.10f, 0.85f, 0.08f, 1.0f}, 1.0f, 0.25f, HueClass::Green,   "green@6m"},
                {{0.10f, 0.12f, 0.90f, 1.0f}, 0.0f, 0.70f, HueClass::Blue,    "blue@12m"},
                {{0.95f, 0.90f, 0.12f, 1.0f}, 1.0f, 0.35f, HueClass::Yellow,  "yellow@24m"},
                {{0.10f, 0.90f, 0.95f, 1.0f}, 0.0f, 0.60f, HueClass::Cyan,    "cyan@48m"},
                {{0.90f, 0.10f, 0.90f, 1.0f}, 1.0f, 0.30f, HueClass::Magenta, "magenta@96m"},
            }};

            const auto rings = BuildRingLayout();
            for (uint32_t i = 0; i < kRingCount; ++i) {
                const auto mat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc,
                    ZHLN::CreativeWorksFactory::MaterialDesc {
                        .metallic = kMaterials[i].metallic, .roughness = kMaterials[i].roughness,
                        .baseColor = {kMaterials[i].baseColor[0], kMaterials[i].baseColor[1], kMaterials[i].baseColor[2], 1.0f}
                    }
                );
                auto checkMat = ZHLN::Test::AssertTrue(mat.has_value());
                if (!checkMat) {
                    return std::unexpected(DistanceStabilityTestError::EngineInitFailed);
                }

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(rings[i].size, rings[i].size, rings[i].size),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position        = JPH::RVec3(static_cast<double>(rings[i].x), kEyeHeight, static_cast<double>(rings[i].distance)),
                        .createPhysics   = false,
                        .materialOverride = *mat
                    }
                );
            }

            // Sun (cascade shadows) + two punctuals anchored at mid/far depth
            // so the cluster z-slices at depth carry light.
            const ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 60.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({50.0f, 0.0f, 0.0f})
                },
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Sun,
                    .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                    .intensity = 220.0f,
                    .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                }
            );

            auto addPointLight = [&](const JPH::Vec3& pos, const JPH::Vec3& color, float intensity, float range) {
                const ZHLN::Entity e = reg.Create();
                reg.Add(e, ZHLN::Components::TransformComponent {.position = pos}, ZHLN::Components::LightComponent {.type = ZHLN::LightType::Point, .color = color, .intensity = intensity, .range = range});
            };
            addPointLight(JPH::Vec3(rings[3].x, 4.0f, rings[3].distance), JPH::Vec3(1.0f, 0.9f, 0.7f), 1200.0f, 60.0f);
            addPointLight(JPH::Vec3(rings[5].x, 5.0f, rings[5].distance - 6.0f), JPH::Vec3(0.7f, 0.85f, 1.0f), 2000.0f, 80.0f);

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, kEyeHeight, 0.0f);
            cam.yaw      = 90.0f; // forward = +Z
            cam.pitch    = 0.0f;
            cam.fov      = kVerticalFovDeg;
        }

        uint32_t validationRaised = 0;

        const auto stable = RunStableScene(
            *engine, 20, "pbr_distance_stability",
            [](ZHLN::Engine& eng) -> bool {
                const auto rings = BuildRingLayout();
                const float tanH = HorizontalHalfTan();

                // ---------------- Phase A: coverage at every distance -------
                TickFrames(eng, 1);
                const RgbImage cover = Capture(eng, "headless_distance_coverage.ppm");
                if (!ZHLN::Test::ExpectTrue(cover.Valid())) {
                    return false;
                }
                const bool notBlank = ZHLN::Test::ExpectTrue(MeanLuma(cover) > 1.0 && CountLitPixels(cover) > 500);
                if (!notBlank) {
                    return false;
                }

                std::array<uint32_t, kRingCount> coverCounts {};
                bool allRingsVisible = true;
                for (uint32_t i = 0; i < kRingCount; ++i) {
                    coverCounts[i] = CountHue(cover, static_cast<HueClass>(i));
                    ZHLN::Println("    [INFO] ring {} ({}): {} signature px", i, kRingDistances[i], coverCounts[i]);
                    if (!ZHLN::Test::ExpectTrue(coverCounts[i] >= kMinRingPixels)) {
                        allRingsVisible = false;
                    }
                }
                if (!allRingsVisible) {
                    return false;
                }

                // ---------------- Phase B: static temporal stability --------
                // Repeat-capture control: two captures of the SAME frame give
                // the readback noise floor.
                const RgbImage  repeatA    = Capture(eng, "headless_distance_static_r0.ppm");
                const RgbImage  repeatB    = Capture(eng, "headless_distance_static_r1.ppm");
                const FrameDiff repeatDiff = CompareFrames(repeatA, repeatB);
                if (!ZHLN::Test::ExpectTrue(repeatDiff.meanAbs < 0.5)) {
                    return false;
                }

                constexpr uint32_t             kStaticFrames = 6;
                std::array<std::vector<double>, kRingCount> ringSeries;
                std::vector<double>               litSeries;
                std::vector<double>               lumaSeries;
                std::vector<FrameDiff>            temporalDiffs;

                RgbImage prev;
                for (uint32_t f = 0; f < kStaticFrames; ++f) {
                    TickFrames(eng, 1);
                    const RgbImage frame = Capture(eng, "headless_distance_static_f" + std::to_string(f) + ".ppm");
                    if (!ZHLN::Test::AssertTrue(frame.Valid())) {
                        return false;
                    }

                    litSeries.push_back(static_cast<double>(CountLitPixels(frame)));
                    lumaSeries.push_back(MeanLuma(frame));
                    for (uint32_t i = 0; i < kRingCount; ++i) {
                        ringSeries[i].push_back(static_cast<double>(CountHue(frame, static_cast<HueClass>(i))));
                    }
                    if (prev.Valid()) {
                        temporalDiffs.push_back(CompareFrames(prev, frame));
                    }
                    prev = frame;
                }

                double worstRingCV  = 0.0;
                uint32_t worstRing  = 0;
                for (uint32_t i = 0; i < kRingCount; ++i) {
                    const double mean = Mean(ringSeries[i]);
                    if (mean >= 4.0) { // tiny counts have quantized CV; floor guards
                        const double cv = CoefficientOfVariation(ringSeries[i]);
                        if (cv > worstRingCV) {
                            worstRingCV = cv;
                            worstRing   = i;
                        }
                    }
                    if (!ZHLN::Test::ExpectTrue(Mean(ringSeries[i]) >= 1.0)) {
                        return false; // a ring dropped out entirely mid-run
                    }
                }

                double worstFrac32 = 0.0;
                for (const auto& d: temporalDiffs) {
                    worstFrac32 = std::max(worstFrac32, d.frac32);
                }
                const double litCV  = CoefficientOfVariation(litSeries);
                const double lumaCV = CoefficientOfVariation(lumaSeries);

                ZHLN::Println(
                    "    [INFO] static: worst ring CV {:.4f} (ring {}), lit CV {:.5f}, luma CV {:.5f}, |d|>32 frac {:.6f}, repeat mean|d| {:.5f}",
                    worstRingCV, worstRing, litCV, lumaCV, worstFrac32, repeatDiff.meanAbs
                );

                // Thresholds mirror the lighting suite's static flicker gate:
                // generous for dither/ACES rounding, but a light/cluster/cull
                // pop on any ring moves far more.
                const bool stableRings = ZHLN::Test::ExpectTrue(worstRingCV < 0.30);
                const bool stableLit   = ZHLN::Test::ExpectTrue(litCV < 0.03);
                const bool stableLuma  = ZHLN::Test::ExpectTrue(lumaCV < 0.01);
                const bool noPixelPop  = ZHLN::Test::ExpectTrue(worstFrac32 < 0.01);
                if (!stableRings || !stableLit || !stableLuma || !noPixelPop) {
                    return false;
                }

                // ---------------- Phase C: dolly sweep ----------------------
                // Sinusoidal lateral dolly (there and back to the origin):
                // every ring's viewer distance and shadow-cascade occupancy
                // changes continuously. Only DISAPPEARANCE is gated during
                // deliberate motion (specular sweeps legitimately wobble).
                constexpr uint32_t kSweepFrames   = 80;
                constexpr float    kSweepAmplitude = 30.0f;
                constexpr uint32_t kSweepSample   = 8;

                auto& cam = eng.GetCamera();
                bool  popped = false;
                for (uint32_t f = 0; f < kSweepFrames; ++f) {
                    cam.position = JPH::Vec3(kSweepAmplitude * std::sin(2.0f * std::numbers::pi_v<float> * static_cast<float>(f) / static_cast<float>(kSweepFrames)), kEyeHeight, 0.0f);
                    TickFrames(eng, 1);

                    if (f % kSweepSample != 0) {
                        continue;
                    }
                    const RgbImage frame = Capture(eng, "headless_distance_sweep_s" + std::to_string(f) + ".ppm");
                    if (!ZHLN::Test::AssertTrue(frame.Valid())) {
                        return false;
                    }

                    const float camX = cam.position.GetX();
                    for (uint32_t i = 0; i < kRingCount; ++i) {
                        // Rings are at z = distance > 0, camera at z = 0, so the
                        // view-space depth is the ring distance itself.
                        const float angleDeg = JPH::RadiansToDegrees(std::atan2(rings[i].x - camX, rings[i].distance));
                        if (std::abs(angleDeg) > kSweepConeDeg) {
                            continue; // legitimately outside the frustum
                        }
                        const uint32_t count = CountHue(frame, static_cast<HueClass>(i));
                        if (!ZHLN::Test::ExpectTrue(count >= 2)) {
                            ZHLN::Println("    [FAIL] sweep frame {}: ring {} ({} m, {:.1f} deg) collapsed to {} px", f, i, kRingDistances[i], angleDeg, count);
                            popped = true;
                        }
                    }
                }
                if (popped) {
                    return false;
                }

                // ---------------- Phase D: post-sweep parity ---------------
                // The sweep ended back at x = 0 (sin returns to 0): the static
                // view must return to Phase B's noise floor. Lingering change
                // means stale history / double-buffered state survived motion.
                constexpr uint32_t kParityFrames = 8;
                RgbImage           parityPrev;
                double             parityWorstFrac32 = 0.0;
                std::array<std::vector<double>, kRingCount> paritySeries;
                for (uint32_t f = 0; f < kParityFrames; ++f) {
                    TickFrames(eng, 1);
                    const RgbImage frame = Capture(eng, "headless_distance_parity_f" + std::to_string(f) + ".ppm");
                    if (!ZHLN::Test::AssertTrue(frame.Valid())) {
                        return false;
                    }
                    for (uint32_t i = 0; i < kRingCount; ++i) {
                        paritySeries[i].push_back(static_cast<double>(CountHue(frame, static_cast<HueClass>(i))));
                    }
                    if (parityPrev.Valid()) {
                        parityWorstFrac32 = std::max(parityWorstFrac32, CompareFrames(parityPrev, frame).frac32);
                    }
                    parityPrev = frame;
                }

                double parityWorstRingCV = 0.0;
                for (uint32_t i = 0; i < kRingCount; ++i) {
                    const double mean = Mean(paritySeries[i]);
                    if (mean >= 4.0) {
                        parityWorstRingCV = std::max(parityWorstRingCV, CoefficientOfVariation(paritySeries[i]));
                    }
                }

                ZHLN::Println("    [INFO] parity: worst ring CV {:.4f}, |d|>32 frac {:.6f}", parityWorstRingCV, parityWorstFrac32);

                const bool parityStable = ZHLN::Test::ExpectTrue(parityWorstRingCV < 0.30);
                const bool parityPixels = ZHLN::Test::ExpectTrue(parityWorstFrac32 < 0.01);
                return parityStable && parityPixels;
            },
            &validationRaised
        );

        if (validationRaised != 0) {
            ZHLN::Test::ExpectEq(validationRaised, 0u);
            return std::unexpected(DistanceStabilityTestError::ValidationErrorsRaised);
        }
        if (stable == StableRunResult::AssertionsFailed) {
            // Attribute by phase: the diagnostics above print which gate died;
            // the captures on disk show the offending frames.
            return std::unexpected(DistanceStabilityTestError::TemporalFlickerDetected);
        }
        if (stable != StableRunResult::Ok) {
            return std::unexpected(DistanceStabilityTestError::DeviceLostDuringTest);
        }

        ZHLN::Println("    [ PASS ] PBR surfaces stable at 3 m .. 96 m (static + sweep + parity)");
        return {};
    }

    struct Tests {
        auto pbr_surfaces_distance_stability() {
            return pbr_distance_stability();
        }
    };
};

// ============================================================================
// Main Execution Entry Point
// ============================================================================

auto main(int argc, char** argv) -> int {
    // --convert-ppm FILE... : convert already-captured PPM frames to PNG
    // without re-running the suite (attach diagnostics from a failing run).
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

    return ZHLN::Test::Runner::Run<DistanceStabilitySuite>();
}
