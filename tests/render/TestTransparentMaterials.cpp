// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestTransparentMaterials.cpp
//
// Alpha-blended materials skip the G-Buffer (IsForwardOnly / alphaMode 2) and
// composite in ForwardPass with SRC_ALPHA, ONE_MINUS_SRC_ALPHA. An opaque
// twin of the same geometry writes the G-Buffer and occludes whatever sits
// behind it. This suite is the difference between those two pipelines:
//
//   1. a cyan glass pane in front of a red wall still shows the wall;
//   2. the same pane with blending off hides the wall.
//
// CreateBox does not set DrawFlags::ExcludeFromTLAS the way prefab Instantiation
// does for alphaMode 2, so the glass mesh is patched after spawn. TAA is off
// so a static scene stays static. Mesh shading is left alone (Hybrid).

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <string>

enum class TransparentMaterialError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize the headless Engine for the transparent-material scene.">{}) = 1,
    MaterialCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"CreateMaterial rejected the glass, the opaque twin, or the wall.">{}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"The rendered frame could not be read back.">{}),
    GlassHidTheWall ZHLN_ANNOTATION(ZHLN::Description<"A blended pane occluded the opaque surface behind it -- it is not compositing in ForwardPass.">{}),
    GlassDidNotTint ZHLN_ANNOTATION(ZHLN::Description<"The wall shows through the pane but the pane contributed no colour of its own.">{}),
    OpaqueTwinDidNotOcclude ZHLN_ANNOTATION(ZHLN::Description<"The non-blended twin of the pane still showed the wall -- both materials took the same path.">{}),
};

namespace {

using ZHLN::Test::Image::NormalizedRect;
using ZHLN::Test::Image::SubRegionStats;

// Camera looks down -Z at the origin. The wall is a large slab at z = -1 so it
// still fills a strip of the frame beside the pane; the pane is a 1x1 slab at
// z = 0. The centre of the frame therefore looks through the pane at the wall,
// and a strip to the left of the pane still sees the wall with nothing in front.
constexpr NormalizedRect kThroughPane {.x0 = 0.42, .y0 = 0.42, .x1 = 0.58, .y1 = 0.58};
constexpr NormalizedRect kWallOnly {.x0 = 0.12, .y0 = 0.42, .x1 = 0.24, .y1 = 0.58};

constexpr std::array<float, 4> kWallRed {1.0f, 0.05f, 0.05f, 1.0f};
constexpr std::array<float, 4> kPaneCyan {0.05f, 0.85f, 0.95f, 0.40f};
constexpr std::array<float, 4> kPaneOpaqueCyan {0.05f, 0.85f, 0.95f, 1.0f};

enum class PaneKind : uint8_t { None, Glass, Opaque };

enum class SceneBuild : uint8_t { Ok, Material };

[[nodiscard]] auto SpawnWallAndPane(ZHLN::Engine& engine, PaneKind pane) -> SceneBuild {
    auto& registry  = engine.GetRegistry();
    auto& renderCtx = engine.GetRenderContext();

    for (const ZHLN::Entity settings: registry.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        registry.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& pp) {
            pp.fullBright        = 1;
            pp.vignetteIntensity = 0.0f;
            pp.glowIntensity     = 0.0f;
            pp.bloomStrength     = 0.0f;
            pp.enableSSR         = 0;
            pp.enableRTR         = 0;
            pp.giMode            = 0;
            pp.skyZenith         = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            pp.skyHorizon        = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            pp.skyGround         = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        });
    }

    // ForwardPass still shades; a sun on the camera-facing +Z faces keeps the
    // glass from compositing as an unlit black film on top of the wall albedo.
    const ZHLN::Entity sun = registry.Create();
    registry.Add(
        sun, ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 0.0f, 8.0f)},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 220.0f, .direction = JPH::Vec3(0.0f, 0.0f, 1.0f)
        }
    );

    const auto wallMat = ZHLN::CreativeWorksFactory::CreateMaterial(
        renderCtx,
        ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 1.0f, .baseColor = kWallRed}
    );
    if (!wallMat.has_value()) {
        return SceneBuild::Material;
    }

    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(4.0f, 3.0f, 0.08f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, -1.0), .createPhysics = false, .materialOverride = *wallMat}
    );

    if (pane != PaneKind::None) {
        const bool glass = pane == PaneKind::Glass;
        const auto paneMat = ZHLN::CreativeWorksFactory::CreateMaterial(
            renderCtx,
            ZHLN::CreativeWorksFactory::MaterialDesc {
                .doubleSided = true,
                .alphaBlend  = glass,
                .alphaMode   = glass ? 2u : 0u,
                .metallic    = 0.0f,
                .roughness   = 0.15f,
                .baseColor   = glass ? kPaneCyan : kPaneOpaqueCyan
            }
        );
        if (!paneMat.has_value()) {
            return SceneBuild::Material;
        }
        if (glass && paneMat->alphaMode != 2u) {
            return SceneBuild::Material;
        }

        const ZHLN::Entity paneEnt = ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(1.0f, 1.0f, 0.04f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *paneMat}
        );
        if (glass) {
            registry.Patch<ZHLN::Components::MeshComponent>(paneEnt, [](auto& mesh) { mesh.flags |= ZHLN::DrawFlags::ExcludeFromTLAS; });
        }
    }

    auto& camera    = engine.GetCamera();
    camera.position = JPH::Vec3(0.0f, 0.0f, 5.0f);
    camera.yaw      = -90.0f;
    camera.pitch    = 0.0f;
    camera.fov      = 60.0f;
    return SceneBuild::Ok;
}

struct PaneMeasurement {
    SubRegionStats          through;
    SubRegionStats          wall;
    TransparentMaterialError error = TransparentMaterialError::CaptureFailed;
    bool                     valid = false;
};

[[nodiscard]] auto MeasurePane(PaneKind pane, const std::string& ppmPath) -> PaneMeasurement {
    PaneMeasurement out;
    const char*     name = pane == PaneKind::Glass ? "Headless Transparent Glass" :
                           pane == PaneKind::Opaque ? "Headless Transparent Opaque Twin" :
                                                      "Headless Transparent Wall Only";
    const auto engine = ZHLN::Test::Headless::AcquireEngine(name);
    if (engine == nullptr) {
        out.error = TransparentMaterialError::EngineInitFailed;
        return out;
    }
    ZHLN::Test::Headless::DisableTAA(*engine);
    if (SpawnWallAndPane(*engine, pane) != SceneBuild::Ok) {
        out.error = TransparentMaterialError::MaterialCreationFailed;
        return out;
    }
    ZHLN::Test::Headless::TickFrames(*engine, 6);
    const auto frame = ZHLN::Test::Headless::Capture(*engine, ppmPath);
    if (!frame.Valid()) {
        out.error = TransparentMaterialError::CaptureFailed;
        return out;
    }
    out.through = ZHLN::Test::Image::MeasureSubRegion(frame, kThroughPane);
    out.wall    = ZHLN::Test::Image::MeasureSubRegion(frame, kWallOnly);
    out.valid   = true;
    return out;
}

} // namespace

struct TransparentMaterialsTestSuite {
    TransparentMaterialsTestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~TransparentMaterialsTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        /// Blended glass composites over the wall; the opaque twin occludes it.
        ///
        /// Three frames of the same camera and wall, differing only in the pane:
        /// none, alpha-blended cyan, and an opaque cyan twin. The through-pane
        /// window has to stay redder than the opaque-twin frame (the wall is
        /// showing through) and pick up cyan relative to the no-pane frame (the
        /// glass contributed). The opaque twin has to flip that window to cyan
        /// and drop the red, which is what proves the two materials did not take
        /// the same draw path.
        std::expected<void, ZHLN::Error> glass_composites_over_the_wall_opaque_occludes() {
            const PaneMeasurement clear  = MeasurePane(PaneKind::None, "transparent_wall.ppm");
            const PaneMeasurement glass  = MeasurePane(PaneKind::Glass, "transparent_glass.ppm");
            const PaneMeasurement opaque = MeasurePane(PaneKind::Opaque, "transparent_opaque.ppm");
            if (!clear.valid) {
                return std::unexpected(clear.error);
            }
            if (!glass.valid) {
                return std::unexpected(glass.error);
            }
            if (!opaque.valid) {
                return std::unexpected(opaque.error);
            }

            ZHLN::Println(
                "    [INFO] wall-only  through meanRGB=({:.1f}, {:.1f}, {:.1f}) red={} | side meanRGB=({:.1f}, {:.1f}, {:.1f})",
                clear.through.meanR, clear.through.meanG, clear.through.meanB, clear.through.dominantRed, clear.wall.meanR, clear.wall.meanG, clear.wall.meanB
            );
            ZHLN::Println(
                "    [INFO] glass      through meanRGB=({:.1f}, {:.1f}, {:.1f}) red={} cyan={} | side meanRGB=({:.1f}, {:.1f}, {:.1f})",
                glass.through.meanR, glass.through.meanG, glass.through.meanB, glass.through.dominantRed, glass.through.cyanMix, glass.wall.meanR,
                glass.wall.meanG, glass.wall.meanB
            );
            ZHLN::Println(
                "    [INFO] opaque twin through meanRGB=({:.1f}, {:.1f}, {:.1f}) red={} cyan={}", opaque.through.meanR, opaque.through.meanG,
                opaque.through.meanB, opaque.through.dominantRed, opaque.through.cyanMix
            );

            // The uncovered wall strip is the same in every frame, so a
            // difference in the through-pane window cannot be a lighting or
            // exposure drift.
            if (clear.wall.meanR < 40.0 || glass.wall.meanR < 40.0) {
                return std::unexpected(TransparentMaterialError::CaptureFailed);
            }

            // 1. Glass does not hide the wall: the through-pane window keeps
            //    more red than the opaque-twin frame of the same geometry.
            const bool wallShowsThrough = glass.through.meanR > opaque.through.meanR + 15.0 && glass.through.meanR > 20.0;
            if (!wallShowsThrough) {
                return std::unexpected(TransparentMaterialError::GlassHidTheWall);
            }

            // 2. Glass is not a no-op: it lifts green/blue relative to the
            //    uncovered wall.
            const bool paneTints = glass.through.meanG > clear.through.meanG + 8.0 && glass.through.meanB > clear.through.meanB + 8.0;
            if (!paneTints) {
                return std::unexpected(TransparentMaterialError::GlassDidNotTint);
            }

            // 3. The opaque twin of the same pane hides the wall: cyan wins,
            //    red collapses.
            const bool twinOccludes = opaque.through.meanG > opaque.through.meanR + 15.0 && opaque.through.meanB > opaque.through.meanR + 15.0 &&
                                      opaque.through.meanR + 25.0 < clear.through.meanR;
            if (!twinOccludes) {
                return std::unexpected(TransparentMaterialError::OpaqueTwinDidNotOcclude);
            }

            return {};
        }
    };
};

auto RunTransparentMaterialsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TransparentMaterialsTestSuite>();
}
