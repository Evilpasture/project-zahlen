// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Public-API GPU suite: reflection *colour* must follow PBR.
//
// The reflection pass (resources/shaders/reflection.slang) only traces when
// roughness <= 0.4, then multiplies the hit colour by FssEss where
//   F0 = lerp(0.04, albedo, metallic)
// so a gold metal must yellow-tint a white source, a dielectric of the same
// albedo must stay much dimmer, and raising roughness past the threshold
// must kill the traced signature. All of that is judged from captured PPM
// pixels — no src/ includes.
//
// Device-lost handling: HandleDeviceLost() clears the GPU mesh cache, so
// geometry spawned before a TDR becomes invisible and every later capture is
// black. Each attempt therefore boots a *fresh* Engine, warms it, then
// spawns. RTR is requested first; if that attempt captures a blank frame we
// fall back to SSR (same F0 composite). Never reuse a RenderContext& across
// Tick.

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
    uint32_t yellow   = 0;
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

[[nodiscard]] bool ImageIsBlank(const RgbImage& img) {
    if (!img.Valid()) {
        return true;
    }
    double sum = 0.0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        sum += Luma(img.rgb[i], img.rgb[i + 1], img.rgb[i + 2]);
    }
    return (sum / static_cast<double>(img.rgb.size() / 3u)) < 0.4;
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

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
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

    static void ConfigureReflections(ZHLN::Engine& engine, bool wantRTR) {
        auto&      reg      = engine.GetRegistry();
        const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings.empty()) {
            return;
        }
        const bool rtr = wantRTR && engine.GetRenderContext().RayTracingSupported();
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [rtr](auto& pp) {
            pp.fullBright        = 0;
            pp.ambientExposure   = 6.0f;
            pp.vignetteIntensity = 0.0f;
            // F0 tint lives in the shared composite; SSR is the stable default
            // path, RTR is requested when the device can take it.
            pp.enableSSR   = 1;
            pp.enableRTR   = rtr ? 1 : 0;
            pp.giMode      = 0;
            pp.giIntensity = 0.0f;
            pp.skyZenith   = JPH::Vec4(0.001f, 0.002f, 0.006f, 1.0f);
            pp.skyHorizon  = JPH::Vec4(0.004f, 0.006f, 0.012f, 1.0f);
            pp.skyGround   = JPH::Vec4(0.001f, 0.001f, 0.002f, 1.0f);
        });
    }

    static void PlaceSunAndCamera(ZHLN::Engine& engine) {
        auto&              reg    = engine.GetRegistry();
        const ZHLN::Entity sunEnt = reg.Create();
        reg.Add(
            sunEnt,
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 140.0f, .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
            }
        );

        auto& cam    = engine.GetCamera();
        cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
        cam.yaw      = -90.0f;
        cam.pitch    = -22.0f;
        cam.fov      = 60.0f;
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
            engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = mat}
        );
    }

    static ZHLN::Entity SpawnEmitter(ZHLN::Engine& engine, const ZHLN::Material& mat, float x = 0.0f) {
        return ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(x, 3.0, 0.0), .createPhysics = false, .materialOverride = mat}
        );
    }

    static void TickFrames(ZHLN::Engine& engine, uint32_t frames) {
        constexpr float dt = 1.0f / 60.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            engine.ProcessEvents();
            // Do not ExpectEq: a device-lost Tick must not poison a later retry.
            (void) engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
        }
    }

    static auto Capture(ZHLN::Engine& engine, const std::string& path) -> RgbImage {
        if (!engine.GetRenderContext().CaptureScreenshotPPM(path)) {
            return {};
        }
        return LoadPPM(path);
    }

    [[nodiscard]] static bool LostSince(ZHLN::Engine& engine, ZHLN::RenderContext* expected, uint32_t lostBefore) {
        return &engine.GetRenderContext() != expected || ZHLN::RenderContext::DeviceLostCount() != lostBefore;
    }

    // Lower half of the frame is the polished floor (source sits in the upper half).
    static constexpr NormRect kFloor {.x0 = 0.10, .y0 = 0.55, .x1 = 0.90, .y1 = 0.95};
    static constexpr NormRect kFloorLeft {.x0 = 0.08, .y0 = 0.55, .x1 = 0.42, .y1 = 0.95};
    static constexpr NormRect kFloorRight {.x0 = 0.58, .y0 = 0.55, .x1 = 0.92, .y1 = 0.95};
    static constexpr NormRect kSource {.x0 = 0.30, .y0 = 0.08, .x1 = 0.70, .y1 = 0.42};

    struct Attempt {
        std::unique_ptr<ZHLN::Engine> engine;
        ZHLN::RenderContext*          ctx        = nullptr;
        uint32_t                      lostBefore = 0;
        bool                          usedRTR    = false;
    };

    static auto BeginAttempt(uint32_t attempt) -> Attempt {
        Attempt a;
        a.engine = CreateTestEngine();
        if (!a.engine) {
            return a;
        }
        a.usedRTR = (attempt == 0) && a.engine->GetRenderContext().RayTracingSupported();
        DisableTAAAndFreeCam(*a.engine);
        ConfigureReflections(*a.engine, a.usedRTR);
        PlaceSunAndCamera(*a.engine);
        a.lostBefore = ZHLN::RenderContext::DeviceLostCount();
        a.ctx        = &a.engine->GetRenderContext();
        TickFrames(*a.engine, 5);
        if (LostSince(*a.engine, a.ctx, a.lostBefore)) {
            ZHLN::Println("    [WARN] Device lost during warmup (attempt {}).", attempt);
            a.engine.reset();
            return a;
        }
        a.ctx        = &a.engine->GetRenderContext();
        a.lostBefore = ZHLN::RenderContext::DeviceLostCount();
        ZHLN::Println("    [INFO] attempt {} using {}.", attempt, a.usedRTR ? "SSR+RTR" : "SSR");
        return a;
    }

    static auto CaptureOrBlank(Attempt& a, const std::string& path) -> RgbImage {
        if (!a.engine || LostSince(*a.engine, a.ctx, a.lostBefore)) {
            return {};
        }
        TickFrames(*a.engine, 4);
        if (LostSince(*a.engine, a.ctx, a.lostBefore)) {
            return {};
        }
        const RgbImage img = Capture(*a.engine, path);
        if (ImageIsBlank(img) || LostSince(*a.engine, a.ctx, a.lostBefore)) {
            ZHLN::Println("    [WARN] Blank or lost capture {}.", path);
            return {};
        }
        return img;
    }

    struct Tests {
        std::expected<void, ZHLN::Error> rtr_metallic_f0_tints_white_source() {
            ZHLN::Test::AllowDeviceLost(true);
            for (uint32_t attempt = 0; attempt < 2; ++attempt) {
                Attempt a = BeginAttempt(attempt);
                if (!a.engine) {
                    continue;
                }

                auto chromeRes = MakeMat(*a.engine, 1.0f, 0.03f, {0.92f, 0.92f, 0.94f, 1.0f});
                auto goldRes   = MakeMat(*a.engine, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
                auto dielRes   = MakeMat(*a.engine, 0.0f, 0.03f, {0.92f, 0.92f, 0.94f, 1.0f});
                auto whiteEm   = MakeMat(*a.engine, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {6.0f, 6.0f, 6.0f, 1.0f});
                if (!chromeRes || !goldRes || !dielRes || !whiteEm) {
                    return std::unexpected(RTRPBRError::MaterialCreationFailed);
                }

                SpawnEmitter(*a.engine, *whiteEm);
                auto&              reg    = a.engine->GetRegistry();
                const ZHLN::Entity chrome = SpawnMirror(*a.engine, *chromeRes);
                const RgbImage     imgC   = CaptureOrBlank(a, "headless_rtr_pbr_f0_chrome.ppm");
                if (ImageIsBlank(imgC)) {
                    continue;
                }
                reg.Destroy(chrome);

                const ZHLN::Entity gold = SpawnMirror(*a.engine, *goldRes);
                const RgbImage     imgG = CaptureOrBlank(a, "headless_rtr_pbr_f0_gold.ppm");
                if (ImageIsBlank(imgG)) {
                    continue;
                }
                reg.Destroy(gold);

                SpawnMirror(*a.engine, *dielRes);
                const RgbImage imgD = CaptureOrBlank(a, "headless_rtr_pbr_f0_diel.ppm");
                if (ImageIsBlank(imgD)) {
                    continue;
                }

                const RegionStats chromeS = MeasureRegion(imgC, kFloor);
                const RegionStats goldS   = MeasureRegion(imgG, kFloor);
                const RegionStats dielS   = MeasureRegion(imgD, kFloor);
                const RegionStats src     = MeasureRegion(imgC, kSource);
                LogRegion("chrome metal", chromeS);
                LogRegion("gold metal", goldS);
                LogRegion("white dielectric", dielS);
                LogRegion("white emitter", src);

                const bool sourceLit     = ZHLN::Test::ExpectTrue(src.meanL > 18.0);
                const bool chromeLit     = ZHLN::Test::ExpectTrue(chromeS.meanL > 8.0);
                const bool goldYellow    = ZHLN::Test::ExpectTrue(
                    goldS.yellow > 40u || (goldS.meanR > 28.0 && goldS.meanG > 18.0 && BlueRatio(goldS) + 0.08 < BlueRatio(chromeS))
                );
                const bool goldBluerLess = ZHLN::Test::ExpectTrue(BlueRatio(goldS) + 0.06 < BlueRatio(chromeS));
                const bool metalBrighter = ZHLN::Test::ExpectTrue(chromeS.meanL > dielS.meanL * 1.35 + 2.0);
                const bool goldNotBlue   = ZHLN::Test::ExpectTrue(goldS.meanB + 6.0 < goldS.meanR);

                if (!sourceLit || !chromeLit || !goldYellow || !goldBluerLess || !metalBrighter || !goldNotBlue) {
                    return std::unexpected(RTRPBRError::ReflectionColorMismatch);
                }
                ZHLN::Println("    [PASS] Metallic F0 tints the reflected white source; dielectric stays dimmer.");
                return {};
            }
            return std::unexpected(RTRPBRError::RenderOutputBlank);
        }

        std::expected<void, ZHLN::Error> rtr_roughness_monotone_reflection_energy() {
            ZHLN::Test::AllowDeviceLost(true);
            for (uint32_t attempt = 0; attempt < 2; ++attempt) {
                Attempt a = BeginAttempt(attempt);
                if (!a.engine) {
                    continue;
                }

                auto smooth = MakeMat(*a.engine, 1.0f, 0.02f, {0.90f, 0.90f, 0.92f, 1.0f});
                auto mid    = MakeMat(*a.engine, 1.0f, 0.22f, {0.90f, 0.90f, 0.92f, 1.0f});
                auto rough  = MakeMat(*a.engine, 1.0f, 0.70f, {0.90f, 0.90f, 0.92f, 1.0f});
                auto redEm  = MakeMat(*a.engine, 0.0f, 0.40f, {1.0f, 0.05f, 0.04f, 1.0f}, {8.0f, 0.0f, 0.0f, 1.0f});
                if (!smooth || !mid || !rough || !redEm) {
                    return std::unexpected(RTRPBRError::MaterialCreationFailed);
                }

                SpawnEmitter(*a.engine, *redEm);
                auto&              reg      = a.engine->GetRegistry();
                const ZHLN::Entity eSmooth  = SpawnMirror(*a.engine, *smooth);
                const RgbImage     imgS     = CaptureOrBlank(a, "headless_rtr_pbr_rough_s.ppm");
                if (ImageIsBlank(imgS)) {
                    continue;
                }
                reg.Destroy(eSmooth);

                const ZHLN::Entity eMid = SpawnMirror(*a.engine, *mid);
                const RgbImage     imgM = CaptureOrBlank(a, "headless_rtr_pbr_rough_m.ppm");
                if (ImageIsBlank(imgM)) {
                    continue;
                }
                reg.Destroy(eMid);

                SpawnMirror(*a.engine, *rough);
                const RgbImage imgR = CaptureOrBlank(a, "headless_rtr_pbr_rough_r.ppm");
                if (ImageIsBlank(imgR)) {
                    continue;
                }

                const RegionStats sSmooth = MeasureRegion(imgS, kFloor);
                const RegionStats sMid    = MeasureRegion(imgM, kFloor);
                const RegionStats sRough  = MeasureRegion(imgR, kFloor);
                LogRegion("roughness 0.02", sSmooth);
                LogRegion("roughness 0.22", sMid);
                LogRegion("roughness 0.70", sRough);

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
            return std::unexpected(RTRPBRError::RenderOutputBlank);
        }

        std::expected<void, ZHLN::Error> rtr_chrome_preserves_emitter_hue() {
            ZHLN::Test::AllowDeviceLost(true);
            for (uint32_t attempt = 0; attempt < 2; ++attempt) {
                Attempt a = BeginAttempt(attempt);
                if (!a.engine) {
                    continue;
                }

                auto chrome  = MakeMat(*a.engine, 1.0f, 0.025f, {0.94f, 0.94f, 0.96f, 1.0f});
                auto redEm   = MakeMat(*a.engine, 0.0f, 0.40f, {1.0f, 0.04f, 0.03f, 1.0f}, {8.0f, 0.0f, 0.0f, 1.0f});
                auto greenEm = MakeMat(*a.engine, 0.0f, 0.40f, {0.04f, 1.0f, 0.04f, 1.0f}, {0.0f, 8.0f, 0.0f, 1.0f});
                if (!chrome || !redEm || !greenEm) {
                    return std::unexpected(RTRPBRError::MaterialCreationFailed);
                }

                SpawnMirror(*a.engine, *chrome);
                SpawnEmitter(*a.engine, *redEm, -2.4f);
                SpawnEmitter(*a.engine, *greenEm, 2.4f);

                const RgbImage img = CaptureOrBlank(a, "headless_rtr_pbr_emitter_hue.ppm");
                if (ImageIsBlank(img)) {
                    continue;
                }

                const RegionStats left  = MeasureRegion(img, kFloorLeft);
                const RegionStats right = MeasureRegion(img, kFloorRight);
                LogRegion("chrome under red", left);
                LogRegion("chrome under green", right);

                const bool leftRedder   = ZHLN::Test::ExpectTrue(left.meanR > left.meanG + 4.0 && left.meanR > left.meanB + 4.0);
                const bool rightGreener = ZHLN::Test::ExpectTrue(right.meanG > right.meanR + 4.0 && right.meanG > right.meanB + 2.0);
                const bool splitHue     = ZHLN::Test::ExpectTrue(left.meanR > right.meanR + 6.0 && right.meanG > left.meanG + 6.0);
                const bool counts       = ZHLN::Test::ExpectTrue(left.redDom + 10u > right.redDom && right.greenDom + 10u > left.greenDom);

                if (!leftRedder || !rightGreener || !splitHue || !counts) {
                    return std::unexpected(RTRPBRError::ReflectionColorMismatch);
                }
                ZHLN::Println("    [PASS] Chrome reflection preserves emitter hue (red left / green right).");
                return {};
            }
            return std::unexpected(RTRPBRError::RenderOutputBlank);
        }

        std::expected<void, ZHLN::Error> rtr_live_pbr_patch_retints_same_tile() {
            for (uint32_t attempt = 0; attempt < 3; ++attempt) {
                Attempt a = BeginAttempt(attempt);
                if (!a.engine) {
                    continue;
                }

                auto gold    = MakeMat(*a.engine, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
                auto whiteEm = MakeMat(*a.engine, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {6.0f, 6.0f, 6.0f, 1.0f});
                if (!gold || !whiteEm) {
                    return std::unexpected(RTRPBRError::MaterialCreationFailed);
                }

                const ZHLN::Entity tile = SpawnMirror(*a.engine, *gold);
                SpawnEmitter(*a.engine, *whiteEm);
                auto& reg = a.engine->GetRegistry();

                auto captureTile = [&](const char* path, float metallic, float roughness) -> RegionStats {
                    if (!a.engine || LostSince(*a.engine, a.ctx, a.lostBefore)) {
                        return {};
                    }
                    reg.Patch<ZHLN::Components::PBRComponent>(tile, [&](auto& pbr) {
                        pbr.metallic  = metallic;
                        pbr.roughness = roughness;
                    });
                    const RgbImage img = CaptureOrBlank(a, path);
                    return MeasureRegion(img, kFloor);
                };

                const RegionStats metalSmooth = captureTile("headless_rtr_pbr_patch_metal.ppm", 1.0f, 0.03f);
                if (metalSmooth.pixels == 0) {
                    continue;
                }
                const RegionStats dielectric = captureTile("headless_rtr_pbr_patch_diel.ppm", 0.0f, 0.03f);
                if (dielectric.pixels == 0) {
                    continue;
                }
                const RegionStats metalRough = captureTile("headless_rtr_pbr_patch_rough.ppm", 1.0f, 0.75f);
                if (metalRough.pixels == 0) {
                    continue;
                }
                LogRegion("patch metal r=0.03", metalSmooth);
                LogRegion("patch dielectric r=0.03", dielectric);
                LogRegion("patch metal r=0.75", metalRough);

                const bool metalEnergy   = ZHLN::Test::ExpectTrue(metalSmooth.meanL > dielectric.meanL * 1.30 + 2.0);
                const bool metalYellower =
                    ZHLN::Test::ExpectTrue(BlueRatio(metalSmooth) + 0.05 < BlueRatio(dielectric) || metalSmooth.yellow > dielectric.yellow + 15u);
                const bool roughKills    = ZHLN::Test::ExpectTrue(metalSmooth.meanL > metalRough.meanL + 2.0 && metalSmooth.yellow + 8u > metalRough.yellow);
                const bool roughLessGold = ZHLN::Test::ExpectTrue(metalSmooth.maxL + 2.0 >= metalRough.maxL);

                if (!metalEnergy || !metalYellower || !roughKills || !roughLessGold) {
                    return std::unexpected(RTRPBRError::ReflectionColorMismatch);
                }
                ZHLN::Println("    [PASS] Live PBR patches retint / kill the same tile's reflection colour.");
                return {};
            }
            return std::unexpected(RTRPBRError::RenderOutputBlank);
        }

        std::expected<void, ZHLN::Error> rtr_metal_palette_channel_order() {
            ZHLN::Test::AllowDeviceLost(true);
            for (uint32_t attempt = 0; attempt < 2; ++attempt) {
                Attempt a = BeginAttempt(attempt);
                if (!a.engine) {
                    continue;
                }

                auto silver  = MakeMat(*a.engine, 1.0f, 0.03f, {0.97f, 0.96f, 0.93f, 1.0f});
                auto gold    = MakeMat(*a.engine, 1.0f, 0.03f, {1.00f, 0.76f, 0.14f, 1.0f});
                auto copper  = MakeMat(*a.engine, 1.0f, 0.03f, {0.95f, 0.64f, 0.54f, 1.0f});
                auto whiteEm = MakeMat(*a.engine, 0.0f, 0.45f, {1.0f, 1.0f, 1.0f, 1.0f}, {6.0f, 6.0f, 6.0f, 1.0f});
                if (!silver || !gold || !copper || !whiteEm) {
                    return std::unexpected(RTRPBRError::MaterialCreationFailed);
                }

                SpawnEmitter(*a.engine, *whiteEm);
                auto&              reg    = a.engine->GetRegistry();
                const ZHLN::Entity eSil   = SpawnMirror(*a.engine, *silver);
                const RgbImage     imgSil = CaptureOrBlank(a, "headless_rtr_pbr_silver.ppm");
                if (ImageIsBlank(imgSil)) {
                    continue;
                }
                reg.Destroy(eSil);

                const ZHLN::Entity eAu   = SpawnMirror(*a.engine, *gold);
                const RgbImage     imgAu = CaptureOrBlank(a, "headless_rtr_pbr_gold.ppm");
                if (ImageIsBlank(imgAu)) {
                    continue;
                }
                reg.Destroy(eAu);

                SpawnMirror(*a.engine, *copper);
                const RgbImage imgCu = CaptureOrBlank(a, "headless_rtr_pbr_copper.ppm");
                if (ImageIsBlank(imgCu)) {
                    continue;
                }

                const RegionStats sil = MeasureRegion(imgSil, kFloor);
                const RegionStats au  = MeasureRegion(imgAu, kFloor);
                const RegionStats cu  = MeasureRegion(imgCu, kFloor);
                LogRegion("silver", sil);
                LogRegion("gold", au);
                LogRegion("copper", cu);

                const bool goldLeastBlue  = ZHLN::Test::ExpectTrue(BlueRatio(au) + 0.04 < BlueRatio(sil) && BlueRatio(au) + 0.03 < BlueRatio(cu));
                const bool goldMoreYellow = ZHLN::Test::ExpectTrue(GreenRatio(au) > GreenRatio(cu) + 0.03 && au.yellow + 5u >= cu.yellow);
                const bool silverNeutral  = ZHLN::Test::ExpectTrue(std::abs(sil.meanR - sil.meanG) < 28.0 && BlueRatio(sil) > 0.45);
                const bool allReflect     = ZHLN::Test::ExpectTrue(sil.meanL > 6.0 && au.meanL > 6.0 && cu.meanL > 6.0);

                if (!goldLeastBlue || !goldMoreYellow || !silverNeutral || !allReflect) {
                    return std::unexpected(RTRPBRError::ReflectionColorMismatch);
                }
                ZHLN::Println("    [PASS] Silver / gold / copper reflection channel order matches F0.");
                return {};
            }
            return std::unexpected(RTRPBRError::RenderOutputBlank);
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<RTRPBRReflectionTestSuite>();
}
