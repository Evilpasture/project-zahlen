// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Public-API GPU suite: ray-traced reflection *colour* must follow PBR.
//
// The reflection pass only traces when roughness <= 0.4, then multiplies the
// hit colour by FssEss where F0 = lerp(0.04, albedo, metallic). A gold metal
// must yellow-tint a white source, a dielectric of the same albedo must stay
// dimmer, and roughness past the threshold must kill the traced signature.
// Judged from captured PPM pixels — no src/ includes.
//
// Device-lost is a hard failure. The framework already fails the test if
// DeviceLostCount rises; this suite does not AllowDeviceLost and does not
// fall back to SSR. No RT support → skip. A blank capture after RTR is on
// is a failure, not a retry.

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
    EngineInitFailed[[= ZHLN::Reflect::Description<"Failed to initialize headless Engine for RTR PBR colour test.">{}]] = 1,
    RenderOutputBlank[[= ZHLN::Reflect::Description<"Rendered frame is blank or could not be captured.">{}]],
    MaterialCreationFailed[[= ZHLN::Reflect::Description<"CreativeWorksFactory::CreateMaterial failed.">{}]],
    ReflectionColorMismatch[[= ZHLN::Reflect::Description<"Ray-traced reflection colour does not follow the surface PBR values.">{}]],
    DeviceLostDuringTest[[= ZHLN::Reflect::Description<"Vulkan device was lost while ray-traced reflections were enabled.">{}]],
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
    uint32_t yellow   = 0;
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
    return s.meanB / std::max(s.meanR, 1.0);
}

[[nodiscard]] double GreenRatio(const RegionStats& s) noexcept {
    return s.meanG / std::max(s.meanR, 1.0);
}

void LogRegion(std::string_view name, const RegionStats& s) {
    ZHLN::Println(
        "    [INFO] {} px={} meanRGB=({:.1f},{:.1f},{:.1f}) L={:.1f} maxL={:.1f} red={} green={} yellow={} B/R={:.3f} G/R={:.3f}", name, s.pixels, s.meanR,
        s.meanG, s.meanB, s.meanL, s.maxL, s.redDom, s.greenDom, s.yellow, BlueRatio(s), GreenRatio(s)
    );
}

[[nodiscard]] bool ImageIsBlank(const RgbImage& img) {
    if (!img.Valid()) {
        return true;
    }
    // Rec.709 luma treats a saturated red emitter as dark (0.21·R). A
    // working RTR frame must have *some* channel above the ACES floor.
    uint8_t maxC = 0;
    for (const uint8_t c: img.rgb) {
        maxC = std::max(maxC, c);
    }
    return maxC < 6u;
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

    static auto CreateTestEngine() -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);
        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless RTR PBR Colour",
                .width          = 640,
                .height         = 480,
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

    static void SetReflectionFlags(ZHLN::Engine& engine, int enableSSR, int enableRTR) {
        auto&      reg      = engine.GetRegistry();
        const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings.empty()) {
            return;
        }
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [enableSSR, enableRTR](auto& pp) {
            pp.fullBright        = 0;
            pp.ambientExposure   = 6.0f;
            pp.vignetteIntensity = 0.0f;
            pp.enableSSR         = enableSSR;
            pp.enableRTR         = enableRTR;
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
        // Intensity 0: LightingSystem falls back to a 180-nit default sun if
        // no Sun exists, which floods dielectric albedo and hides F0 tint.
        reg.Add(
            sunEnt,
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 0.0f, .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
            }
        );

        auto& cam    = engine.GetCamera();
        cam.position = JPH::Vec3(0.0f, 5.5f, 11.0f);
        cam.yaw      = -90.0f;
        cam.pitch    = -28.0f;
        cam.fov      = 55.0f;
    }

    static auto MakeMat(ZHLN::Engine& engine, float metallic, float roughness, std::array<float, 4> base, std::array<float, 4> emissive = {0, 0, 0, 1})
        -> std::expected<ZHLN::Material, ZHLN::Error> {
        return ZHLN::CreativeWorksFactory::CreateMaterial(
            engine.GetRenderContext(),
            ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = metallic, .roughness = roughness, .baseColor = base, .emissive = emissive}
        );
    }

    static ZHLN::Entity SpawnMirror(ZHLN::Engine& engine, const ZHLN::Material& mat) {
        return ZHLN::CreativeWorksFactory::CreatePlane(
            engine, 16.0f, {0.85f, 0.85f, 0.88f, 1.0f},
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = mat}
        );
    }

    static ZHLN::Entity SpawnEmitter(ZHLN::Engine& engine, const ZHLN::Material& mat, float x = 0.0f) {
        return ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(2.4f, 1.1f, 0.12f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(x, 3.2, -1.2), .createPhysics = false, .materialOverride = mat}
        );
    }

    static bool TickFrames(ZHLN::Engine& engine, uint32_t frames, uint32_t lostBefore) {
        constexpr float dt = 1.0f / 60.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            engine.ProcessEvents();
            const auto status = engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
            if (ZHLN::RenderContext::DeviceLostCount() != lostBefore) {
                return false;
            }
            ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
        }
        return true;
    }

    static auto Capture(ZHLN::Engine& engine, const std::string& path) -> RgbImage {
        if (!engine.GetRenderContext().CaptureScreenshotPPM(path)) {
            return {};
        }
        return LoadPPM(path);
    }

    static constexpr NormRect kFloor {.x0 = 0.10, .y0 = 0.55, .x1 = 0.90, .y1 = 0.95};
    static constexpr NormRect kFloorLeft {.x0 = 0.08, .y0 = 0.55, .x1 = 0.42, .y1 = 0.95};
    static constexpr NormRect kFloorRight {.x0 = 0.58, .y0 = 0.55, .x1 = 0.92, .y1 = 0.95};
    static constexpr NormRect kSource {.x0 = 0.30, .y0 = 0.08, .x1 = 0.70, .y1 = 0.42};

    struct Session {
        std::unique_ptr<ZHLN::Engine> engine;
        uint32_t                      lostBefore = 0;
    };

    static auto Boot() -> std::expected<Session, ZHLN::Error> {
        Session s;
        s.engine = CreateTestEngine();
        if (!s.engine) {
            return std::unexpected(RTRPBRError::EngineInitFailed);
        }
        if (!s.engine->GetRenderContext().RayTracingSupported()) {
            ZHLN::Println("    [SKIP] Device has no raytracing; RTR PBR colour checks are not applicable.");
            s.engine.reset();
            return s;
        }

        DisableTAAAndFreeCam(*s.engine);
        // Build the TLAS on the default (SSR) path first. Flipping RTR on a
        // cold context with no acceleration structure is how the previous
        // suite TDR'd; that is a hang, not a colour bug.
        SetReflectionFlags(*s.engine, 1, 0);
        PlaceSunAndCamera(*s.engine);
        s.lostBefore = ZHLN::RenderContext::DeviceLostCount();
        if (!TickFrames(*s.engine, 4, s.lostBefore)) {
            return std::unexpected(RTRPBRError::DeviceLostDuringTest);
        }
        return s;
    }

    static auto EnableRTR(Session& s) -> std::expected<void, ZHLN::Error> {
        SetReflectionFlags(*s.engine, 0, 1);
        if (!TickFrames(*s.engine, 4, s.lostBefore)) {
            return std::unexpected(RTRPBRError::DeviceLostDuringTest);
        }
        return {};
    }

    static auto CaptureRTR(Session& s, const std::string& path) -> std::expected<RgbImage, ZHLN::Error> {
        if (!TickFrames(*s.engine, 2, s.lostBefore)) {
            return std::unexpected(RTRPBRError::DeviceLostDuringTest);
        }
        const RgbImage img = Capture(*s.engine, path);
        if (ZHLN::RenderContext::DeviceLostCount() != s.lostBefore) {
            return std::unexpected(RTRPBRError::DeviceLostDuringTest);
        }
        if (ImageIsBlank(img)) {
            ZHLN::Println("    [FAIL] Blank RTR capture {} (device-lost aftermath or empty TLAS).", path);
            return std::unexpected(RTRPBRError::RenderOutputBlank);
        }
        return img;
    }

    struct Tests {
        std::expected<void, ZHLN::Error> rtr_metallic_f0_tints_white_source() {
            auto session = Boot();
            if (!session) {
                return std::unexpected(session.error());
            }
            if (!session->engine) {
                return {};
            }
            Session& s = *session;

            auto chromeRes = MakeMat(*s.engine, 1.0f, 0.03f, {0.92f, 0.92f, 0.94f, 1.0f});
            auto goldRes   = MakeMat(*s.engine, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
            auto dielRes   = MakeMat(*s.engine, 0.0f, 0.03f, {0.92f, 0.92f, 0.94f, 1.0f});
            auto whiteEm   = MakeMat(*s.engine, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {24.0f, 24.0f, 24.0f, 1.0f});
            if (!chromeRes || !goldRes || !dielRes || !whiteEm) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            SpawnEmitter(*s.engine, *whiteEm);
            auto&              reg    = s.engine->GetRegistry();
            const ZHLN::Entity chrome = SpawnMirror(*s.engine, *chromeRes);
            auto               rtrOn  = EnableRTR(s);
            if (!rtrOn) {
                return rtrOn;
            }

            auto imgC = CaptureRTR(s, "headless_rtr_pbr_f0_chrome.ppm");
            if (!imgC) {
                return std::unexpected(imgC.error());
            }
            reg.Destroy(chrome);

            const ZHLN::Entity gold = SpawnMirror(*s.engine, *goldRes);
            auto               imgG = CaptureRTR(s, "headless_rtr_pbr_f0_gold.ppm");
            if (!imgG) {
                return std::unexpected(imgG.error());
            }
            (void) gold;
            reg.Destroy(gold);

            SpawnMirror(*s.engine, *dielRes);
            auto imgD = CaptureRTR(s, "headless_rtr_pbr_f0_diel.ppm");
            if (!imgD) {
                return std::unexpected(imgD.error());
            }

            const RegionStats chromeS = MeasureRegion(*imgC, kFloor);
            const RegionStats goldS   = MeasureRegion(*imgG, kFloor);
            const RegionStats dielS   = MeasureRegion(*imgD, kFloor);
            const RegionStats src     = MeasureRegion(*imgC, kSource);
            LogRegion("chrome metal", chromeS);
            LogRegion("gold metal", goldS);
            LogRegion("white dielectric", dielS);
            LogRegion("white emitter", src);

            // ACES×0.015 leaves the floor mean near 2 even when F0 is correct.
            // Judge ratios and metal-vs-dielectric energy, not absolute luma.
            const bool sourceSeen    = ZHLN::Test::ExpectTrue(src.maxL > 4.0);
            const bool chromeLit     = ZHLN::Test::ExpectTrue(chromeS.maxL > 6.0 && chromeS.meanL > dielS.meanL);
            const bool goldYellow    = ZHLN::Test::ExpectTrue(BlueRatio(goldS) + 0.08 < BlueRatio(chromeS) && goldS.meanR > goldS.meanB);
            const bool goldBluerLess = ZHLN::Test::ExpectTrue(BlueRatio(goldS) + 0.06 < BlueRatio(chromeS));
            const bool metalBrighter = ZHLN::Test::ExpectTrue(chromeS.meanL > dielS.meanL * 4.0 && chromeS.maxL > dielS.maxL * 2.0);
            const bool goldNotBlue   = ZHLN::Test::ExpectTrue(goldS.meanB < goldS.meanR * 0.5);

            if (!sourceSeen || !chromeLit || !goldYellow || !goldBluerLess || !metalBrighter || !goldNotBlue) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] RTR metallic F0 tints the white source; dielectric stays dimmer.");
            return {};
        }

        std::expected<void, ZHLN::Error> rtr_roughness_monotone_reflection_energy() {
            auto session = Boot();
            if (!session) {
                return std::unexpected(session.error());
            }
            if (!session->engine) {
                return {};
            }
            Session& s = *session;

            auto smooth = MakeMat(*s.engine, 1.0f, 0.02f, {0.90f, 0.90f, 0.92f, 1.0f});
            auto mid    = MakeMat(*s.engine, 1.0f, 0.22f, {0.90f, 0.90f, 0.92f, 1.0f});
            auto rough  = MakeMat(*s.engine, 1.0f, 0.70f, {0.90f, 0.90f, 0.92f, 1.0f});
            auto redEm  = MakeMat(*s.engine, 0.0f, 0.40f, {1.0f, 0.05f, 0.04f, 1.0f}, {24.0f, 0.0f, 0.0f, 1.0f});
            if (!smooth || !mid || !rough || !redEm) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            SpawnEmitter(*s.engine, *redEm);
            auto&              reg     = s.engine->GetRegistry();
            const ZHLN::Entity eSmooth = SpawnMirror(*s.engine, *smooth);
            auto               rtrOn   = EnableRTR(s);
            if (!rtrOn) {
                return rtrOn;
            }

            auto imgS = CaptureRTR(s, "headless_rtr_pbr_rough_s.ppm");
            if (!imgS) {
                return std::unexpected(imgS.error());
            }
            reg.Destroy(eSmooth);

            const ZHLN::Entity eMid = SpawnMirror(*s.engine, *mid);
            auto               imgM = CaptureRTR(s, "headless_rtr_pbr_rough_m.ppm");
            if (!imgM) {
                return std::unexpected(imgM.error());
            }
            (void) eMid;
            reg.Destroy(eMid);

            SpawnMirror(*s.engine, *rough);
            auto imgR = CaptureRTR(s, "headless_rtr_pbr_rough_r.ppm");
            if (!imgR) {
                return std::unexpected(imgR.error());
            }

            const RegionStats sSmooth = MeasureRegion(*imgS, kFloor);
            const RegionStats sMid    = MeasureRegion(*imgM, kFloor);
            const RegionStats sRough  = MeasureRegion(*imgR, kFloor);
            LogRegion("roughness 0.02", sSmooth);
            LogRegion("roughness 0.22", sMid);
            LogRegion("roughness 0.70", sRough);

            const bool monotoneL = ZHLN::Test::ExpectTrue(sSmooth.meanR + 0.05 >= sMid.meanR && sSmooth.maxL + 0.5 >= sRough.maxL);
            const bool sharpHot  = ZHLN::Test::ExpectTrue(sSmooth.maxL + 1.0 > sRough.maxL);
            const bool rtrOnSig  = ZHLN::Test::ExpectTrue(sSmooth.meanR > sRough.meanR * 1.5 + 0.05 || sSmooth.maxL > sRough.maxL + 1.0);
            const bool rtrOff    = ZHLN::Test::ExpectTrue(sRough.meanR * 2.0 <= sSmooth.meanR + 0.2 && sSmooth.maxL > sRough.maxL);

            if (!monotoneL || !sharpHot || !rtrOnSig || !rtrOff) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] RTR energy falls with roughness; r=0.70 drops the traced red.");
            return {};
        }

        std::expected<void, ZHLN::Error> rtr_chrome_preserves_emitter_hue() {
            auto session = Boot();
            if (!session) {
                return std::unexpected(session.error());
            }
            if (!session->engine) {
                return {};
            }
            Session& s = *session;

            auto chrome  = MakeMat(*s.engine, 1.0f, 0.025f, {0.94f, 0.94f, 0.96f, 1.0f});
            auto redEm   = MakeMat(*s.engine, 0.0f, 0.40f, {1.0f, 0.04f, 0.03f, 1.0f}, {24.0f, 0.0f, 0.0f, 1.0f});
            auto greenEm = MakeMat(*s.engine, 0.0f, 0.40f, {0.04f, 1.0f, 0.04f, 1.0f}, {0.0f, 24.0f, 0.0f, 1.0f});
            if (!chrome || !redEm || !greenEm) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            SpawnMirror(*s.engine, *chrome);
            SpawnEmitter(*s.engine, *redEm, -2.4f);
            SpawnEmitter(*s.engine, *greenEm, 2.4f);
            auto rtrOn = EnableRTR(s);
            if (!rtrOn) {
                return rtrOn;
            }

            auto img = CaptureRTR(s, "headless_rtr_pbr_emitter_hue.ppm");
            if (!img) {
                return std::unexpected(img.error());
            }

            const RegionStats left  = MeasureRegion(*img, kFloorLeft);
            const RegionStats right = MeasureRegion(*img, kFloorRight);
            LogRegion("chrome under red", left);
            LogRegion("chrome under green", right);

            const bool leftRedder   = ZHLN::Test::ExpectTrue(left.meanR > left.meanG && left.meanR > left.meanB);
            const bool rightGreener = ZHLN::Test::ExpectTrue(right.meanG > right.meanR && right.meanG > right.meanB);
            const bool splitHue     = ZHLN::Test::ExpectTrue(left.meanR > right.meanR && right.meanG > left.meanG);
            const bool counts       = ZHLN::Test::ExpectTrue(left.meanR > 0.5 && right.meanG > 0.5);

            if (!leftRedder || !rightGreener || !splitHue || !counts) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Chrome RTR preserves emitter hue (red left / green right).");
            return {};
        }

        std::expected<void, ZHLN::Error> rtr_live_pbr_patch_retints_same_tile() {
            auto session = Boot();
            if (!session) {
                return std::unexpected(session.error());
            }
            if (!session->engine) {
                return {};
            }
            Session& s = *session;

            auto gold    = MakeMat(*s.engine, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
            auto whiteEm = MakeMat(*s.engine, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {24.0f, 24.0f, 24.0f, 1.0f});
            if (!gold || !whiteEm) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            const ZHLN::Entity tile = SpawnMirror(*s.engine, *gold);
            SpawnEmitter(*s.engine, *whiteEm);
            auto rtrOn = EnableRTR(s);
            if (!rtrOn) {
                return rtrOn;
            }

            auto& reg         = s.engine->GetRegistry();
            auto  captureTile = [&](const char* path, float metallic, float roughness) -> std::expected<RegionStats, ZHLN::Error> {
                if (!reg.Patch<ZHLN::Components::PBRComponent>(tile, [&](auto& pbr) {
                        pbr.metallic  = metallic;
                        pbr.roughness = roughness;
                    })) {
                    return std::unexpected(RTRPBRError::MaterialCreationFailed);
                }
                auto img = CaptureRTR(s, path);
                if (!img) {
                    return std::unexpected(img.error());
                }
                return MeasureRegion(*img, kFloor);
            };

            auto metalSmooth = captureTile("headless_rtr_pbr_patch_metal.ppm", 1.0f, 0.03f);
            if (!metalSmooth) {
                return std::unexpected(metalSmooth.error());
            }
            auto dielectric = captureTile("headless_rtr_pbr_patch_diel.ppm", 0.0f, 0.03f);
            if (!dielectric) {
                return std::unexpected(dielectric.error());
            }
            auto metalRough = captureTile("headless_rtr_pbr_patch_rough.ppm", 1.0f, 0.75f);
            if (!metalRough) {
                return std::unexpected(metalRough.error());
            }
            LogRegion("patch metal r=0.03", *metalSmooth);
            LogRegion("patch dielectric r=0.03", *dielectric);
            LogRegion("patch metal r=0.75", *metalRough);

            const bool metalEnergy   = ZHLN::Test::ExpectTrue(metalSmooth->meanL > dielectric->meanL * 4.0 && metalSmooth->maxL > dielectric->maxL * 2.0);
            const bool metalYellower = ZHLN::Test::ExpectTrue(metalSmooth->meanR > metalSmooth->meanB && metalSmooth->meanG > metalSmooth->meanB);
            const bool roughKills    = ZHLN::Test::ExpectTrue(metalSmooth->meanL > metalRough->meanL && metalSmooth->maxL > metalRough->maxL);
            const bool roughLessGold = ZHLN::Test::ExpectTrue(metalSmooth->maxL + 0.5 >= metalRough->maxL);

            if (!metalEnergy || !metalYellower || !roughKills || !roughLessGold) {
                return std::unexpected(RTRPBRError::ReflectionColorMismatch);
            }
            ZHLN::Println("    [PASS] Live PBR patches retint / kill the same tile's RTR colour.");
            return {};
        }

        std::expected<void, ZHLN::Error> rtr_metal_palette_channel_order() {
            auto session = Boot();
            if (!session) {
                return std::unexpected(session.error());
            }
            if (!session->engine) {
                return {};
            }
            Session& s = *session;

            auto silver  = MakeMat(*s.engine, 1.0f, 0.03f, {0.97f, 0.96f, 0.93f, 1.0f});
            auto gold    = MakeMat(*s.engine, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
            auto copper  = MakeMat(*s.engine, 1.0f, 0.03f, {0.95f, 0.64f, 0.54f, 1.0f});
            auto whiteEm = MakeMat(*s.engine, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {24.0f, 24.0f, 24.0f, 1.0f});
            if (!silver || !gold || !copper || !whiteEm) {
                return std::unexpected(RTRPBRError::MaterialCreationFailed);
            }

            SpawnEmitter(*s.engine, *whiteEm);
            auto&              reg   = s.engine->GetRegistry();
            const ZHLN::Entity eSil  = SpawnMirror(*s.engine, *silver);
            auto               rtrOn = EnableRTR(s);
            if (!rtrOn) {
                return rtrOn;
            }

            auto imgSil = CaptureRTR(s, "headless_rtr_pbr_silver.ppm");
            if (!imgSil) {
                return std::unexpected(imgSil.error());
            }
            reg.Destroy(eSil);

            const ZHLN::Entity eAu   = SpawnMirror(*s.engine, *gold);
            auto               imgAu = CaptureRTR(s, "headless_rtr_pbr_gold.ppm");
            if (!imgAu) {
                return std::unexpected(imgAu.error());
            }
            (void) eAu;
            reg.Destroy(eAu);

            SpawnMirror(*s.engine, *copper);
            auto imgCu = CaptureRTR(s, "headless_rtr_pbr_copper.ppm");
            if (!imgCu) {
                return std::unexpected(imgCu.error());
            }

            const RegionStats sil = MeasureRegion(*imgSil, kFloor);
            const RegionStats au  = MeasureRegion(*imgAu, kFloor);
            const RegionStats cu  = MeasureRegion(*imgCu, kFloor);
            LogRegion("silver", sil);
            LogRegion("gold", au);
            LogRegion("copper", cu);

            const bool goldLeastBlue  = ZHLN::Test::ExpectTrue(BlueRatio(au) + 0.04 < BlueRatio(sil) && BlueRatio(au) + 0.03 < BlueRatio(cu));
            const bool goldMoreYellow = ZHLN::Test::ExpectTrue(GreenRatio(au) > GreenRatio(cu) + 0.03 && au.yellow + 5u >= cu.yellow);
            const bool silverNeutral  = ZHLN::Test::ExpectTrue(std::abs(sil.meanR - sil.meanG) < 28.0 && BlueRatio(sil) > 0.45);
            const bool allReflect     = ZHLN::Test::ExpectTrue(sil.maxL > 6.0 && au.maxL > 6.0 && cu.maxL > 6.0);

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
