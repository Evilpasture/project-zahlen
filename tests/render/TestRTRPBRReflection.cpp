// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Public-API GPU suite: ray-traced reflection *colour* must follow PBR.
//
// The reflection pass (resources/shaders/reflection.slang) only traces when
// roughness <= 0.4, then multiplies the hit colour by FssEss where
//   F0 = lerp(0.04, albedo, metallic)
// so a gold metal must yellow-tint a white source, a dielectric of the same
// albedo must stay much dimmer, and raising roughness past the threshold
// must kill the traced signature. All of that is judged from captured PPM
// pixels — no src/ includes.

#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
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
#include <string_view>
#include <vector>

enum class RTRPBRError : uint8_t {
    Success = 0,
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine for RTR PBR colour test.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is blank or could not be captured.")]],
    MaterialCreationFailed[[= ZHLN::Reflect::Description("CreativeWorksFactory::CreateMaterial failed.")]],
    ReflectionColorMismatch[[= ZHLN::Reflect::Description("Ray-traced reflection colour does not follow the surface PBR values.")]],
};

namespace {

struct RgbImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 && rgb.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    }
};

struct NormRect {
    double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;
};

struct RegionStats {
    uint32_t pixels   = 0;
    double   meanR    = 0.0;
    double   meanG    = 0.0;
    double   meanB    = 0.0;
    double   meanL    = 0.0;
    double   maxL     = 0.0;
    uint32_t redDom   = 0;
    uint32_t greenDom = 0;
    uint32_t yellow   = 0; // gold-like: R&G high, B suppressed
    uint32_t cyan     = 0;
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
    if (header != "P6" || img.width <= 0 || img.height <= 0) {
        return {};
    }
    img.rgb.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3u);
    ppm.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    if (ppm.gcount() != static_cast<std::streamsize>(img.rgb.size())) {
        return {};
    }
    return img;
}

[[nodiscard]] double Luma(double r, double g, double b) noexcept {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

[[nodiscard]] RegionStats MeasureRegion(const RgbImage& img, const NormRect& nr) {
    RegionStats s;
    if (!img.Valid()) {
        return s;
    }
    const int x0 = std::clamp(static_cast<int>(nr.x0 * img.width), 0, img.width - 1);
    const int y0 = std::clamp(static_cast<int>(nr.y0 * img.height), 0, img.height - 1);
    const int x1 = std::clamp(static_cast<int>(nr.x1 * img.width), x0 + 1, img.width);
    const int y1 = std::clamp(static_cast<int>(nr.y1 * img.height), y0 + 1, img.height);

    double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumL = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
            const double r = img.rgb[i + 0];
            const double g = img.rgb[i + 1];
            const double b = img.rgb[i + 2];
            const double l = Luma(r, g, b);
            sumR += r;
            sumG += g;
            sumB += b;
            sumL += l;
            s.maxL = std::max(s.maxL, l);
            ++s.pixels;
            if (r > 70.0 && r > 1.45 * g && r > 1.45 * b) {
                ++s.redDom;
            }
            if (g > 70.0 && g > 1.45 * r && g > 1.45 * b) {
                ++s.greenDom;
            }
            if (r > 55.0 && g > 40.0 && b < 0.62 * std::min(r, g) && r >= g * 0.85) {
                ++s.yellow;
            }
            if (g > 55.0 && b > 55.0 && r < 0.70 * std::min(g, b)) {
                ++s.cyan;
            }
        }
    }
    if (s.pixels > 0) {
        const double n = static_cast<double>(s.pixels);
        s.meanR        = sumR / n;
        s.meanG        = sumG / n;
        s.meanB        = sumB / n;
        s.meanL        = sumL / n;
    }
    return s;
}

[[nodiscard]] double BlueRatio(const RegionStats& s) noexcept {
    const double denom = std::max(s.meanR, 1.0);
    return s.meanB / denom;
}

[[nodiscard]] double GreenRatio(const RegionStats& s) noexcept {
    const double denom = std::max(s.meanR, 1.0);
    return s.meanG / denom;
}

void LogRegion(std::string_view name, const RegionStats& s) {
    ZHLN::Println(
        "    [INFO] {} px={} meanRGB=({:.1f},{:.1f},{:.1f}) L={:.1f} maxL={:.1f} red={} green={} yellow={} cyan={} B/R={:.3f} G/R={:.3f}", name, s.pixels,
        s.meanR, s.meanG, s.meanB, s.meanL, s.maxL, s.redDom, s.greenDom, s.yellow, s.cyan, BlueRatio(s), GreenRatio(s)
    );
}

} // namespace

struct RTRPBRReflectionTestSuite {
    RTRPBRReflectionTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~RTRPBRReflectionTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 800, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);
        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless RTR PBR Colour",
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

    static void DisableTAAAndFreeCam(ZHLN::Engine& engine) {
        auto& reg = engine.GetRegistry();
        for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
            reg.Remove<ZHLN::Components::FreeCamTagComponent>(e);
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

    static void ConfigureDarkRTR(ZHLN::Engine& engine) {
        auto&      reg      = engine.GetRegistry();
        const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings.empty()) {
            return;
        }
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) {
            pp.fullBright        = 0;
            pp.ambientExposure   = 4.0f;
            pp.vignetteIntensity = 0.0f;
            pp.enableSSR         = 0;
            pp.enableRTR         = 1;
            pp.giMode            = 0;
            pp.giIntensity       = 0.0f;
            pp.skyZenith         = JPH::Vec4(0.001f, 0.002f, 0.006f, 1.0f);
            pp.skyHorizon        = JPH::Vec4(0.004f, 0.006f, 0.012f, 1.0f);
            pp.skyGround         = JPH::Vec4(0.001f, 0.001f, 0.002f, 1.0f);
        });
    }

    static void PlaceSunAndCamera(ZHLN::Engine& engine) {
        auto&              reg    = engine.GetRegistry();
        const ZHLN::Entity sunEnt = reg.Create();
        reg.Add(
            sunEnt,
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 18.0f, 14.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({35.0f, 0.0f, 0.0f})},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 90.0f, .direction = JPH::Vec3(0.0f, 0.70f, 0.71f).Normalized()
            }
        );

        auto& cam    = engine.GetCamera();
        cam.position = JPH::Vec3(0.0f, 5.2f, 10.5f);
        cam.yaw      = -90.0f;
        cam.pitch    = -28.0f;
        cam.fov      = 50.0f;
    }

    static auto MakeMat(ZHLN::RenderContext& rc, float metallic, float roughness, std::array<float, 4> base, std::array<float, 4> emissive = {0, 0, 0, 1})
        -> std::expected<ZHLN::Material, ZHLN::Error> {
        return ZHLN::CreativeWorksFactory::CreateMaterial(
            rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = metallic, .roughness = roughness, .baseColor = base, .emissive = emissive}
        );
    }

    static ZHLN::Entity SpawnTile(ZHLN::Engine& engine, float x, const ZHLN::Material& mat) {
        return ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(1.05f, 0.03f, 1.70f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(x, 0.03, 0.0), .createPhysics = false, .materialOverride = mat}
        );
    }

    static ZHLN::Entity SpawnEmitter(ZHLN::Engine& engine, float x, const ZHLN::Material& mat) {
        return ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(0.45f, 0.45f, 0.45f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(x, 2.45, 0.15), .createPhysics = false, .materialOverride = mat}
        );
    }

    static void TickFrames(ZHLN::Engine& engine, uint32_t frames) {
        constexpr float dt = 1.0f / 60.0f;
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
        return LoadPPM(path);
    }

    [[nodiscard]] static bool RequireRT(ZHLN::Engine& engine) {
        if (engine.GetRenderContext().RayTracingSupported()) {
            return true;
        }
        ZHLN::Println("    [SKIP] Device has no raytracing; RTR PBR colour checks are not applicable.");
        return false;
    }

    // Lower-third bands for three floor tiles (left / mid / right). y starts
    // below the hanging emitter so we only score the reflection, not the source.
    static constexpr NormRect kLeftTile {.x0 = 0.06, .y0 = 0.58, .x1 = 0.32, .y1 = 0.92};
    static constexpr NormRect kMidTile {.x0 = 0.38, .y0 = 0.58, .x1 = 0.62, .y1 = 0.92};
    static constexpr NormRect kRightTile {.x0 = 0.68, .y0 = 0.58, .x1 = 0.94, .y1 = 0.92};
    static constexpr NormRect kUpperSource {.x0 = 0.30, .y0 = 0.08, .x1 = 0.70, .y1 = 0.42};

    struct Tests {
        // ------------------------------------------------------------------
        // White emitter over chrome / gold / dielectric tiles.
        // F0 = lerp(0.04, albedo, metallic): gold must yellow-tint, dielectric
        // must stay far dimmer than chrome, chrome must stay more neutral than gold.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> rtr_metallic_f0_tints_white_source() {
            auto engine = CreateTestEngine();
            if (!engine) {
                return std::unexpected(RTRPBRError::EngineInitFailed);
            }
            if (!RequireRT(*engine)) {
                return {};
            }

            DisableTAAAndFreeCam(*engine);
            ConfigureDarkRTR(*engine);
            PlaceSunAndCamera(*engine);

            auto& rc        = engine->GetRenderContext();
            auto  chromeRes = MakeMat(rc, 1.0f, 0.03f, {0.92f, 0.92f, 0.94f, 1.0f});
            auto  goldRes   = MakeMat(rc, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
            auto  dielRes   = MakeMat(rc, 0.0f, 0.03f, {0.92f, 0.92f, 0.94f, 1.0f});
            auto  whiteEm   = MakeMat(rc, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {6.0f, 6.0f, 6.0f, 1.0f});
            auto  darkRes   = MakeMat(rc, 0.0f, 0.95f, {0.04f, 0.04f, 0.045f, 1.0f});
            if (!chromeRes || !goldRes || !dielRes || !whiteEm || !darkRes) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 80.0f, {0.04f, 0.04f, 0.045f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, -0.12, 0.0), .createPhysics = false, .materialOverride = *darkRes}
            );
            SpawnTile(*engine, -2.45f, *chromeRes);
            SpawnTile(*engine, 0.00f, *goldRes);
            SpawnTile(*engine, 2.45f, *dielRes);
            SpawnEmitter(*engine, 0.0f, *whiteEm);

            TickFrames(*engine, 14);
            const RgbImage img = Capture(*engine, "headless_rtr_pbr_f0_tint.ppm");
            if (!img.Valid()) {
                return std::unexpected(RTRPBRError::RenderOutputBlank);
            }

            const RegionStats chrome = MeasureRegion(img, kLeftTile);
            const RegionStats gold   = MeasureRegion(img, kMidTile);
            const RegionStats diel   = MeasureRegion(img, kRightTile);
            const RegionStats src    = MeasureRegion(img, kUpperSource);
            LogRegion("chrome metal", chrome);
            LogRegion("gold metal", gold);
            LogRegion("white dielectric", diel);
            LogRegion("white emitter", src);

            const bool sourceLit = ZHLN::Test::ExpectTrue(src.meanL > 18.0);
            const bool chromeLit = ZHLN::Test::ExpectTrue(chrome.meanL > 8.0);
            const bool goldYellow =
                ZHLN::Test::ExpectTrue(gold.yellow > 40u || (gold.meanR > 28.0 && gold.meanG > 18.0 && BlueRatio(gold) + 0.08 < BlueRatio(chrome)));
            const bool goldBluerLess = ZHLN::Test::ExpectTrue(BlueRatio(gold) + 0.06 < BlueRatio(chrome));
            const bool metalBrighter = ZHLN::Test::ExpectTrue(chrome.meanL > diel.meanL * 1.35 + 2.0);
            const bool goldNotBlue   = ZHLN::Test::ExpectTrue(gold.meanB + 6.0 < gold.meanR);

            if (!sourceLit || !chromeLit || !goldYellow || !goldBluerLess || !metalBrighter || !goldNotBlue) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Metallic F0 tints the traced white source; dielectric stays dimmer.");
            return {};
        }

        // ------------------------------------------------------------------
        // Same chrome albedo, three roughnesses. Shader fades RTR by
        // (1 - 2*roughness) and disables the trace entirely above 0.4.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> rtr_roughness_monotone_reflection_energy() {
            auto engine = CreateTestEngine();
            if (!engine) {
                return std::unexpected(RTRPBRError::EngineInitFailed);
            }
            if (!RequireRT(*engine)) {
                return {};
            }

            DisableTAAAndFreeCam(*engine);
            ConfigureDarkRTR(*engine);
            PlaceSunAndCamera(*engine);

            auto& rc      = engine->GetRenderContext();
            auto  smooth  = MakeMat(rc, 1.0f, 0.02f, {0.90f, 0.90f, 0.92f, 1.0f});
            auto  mid     = MakeMat(rc, 1.0f, 0.22f, {0.90f, 0.90f, 0.92f, 1.0f});
            auto  rough   = MakeMat(rc, 1.0f, 0.70f, {0.90f, 0.90f, 0.92f, 1.0f});
            auto  redEm   = MakeMat(rc, 0.0f, 0.40f, {1.0f, 0.05f, 0.04f, 1.0f}, {8.0f, 0.0f, 0.0f, 1.0f});
            auto  darkRes = MakeMat(rc, 0.0f, 0.95f, {0.04f, 0.04f, 0.045f, 1.0f});
            if (!smooth || !mid || !rough || !redEm || !darkRes) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 80.0f, {0.04f, 0.04f, 0.045f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, -0.12, 0.0), .createPhysics = false, .materialOverride = *darkRes}
            );
            SpawnTile(*engine, -2.45f, *smooth);
            SpawnTile(*engine, 0.00f, *mid);
            SpawnTile(*engine, 2.45f, *rough);
            SpawnEmitter(*engine, 0.0f, *redEm);

            TickFrames(*engine, 14);
            const RgbImage img = Capture(*engine, "headless_rtr_pbr_roughness.ppm");
            if (!img.Valid()) {
                return std::unexpected(RTRPBRError::RenderOutputBlank);
            }

            const RegionStats sSmooth = MeasureRegion(img, kLeftTile);
            const RegionStats sMid    = MeasureRegion(img, kMidTile);
            const RegionStats sRough  = MeasureRegion(img, kRightTile);
            LogRegion("roughness 0.02", sSmooth);
            LogRegion("roughness 0.22", sMid);
            LogRegion("roughness 0.70", sRough);

            // Energy and the red-emitter signature must fall as roughness rises.
            const bool monotoneL = ZHLN::Test::ExpectTrue(sSmooth.meanL + 1.5 > sMid.meanL && sMid.meanL + 0.5 > sRough.meanL * 0.85);
            const bool sharpHot  = ZHLN::Test::ExpectTrue(sSmooth.maxL + 4.0 > sRough.maxL);
            const bool rtrOn     = ZHLN::Test::ExpectTrue(sSmooth.redDom > 20u || sSmooth.meanR > sRough.meanR * 1.20 + 3.0);
            const bool rtrOff    = ZHLN::Test::ExpectTrue(sSmooth.redDom + 8u > sRough.redDom && sSmooth.meanL > sRough.meanL + 2.0);

            if (!monotoneL || !sharpHot || !rtrOn || !rtrOff) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Reflection energy falls with roughness; r=0.70 drops the traced red.");
            return {};
        }

        // ------------------------------------------------------------------
        // Chrome tiles under a red emitter (left) and a green emitter (right).
        // Untinted metal (F0 ≈ albedo ≈ white) must preserve the source hue.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> rtr_chrome_preserves_emitter_hue() {
            auto engine = CreateTestEngine();
            if (!engine) {
                return std::unexpected(RTRPBRError::EngineInitFailed);
            }
            if (!RequireRT(*engine)) {
                return {};
            }

            DisableTAAAndFreeCam(*engine);
            ConfigureDarkRTR(*engine);
            PlaceSunAndCamera(*engine);

            auto& rc      = engine->GetRenderContext();
            auto  chrome  = MakeMat(rc, 1.0f, 0.025f, {0.94f, 0.94f, 0.96f, 1.0f});
            auto  redEm   = MakeMat(rc, 0.0f, 0.40f, {1.0f, 0.04f, 0.03f, 1.0f}, {8.0f, 0.0f, 0.0f, 1.0f});
            auto  greenEm = MakeMat(rc, 0.0f, 0.40f, {0.04f, 1.0f, 0.04f, 1.0f}, {0.0f, 8.0f, 0.0f, 1.0f});
            auto  darkRes = MakeMat(rc, 0.0f, 0.95f, {0.04f, 0.04f, 0.045f, 1.0f});
            if (!chrome || !redEm || !greenEm || !darkRes) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 80.0f, {0.04f, 0.04f, 0.045f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, -0.12, 0.0), .createPhysics = false, .materialOverride = *darkRes}
            );
            SpawnTile(*engine, -2.20f, *chrome);
            SpawnTile(*engine, 2.20f, *chrome);
            SpawnEmitter(*engine, -2.20f, *redEm);
            SpawnEmitter(*engine, 2.20f, *greenEm);

            TickFrames(*engine, 14);
            const RgbImage img = Capture(*engine, "headless_rtr_pbr_emitter_hue.ppm");
            if (!img.Valid()) {
                return std::unexpected(RTRPBRError::RenderOutputBlank);
            }

            const RegionStats left  = MeasureRegion(img, kLeftTile);
            const RegionStats right = MeasureRegion(img, kRightTile);
            LogRegion("chrome under red", left);
            LogRegion("chrome under green", right);

            const bool leftRedder   = ZHLN::Test::ExpectTrue(left.meanR > left.meanG + 4.0 && left.meanR > left.meanB + 4.0);
            const bool rightGreener = ZHLN::Test::ExpectTrue(right.meanG > right.meanR + 4.0 && right.meanG > right.meanB + 2.0);
            const bool splitHue     = ZHLN::Test::ExpectTrue(left.meanR > right.meanR + 6.0 && right.meanG > left.meanG + 6.0);
            const bool counts       = ZHLN::Test::ExpectTrue(left.redDom + 10u > right.redDom && right.greenDom + 10u > left.greenDom);

            if (!leftRedder || !rightGreener || !splitHue || !counts) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Chrome RTR preserves emitter hue (red left / green right).");
            return {};
        }

        // ------------------------------------------------------------------
        // Same gold-albedo tile, live PBRComponent patches:
        //   metal + smooth  → strong yellow reflection
        //   dielectric      → energy collapse, less gold
        //   metal + rough   → traced signature collapses (roughness > 0.4)
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> rtr_live_pbr_patch_retints_same_tile() {
            auto engine = CreateTestEngine();
            if (!engine) {
                return std::unexpected(RTRPBRError::EngineInitFailed);
            }
            if (!RequireRT(*engine)) {
                return {};
            }

            DisableTAAAndFreeCam(*engine);
            ConfigureDarkRTR(*engine);
            PlaceSunAndCamera(*engine);

            auto& rc      = engine->GetRenderContext();
            auto  gold    = MakeMat(rc, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
            auto  whiteEm = MakeMat(rc, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {6.0f, 6.0f, 6.0f, 1.0f});
            auto  darkRes = MakeMat(rc, 0.0f, 0.95f, {0.04f, 0.04f, 0.045f, 1.0f});
            if (!gold || !whiteEm || !darkRes) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 80.0f, {0.04f, 0.04f, 0.045f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, -0.12, 0.0), .createPhysics = false, .materialOverride = *darkRes}
            );
            const ZHLN::Entity tile = SpawnTile(*engine, 0.0f, *gold);
            SpawnEmitter(*engine, 0.0f, *whiteEm);

            auto& reg         = engine->GetRegistry();
            auto  captureTile = [&](const char* path, float metallic, float roughness) -> RegionStats {
                reg.Patch<ZHLN::Components::PBRComponent>(tile, [&](auto& pbr) {
                    pbr.metallic  = metallic;
                    pbr.roughness = roughness;
                });
                TickFrames(*engine, 8);
                const RgbImage img = Capture(*engine, path);
                return MeasureRegion(img, kMidTile);
            };

            const RegionStats metalSmooth = captureTile("headless_rtr_pbr_patch_metal.ppm", 1.0f, 0.03f);
            const RegionStats dielectric  = captureTile("headless_rtr_pbr_patch_diel.ppm", 0.0f, 0.03f);
            const RegionStats metalRough  = captureTile("headless_rtr_pbr_patch_rough.ppm", 1.0f, 0.75f);
            LogRegion("patch metal r=0.03", metalSmooth);
            LogRegion("patch dielectric r=0.03", dielectric);
            LogRegion("patch metal r=0.75", metalRough);

            const bool metalEnergy = ZHLN::Test::ExpectTrue(metalSmooth.meanL > dielectric.meanL * 1.30 + 2.0);
            const bool metalYellower =
                ZHLN::Test::ExpectTrue(BlueRatio(metalSmooth) + 0.05 < BlueRatio(dielectric) || metalSmooth.yellow > dielectric.yellow + 15u);
            const bool roughKills    = ZHLN::Test::ExpectTrue(metalSmooth.meanL > metalRough.meanL + 2.0 && metalSmooth.yellow + 8u > metalRough.yellow);
            const bool roughLessGold = ZHLN::Test::ExpectTrue(metalSmooth.maxL + 2.0 >= metalRough.maxL);

            if (!metalEnergy || !metalYellower || !roughKills || !roughLessGold) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Live PBR patches retint / kill the same tile's RTR colour.");
            return {};
        }

        // ------------------------------------------------------------------
        // Copper vs gold vs silver — three metals, one white source.
        // Channel order of the *reflection* must follow F0, not swap.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> rtr_metal_palette_channel_order() {
            auto engine = CreateTestEngine();
            if (!engine) {
                return std::unexpected(RTRPBRError::EngineInitFailed);
            }
            if (!RequireRT(*engine)) {
                return {};
            }

            DisableTAAAndFreeCam(*engine);
            ConfigureDarkRTR(*engine);
            PlaceSunAndCamera(*engine);

            auto& rc      = engine->GetRenderContext();
            auto  silver  = MakeMat(rc, 1.0f, 0.03f, {0.97f, 0.96f, 0.93f, 1.0f});
            auto  gold    = MakeMat(rc, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
            auto  copper  = MakeMat(rc, 1.0f, 0.03f, {0.95f, 0.64f, 0.54f, 1.0f});
            auto  whiteEm = MakeMat(rc, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {6.0f, 6.0f, 6.0f, 1.0f});
            auto  darkRes = MakeMat(rc, 0.0f, 0.95f, {0.04f, 0.04f, 0.045f, 1.0f});
            if (!silver || !gold || !copper || !whiteEm || !darkRes) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 80.0f, {0.04f, 0.04f, 0.045f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, -0.12, 0.0), .createPhysics = false, .materialOverride = *darkRes}
            );
            SpawnTile(*engine, -2.45f, *silver);
            SpawnTile(*engine, 0.00f, *gold);
            SpawnTile(*engine, 2.45f, *copper);
            SpawnEmitter(*engine, 0.0f, *whiteEm);

            TickFrames(*engine, 14);
            const RgbImage img = Capture(*engine, "headless_rtr_pbr_metal_palette.ppm");
            if (!img.Valid()) {
                return std::unexpected(RTRPBRError::RenderOutputBlank);
            }

            const RegionStats sil = MeasureRegion(img, kLeftTile);
            const RegionStats au  = MeasureRegion(img, kMidTile);
            const RegionStats cu  = MeasureRegion(img, kRightTile);
            LogRegion("silver", sil);
            LogRegion("gold", au);
            LogRegion("copper", cu);

            // Gold F0.b is tiny; copper F0.b is much higher; silver is near-neutral.
            const bool goldLeastBlue  = ZHLN::Test::ExpectTrue(BlueRatio(au) + 0.04 < BlueRatio(sil) && BlueRatio(au) + 0.03 < BlueRatio(cu));
            const bool goldMoreYellow = ZHLN::Test::ExpectTrue(GreenRatio(au) > GreenRatio(cu) + 0.03 && au.yellow + 5u >= cu.yellow);
            const bool silverNeutral  = ZHLN::Test::ExpectTrue(std::abs(sil.meanR - sil.meanG) < 28.0 && BlueRatio(sil) > 0.45);
            const bool allReflect     = ZHLN::Test::ExpectTrue(sil.meanL > 6.0 && au.meanL > 6.0 && cu.meanL > 6.0);

            if (!goldLeastBlue || !goldMoreYellow || !silverNeutral || !allReflect) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Silver / gold / copper RTR channel order matches F0.");
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<RTRPBRReflectionTestSuite>();
}
