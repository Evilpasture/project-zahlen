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
    GlowHaloMissing
    [[= ZHLN::Description<"An emitter under the bloom threshold cast no glow past its silhouette -- the glow layer is not feeding the bloom chain.">{}]],
    DeviceLost[[= ZHLN::Description<"The Vulkan device was lost and the engine could not recover.">{}]],
};

namespace {

using ZHLN::Test::Image::NormalizedRect;
using ZHLN::Test::Image::SubRegionStats;

// The box sits at the origin, the camera looks straight at it down -Z, so the
// emitter covers the middle of the frame and the corners see only sky.
constexpr NormalizedRect kBoxWindow {.x0 = 0.40, .y0 = 0.40, .x1 = 0.60, .y1 = 0.60};
constexpr NormalizedRect kCornerWindow {.x0 = 0.02, .y0 = 0.02, .x1 = 0.18, .y1 = 0.18};

// A band just above the box, outside its silhouette. The box is 2 units tall,
// 6 away, through a 60-degree vertical FOV, so it covers y 0.36 .. 0.64 -- and
// unlike the horizontal extent that does not depend on the aspect ratio, so
// this window stays off the geometry whatever resolution the fixture picks.
// Anything measured here arrived by blur, not by rasterisation.
constexpr NormalizedRect kHaloWindow {.x0 = 0.44, .y0 = 0.22, .x1 = 0.56, .y1 = 0.32};

// A strong green emitter: green is the one channel neither the sky gradient
// (blue-dominant) nor the default clear colour leans on, so a green-dominant
// pixel in the box window can only be the emissive material.
constexpr std::array<float, 4> kEmissiveGreen {0.0f, 3.0f, 0.0f, 1.0f};
constexpr std::array<float, 4> kNoEmission {0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::array<float, 4> kBoxBaseColor {0.05f, 0.05f, 0.05f, 1.0f};

// Deliberately under the bloom bright-pass threshold: luma(0, 0.8, 0) = 0.57
// against a threshold of 1.0. This is the ordinary glTF case -- emissiveFactor
// is clamped to [0,1] unless the asset ships KHR_materials_emissive_strength --
// and it is the case that stopped glowing when emission moved out of the albedo
// attachment and stopped being multiplied by incident light.
constexpr std::array<float, 4> kDimNeonGreen {0.0f, 0.8f, 0.0f, 1.0f};

/// Builds the unlit scene: one box at the origin, no lights of any kind, and
/// ambient/GI dialled out so nothing but emission can brighten a surface.
///
/// Returns false when material creation fails, which is a setup failure rather
/// than a rendering result.
[[nodiscard]] bool BuildUnlitBoxScene(ZHLN::Engine& engine, const std::array<float, 4>& emissiveFactor) {
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
                       .emissive  = emissiveFactor
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

/// One rendered frame reduced to what the assertions need: the box window, a
/// background corner, and the box's green share. `valid` distinguishes a failed
/// capture from a legitimately black frame.
struct UnlitMeasurement {
    SubRegionStats box;
    SubRegionStats halo;
    SubRegionStats corner;
    double         greenShare = 0.0;
    bool           valid      = false;
};

/// Renders the unlit scene once and measures it.
[[nodiscard]] auto MeasureUnlitBox(const std::array<float, 4>& emissiveFactor, const std::string& ppmPath) -> UnlitMeasurement {
    UnlitMeasurement out;

    const bool emissive = emissiveFactor[1] > 0.0f;

    // Pooled: both halves of the comparison run on the same device with the
    // scene reset in between, so a difference in the frames cannot come from
    // a difference in the engine.
    const auto engine = ZHLN::Test::Headless::AcquireEngine(emissive ? "Headless Emissive Unlit" : "Headless Emissive Control");
    if (engine == nullptr) {
        return out;
    }
    ZHLN::Test::Headless::DisableTAA(*engine);

    if (!BuildUnlitBoxScene(*engine, emissiveFactor)) {
        return out;
    }

    ZHLN::Test::Headless::TickFrames(*engine, 4);
    const auto frame = ZHLN::Test::Headless::Capture(*engine, ppmPath);
    if (!frame.Valid()) {
        return out;
    }

    out.box    = ZHLN::Test::Image::MeasureSubRegion(frame, kBoxWindow);
    out.halo   = ZHLN::Test::Image::MeasureSubRegion(frame, kHaloWindow);
    out.corner = ZHLN::Test::Image::MeasureSubRegion(frame, kCornerWindow);
    // Not MeasureSubRegion::dominantGrn: its 45 floor is absolute, and an
    // emitter this dim never reaches it however green it is.
    out.greenShare = ZHLN::Test::Image::DominantHueShare(frame, kBoxWindow, ZHLN::Test::Image::HueChannel::Green);
    out.valid      = true;
    return out;
}

} // namespace

struct EmissiveShadingTestSuite {
    EmissiveShadingTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~EmissiveShadingTestSuite() {
        ZHLN::Test::Headless::EndSession();
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
            const UnlitMeasurement emissive = MeasureUnlitBox(kEmissiveGreen, "emissive_unlit.ppm");
            if (!emissive.valid) {
                return std::unexpected(EmissiveShadingError::CaptureFailed);
            }

            const UnlitMeasurement control = MeasureUnlitBox(kNoEmission, "emissive_unlit_control.ppm");
            if (!control.valid) {
                return std::unexpected(EmissiveShadingError::CaptureFailed);
            }

            const SubRegionStats& emissiveBox    = emissive.box;
            const SubRegionStats& emissiveCorner = emissive.corner;
            const SubRegionStats& controlBox     = control.box;

            ZHLN::Println(
                "    [INFO] emissive box meanLuma={:.1f} maxLuma={:.1f} meanRGB=({:.1f}, {:.1f}, {:.1f}) greenShare={:.2f} | control box "
                "meanLuma={:.1f} | corner meanLuma={:.1f}",
                emissiveBox.meanLuma, emissiveBox.maxLuma, emissiveBox.meanR, emissiveBox.meanG, emissiveBox.meanB, emissive.greenShare,
                controlBox.meanLuma, emissiveCorner.meanLuma
            );

            // 1. The emitter is visible at all. Before the fix this window was
            //    the clear colour: the lighting pass multiplied the baked-in
            //    emission by an incident light of zero.
            //
            //    The floor is deliberately a degenerate-case guard, not a
            //    calibration: what actually proves emission happened is (3),
            //    which compares this window against the identical box with the
            //    emissive factor removed. Hardware measures ~23 here, and the
            //    control window is capped at 12 by (3), so 16 separates "lit
            //    by its own emission" from "as dark as the control" without
            //    pinning the test to one tonemapper's output.
            if (emissiveBox.meanLuma < 16.0) {
                return std::unexpected(EmissiveShadingError::EmissiveWentDark);
            }

            // 2. It is the colour it emits, not a grey wash.
            if (emissive.greenShare < 0.5 || emissiveBox.meanG <= emissiveBox.meanR || emissiveBox.meanG <= emissiveBox.meanB) {
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

        /**
         * A neon material glows past its own edges even when it never crosses
         * the bloom threshold.
         *
         * Emission owning a G-Buffer channel fixed the shading (see the test
         * above) and cost the halo: it is no longer multiplied by incident
         * light, so an emitter tops out at its emissiveFactor, and glTF clamps
         * that to [0,1] without KHR_materials_emissive_strength. The bright
         * pass thresholds at luma 1.0, so an ordinary neon material -- 0.57
         * here -- contributed roughly 3% of itself to the bloom chain and the
         * glow vanished.
         *
         * bloom_threshold_cs now feeds the emissive channel into the same
         * cascade ungated, the way Babylon.js's GlowLayer does. The band above
         * the box is outside the geometry, so anything bright in it arrived by
         * blur; the far corner is the control for "the whole frame lifted".
         */
        std::expected<void, ZHLN::Error> a_dim_emitter_still_glows_past_its_silhouette() {
            const UnlitMeasurement neon = MeasureUnlitBox(kDimNeonGreen, "emissive_glow_halo.ppm");
            if (!neon.valid) {
                return std::unexpected(EmissiveShadingError::CaptureFailed);
            }

            ZHLN::Println(
                "    [INFO] dim emitter: box meanG={:.1f} | halo meanRGB=({:.1f}, {:.1f}, {:.1f}) meanLuma={:.1f} | corner meanLuma={:.1f}",
                neon.box.meanG, neon.halo.meanR, neon.halo.meanG, neon.halo.meanB, neon.halo.meanLuma, neon.corner.meanLuma
            );

            // 1. The emitter itself is still visible. If this fails the glow is
            //    not the problem -- read the test above first.
            if (neon.box.meanG <= neon.halo.meanG) {
                return std::unexpected(EmissiveShadingError::EmissiveWentDark);
            }

            // 2. There is light outside the silhouette, it is the colour the
            //    box emits, and it is a local halo rather than a lifted frame.
            //    Margins are loose on purpose: the assertion is "a green glow
            //    is present and falls off with distance", not a calibration of
            //    the Kawase cascade.
            const bool brighterThanBackground = neon.halo.meanG > neon.corner.meanG + 4.0;
            const bool greenDominant          = neon.halo.meanG > neon.halo.meanR + 3.0 && neon.halo.meanG > neon.halo.meanB + 3.0;
            const bool fallsOff               = neon.corner.meanLuma < 12.0;

            if (!brighterThanBackground || !greenDominant || !fallsOff) {
                return std::unexpected(EmissiveShadingError::GlowHaloMissing);
            }

            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary.
auto RunEmissiveShadingSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<EmissiveShadingTestSuite>();
}
