// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestEmissiveShading.cpp
//
// Emission has to be additive, not reflectance.
//
// The deferred G-Buffer used to fold emission into the albedo attachment
// (basic.slang wrote `albedo.rgb + emissive` to SV_Target0). The lighting pass
// then multiplied that attachment by incident light, so an emissive surface
// only glowed while something else was already lighting it: at night, in
// shadow, or on a face turned away from the sun, a neon material went black.
// Emission now owns G-Buffer attachment 3 and lighting.slang adds it to the
// composed result unconditionally.
//
// The scene here is the one that used to fail: no sun, no punctual lights, and
// ambient turned all the way down. Whatever the emissive box shows in that
// frame can only have come from the emissive term.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

enum class EmissiveShadingError : uint8_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize the headless Engine for the emissive scene.">{}]] = 1,
    MaterialCreationFailed[[= ZHLN::Description<"CreateMaterial rejected the emissive or the control material.">{}]],
    CaptureFailed[[= ZHLN::Description<"The rendered frame could not be read back.">{}]],
    EmissiveWentDark[[= ZHLN::Description<"An emissive surface rendered black with no light shining on it -- emission is being treated as reflectance.">{}]],
    EmissiveHueLost[[= ZHLN::Description<"The unlit emissive surface is bright but not the colour it emits.">{}]],
    ControlNotDark[[= ZHLN::Description<"The non-emissive control box is lit, so the scene is not the unlit scene the test needs.">{}]],
    BackgroundNotDark[[= ZHLN::Description<"Emission leaked into pixels the emitter does not cover.">{}]],
    DeviceLost[[= ZHLN::Description<"The Vulkan device was lost and the engine could not recover.">{}]],
};

namespace {

using ZHLN::Test::Image::NormalizedRect;
using ZHLN::Test::Image::SubRegionStats;

// The box sits at the origin, the camera looks straight at it down -Z, so the
// emitter covers the middle of the frame and the corners see only sky.
constexpr NormalizedRect kBoxWindow {.x0 = 0.40, .y0 = 0.40, .x1 = 0.60, .y1 = 0.60};
constexpr NormalizedRect kCornerWindow {.x0 = 0.02, .y0 = 0.02, .x1 = 0.18, .y1 = 0.18};

// A strong green emitter: green is the one channel neither the sky gradient
// (blue-dominant) nor the default clear colour leans on, so a green-dominant
// pixel in the box window can only be the emissive material.
constexpr std::array<float, 4> kEmissiveGreen {0.0f, 3.0f, 0.0f, 1.0f};
constexpr std::array<float, 4> kBoxBaseColor {0.05f, 0.05f, 0.05f, 1.0f};

/// Builds the unlit scene: one box at the origin, no lights of any kind, and
/// ambient/GI dialled out so nothing but emission can brighten a surface.
///
/// Returns false when material creation fails, which is a setup failure rather
/// than a rendering result.
[[nodiscard]] bool BuildUnlitBoxScene(ZHLN::Engine& engine, bool emissive) {
    auto& registry = engine.GetRegistry();
    auto& renderCtx = engine.GetRenderContext();

    for (const ZHLN::Entity settings: registry.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        registry.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& pp) {
            pp.fullBright      = 0;
            pp.ambientExposure = 0.0f;
            pp.giMode          = 0;
            pp.giIntensity     = 0.0f;
            pp.enableSSR       = 0;
            pp.enableRTR       = 0;
            pp.skyZenith       = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            pp.skyHorizon      = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            pp.skyGround       = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        });
    }

    const auto material = ZHLN::CreativeWorksFactory::CreateMaterial(
        renderCtx, ZHLN::CreativeWorksFactory::MaterialDesc {
                       .metallic  = 0.0f,
                       .roughness = 0.8f,
                       .baseColor = kBoxBaseColor,
                       .emissive  = emissive ? kEmissiveGreen : std::array<float, 4> {0.0f, 0.0f, 0.0f, 1.0f}
                   }
    );
    if (!material.has_value()) {
        return false;
    }

    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(1.0f, 1.0f, 1.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *material}
    );

    auto& camera    = engine.GetCamera();
    camera.position = JPH::Vec3(0.0f, 0.0f, 6.0f);
    camera.yaw      = -90.0f;
    camera.pitch    = 0.0f;
    camera.fov      = 60.0f;
    return true;
}

/// Renders the unlit scene once and reports the box window and a background
/// corner. `outValid` distinguishes a failed capture from a black frame.
[[nodiscard]] auto MeasureUnlitBox(bool emissive, const std::string& ppmPath, SubRegionStats& outCorner, bool& outValid) -> SubRegionStats {
    outValid = false;
    SubRegionStats box;

    const auto engine = ZHLN::Test::Headless::CreateEngine(emissive ? "Headless Emissive Unlit" : "Headless Emissive Control");
    if (engine == nullptr) {
        return box;
    }
    ZHLN::Test::Headless::DisableTAA(*engine);

    if (!BuildUnlitBoxScene(*engine, emissive)) {
        return box;
    }

    ZHLN::Test::Headless::TickFrames(*engine, 4);
    const auto frame = ZHLN::Test::Headless::Capture(*engine, ppmPath);
    if (!frame.Valid()) {
        return box;
    }

    box       = ZHLN::Test::Image::MeasureSubRegion(frame, kBoxWindow);
    outCorner = ZHLN::Test::Image::MeasureSubRegion(frame, kCornerWindow);
    outValid  = true;
    return box;
}

} // namespace

struct EmissiveShadingTestSuite {
    EmissiveShadingTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~EmissiveShadingTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        /**
         * An emissive surface with nothing shining on it still emits.
         *
         * Three things are asserted against one another rather than against
         * absolute pixel values, so an exposure or tonemapper change does not
         * silently invalidate the test:
         *
         *   1. the emissive box is bright and green in a scene with no lights;
         *   2. the identical non-emissive box in the identical scene is dark,
         *      which is what proves the light in (1) came from emission and
         *      not from a stray ambient term;
         *   3. the frame corner stays dark, so (1) is the emitter and not a
         *      full-screen brightening.
         */
        std::expected<void, ZHLN::Error> emission_survives_a_scene_with_no_lights() {
            SubRegionStats emissiveCorner;
            bool           emissiveValid = false;
            const SubRegionStats emissiveBox = MeasureUnlitBox(true, "emissive_unlit.ppm", emissiveCorner, emissiveValid);
            if (!emissiveValid) {
                return std::unexpected(EmissiveShadingError::CaptureFailed);
            }

            SubRegionStats controlCorner;
            bool           controlValid = false;
            const SubRegionStats controlBox = MeasureUnlitBox(false, "emissive_unlit_control.ppm", controlCorner, controlValid);
            if (!controlValid) {
                return std::unexpected(EmissiveShadingError::CaptureFailed);
            }

            // 1. The emitter is visible at all. Before the fix this window was
            //    the clear colour: the lighting pass multiplied the baked-in
            //    emission by an incident light of zero.
            if (emissiveBox.meanLuma < 24.0) {
                ZHLN::Println("    [INFO] emissive box meanLuma={:.1f} maxLuma={:.1f}", emissiveBox.meanLuma, emissiveBox.maxLuma);
                return std::unexpected(EmissiveShadingError::EmissiveWentDark);
            }

            // 2. It is the colour it emits, not a grey wash.
            const double greenShare = static_cast<double>(emissiveBox.dominantGrn) / static_cast<double>(std::max(emissiveBox.pixels, 1u));
            if (greenShare < 0.5 || emissiveBox.meanG <= emissiveBox.meanR || emissiveBox.meanG <= emissiveBox.meanB) {
                ZHLN::Println(
                    "    [INFO] emissive box greenShare={:.2f} meanRGB=({:.1f}, {:.1f}, {:.1f})", greenShare, emissiveBox.meanR, emissiveBox.meanG,
                    emissiveBox.meanB
                );
                return std::unexpected(EmissiveShadingError::EmissiveHueLost);
            }

            // 3. The same box without the emissive factor stays dark, so the
            //    scene really is unlit and (1) measured emission.
            if (controlBox.meanLuma > 12.0 || controlBox.meanLuma * 3.0 > emissiveBox.meanLuma) {
                ZHLN::Println("    [INFO] control box meanLuma={:.1f} vs emissive {:.1f}", controlBox.meanLuma, emissiveBox.meanLuma);
                return std::unexpected(EmissiveShadingError::ControlNotDark);
            }

            // 4. Emission is local to the emitter. A corner of the same frame
            //    must not have brightened with it -- that would mean the new
            //    attachment is leaking into pixels the geometry never wrote,
            //    which is what an uncleared or mis-bound target looks like.
            if (emissiveCorner.meanLuma > 12.0) {
                ZHLN::Println("    [INFO] corner meanLuma={:.1f}", emissiveCorner.meanLuma);
                return std::unexpected(EmissiveShadingError::BackgroundNotDark);
            }

            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary.
auto RunEmissiveShadingSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<EmissiveShadingTestSuite>();
}
