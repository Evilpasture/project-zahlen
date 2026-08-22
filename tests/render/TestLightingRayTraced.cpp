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
//      signature on the ground/box must never vanish or collapse.
//   4. Ray-traced shadows: an occluder between the sun and the ground must
//      carve a real, stable shadow (not a full-scene blackout, not nothing),
//      and removing it must restore a near-uniformly lit floor.
//   5. Ray-traced reflections: a polished plane must mirror a bright
//      emissive object (coverage), must be stable frame to frame (flicker),
//      must not blow out or produce isolated ray-debris speckles, and the
//      RTR path must not degenerate to the IBL fallback when compared with
//      SSR.
//
// Every RTR-specific case degrades to a skip (not a failure) when the device
// has no raytracing support, mirroring how the mesh-shader suite handles
// VK_EXT_mesh_shader.
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
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
    uint32_t lit         = 0; // luma > 96
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

        if (l > 96.0) {
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
    static auto Capture(ZHLN::Engine& engine, const std::string& path) -> RgbImage {
        if (!engine.GetRenderContext().CaptureScreenshotPPM(path)) {
            return {};
        }
        return LoadPPM(path);
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
    static [[nodiscard]] StableRunResult RunStableScene(
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
                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                ZHLN::Test::ExpectTrue(!settingsEnts.empty());
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 10.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 1; // Exercises the RT reflection/shadow path when the device supports it.
                    });
                }

                // Ground + three contrasting surfaces (mirror, diffuse red, rough
                // blue). Note: SpawnParams.roughness/metallic are ignored by the
                // factory spawners -- materials below control the shading.
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.55f, 0.55f, 0.58f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false}
                );

                auto mirrorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.08f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.7f, .baseColor = {0.9f, 0.1f, 0.1f, 1.0f}}
                );
                auto blueMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.1f, 0.2f, 0.9f, 1.0f}}
                );

                auto checkMaterials = ZHLN::Test::AssertTrue(mirrorMatRes && redMatRes && blueMatRes);
                if (!checkMaterials) {
                    return checkMaterials;
                }

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-2.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *mirrorMatRes}
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
                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 1;
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

                std::vector<double>    redCounts;
                std::vector<uint32_t>  redPeaks;
                double                 worstConsecutiveSlump = 0.0;

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

                    if (redCounts.size() > 1 && redCounts[redCounts.size() - 2] > 1.0) {
                        const double slump = std::abs(redCounts.back() - redCounts[redCounts.size() - 2]) / redCounts[redCounts.size() - 2];
                        worstConsecutiveSlump = std::max(worstConsecutiveSlump, slump);
                    }
                }

                const double   minRed    = *std::ranges::min_element(redCounts);
                const double   maxRed    = *std::ranges::max_element(redCounts);
                const uint32_t minPeak   = *std::ranges::min_element(redPeaks);

                ZHLN::Println(
                    "    [INFO] light sweep: red pixels min={:.0f} max={:.0f} across {} samples, red peak floor={}, worst consecutive slump={:.3f}",
                    minRed, maxRed, redCounts.size(), minPeak, worstConsecutiveSlump
                );

                // Invariant 1: the light stays within range and on screen for the
                // whole sweep, so its signature must never collapse to zero.
                const bool neverCulled      = ZHLN::Test::ExpectTrue(minRed > 16.0);
                const bool brightEverywhere = ZHLN::Test::ExpectTrue(minPeak > 60u);
                // Invariant 2: smooth sweep -> smooth patch; a single-frame cull
                // would show up as a sudden 2x+ slump.
                const bool noSuddenSlump = ZHLN::Test::ExpectTrue(worstConsecutiveSlump < 0.75);

                return neverCulled && brightEverywhere && noSuddenSlump;
            }, &validationRaised);

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::LightCullingPopDetected);
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
                //    a substantial dark region and gains lit pixels.
                const bool shadowExist     = ZHLN::Test::ExpectTrue(darkA > darkClear + 1500u);
                const bool lightRestored   = ZHLN::Test::ExpectTrue(litClear > litA + 1500u);
                // 2. No blackout: the scene must not go dark everywhere when the
                //    occluder is present.
                const bool notBlackout     = ZHLN::Test::ExpectTrue(litA > 3000u);
                // 3. No missing shadow: without the occluder, the floor is lit.
                const bool clearNotDark    = ZHLN::Test::ExpectTrue(darkClear < darkA / 3u);
                // 4. Flicker guard: shadow region must not pulse frame to frame.
                const bool shadowStable    = ZHLN::Test::ExpectTrue(darkJump < 0.15);
                const bool noShadowFlicker = ZHLN::Test::ExpectTrue(temporalDiff.frac32 < 0.015);
                // 5. Repeat capture must be identical (readback noise control).
                const bool repeatClean     = ZHLN::Test::ExpectTrue(repeatDiff.frac32 == 0.0);

                return shadowExist && lightRestored && notBlackout && clearNotDark && shadowStable && noShadowFlicker && repeatClean;
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

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 1;
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
                auto& reg = eng.GetRegistry();

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

                // --- RTR vs SSR parity (only when the device has RT) ------------
                // If the RTR path degenerates to the prefiltered IBL fallback the
                // reflected object disappears even though SSR can still see it,
                // which is exactly the "artifacts during reflection" class of bug.
                if (eng.GetRenderContext().RayTracingSupported()) {
                    auto setReflectionPath = [&](int enableSSR, int enableRTR, const std::string& tag) -> double {
                        const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                        if (!settingsEnts.empty()) {
                            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [&](auto& pp) {
                                pp.enableSSR = enableSSR;
                                pp.enableRTR = enableRTR;
                            });
                        }
                        TickFrames(eng, 8);
                        const RgbImage frame = Capture(eng, "headless_lighting_rt_reflect_" + tag + ".ppm");
                        if (!frame.Valid()) {
                            return -1.0;
                        }
                        return static_cast<double>(MeasureImage(frame, 0.5).red);
                    };

                    const double rtrRed = setReflectionPath(0, 1, "rtr_only");
                    const double ssrRed = setReflectionPath(1, 0, "ssr_only");

                    ZHLN::Println("    [INFO] path parity: RTR red={:.0f}, SSR red={:.0f}", rtrRed, ssrRed);

                    const bool rtrHasReflection = ZHLN::Test::ExpectTrue(rtrRed > 24.0);
                    const bool ssrHasReflection = ZHLN::Test::ExpectTrue(ssrRed > 24.0);
                    const bool rtrNotDegraded   = ZHLN::Test::ExpectTrue(rtrRed >= 0.35 * ssrRed);
                    const bool ssrNotDegraded   = ZHLN::Test::ExpectTrue(ssrRed >= 0.35 * rtrRed);

                    if (!rtrHasReflection || !ssrHasReflection || !rtrNotDegraded || !ssrNotDegraded) {
                        return false;
                    }
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

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<LightingRTTestSuite>();
}
