// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Render the actual Khronos UnlitTest GLB, not an engine-authored flat-color
// stand-in. It contains two bevelled meshes with many different face normals:
// both the front and the sloping faces must show the same authored base color.
// A fully anisotropic lit box verifies both that lighting actually changed and
// that full-strength lit anisotropy never aliases the reserved unlit code.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <glTF/GLTFImporter.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

enum class UnlitMaterialError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the headless unlit test engine.">{}) = 1,
    AssetUnavailable ZHLN_ANNOTATION(ZHLN::Description<"Pinned Khronos UnlitTest.glb fixture is missing.">{}),
    PrefabLoadFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not import and spawn both Khronos UnlitTest meshes.">{}),
    MaterialCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the lit control material.">{}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"The UnlitTest frame could not be read back.">{}),
    WrongBaseColor ZHLN_ANNOTATION(ZHLN::Description<"The imported orange or blue object lost its authored base color.">{}),
    FaceShaded ZHLN_ANNOTATION(ZHLN::Description<"Different normals on the same unlit object rendered different colors.">{}),
    LightAffectedUnlit ZHLN_ANNOTATION(ZHLN::Description<"Changing the sun affected an unlit face.">{}),
    LightDidNotChange ZHLN_ANNOTATION(ZHLN::Description<"The sun change failed to affect the lit control object.">{}),
    TextureCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not upload the unlit alpha-coverage texture.">{}),
    MaskCoverageIgnored ZHLN_ANNOTATION(ZHLN::Description<"Unlit MASK failed to cut holes using the base-color texture's alpha.">{}),
    BlendedCoverageIgnored ZHLN_ANNOTATION(ZHLN::Description<"Unlit BLEND failed to composite with base-color texture and factor alpha.">{}),
};

namespace {

using ZHLN::Test::Image::NormalizedRect;
using ZHLN::Test::Image::RgbImage;

struct MeanRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

[[nodiscard]] auto Mean(const RgbImage& image, NormalizedRect rect) -> MeanRgb {
    const auto stats = ZHLN::Test::Image::MeasureSubRegion(image, rect);
    return {stats.meanR, stats.meanG, stats.meanB};
}

[[nodiscard]] auto MaxChannelDifference(MeanRgb a, MeanRgb b) -> double {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

void ConfigureUnlitCapture(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();
    ZHLN::Test::Headless::DisableTAA(engine);
    for (const ZHLN::Entity settings: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& pp) {
            pp.fullBright        = 0; // fullBright bypasses all shading: it cannot detect an unlit regression.
            pp.giMode            = 0;
            pp.enableSSR         = 0;
            pp.enableRTR         = 0;
            pp.ambientExposure   = 0.0f;
            pp.exposure          = 1.0f;
            pp.tonemapper        = 3;
            pp.vignetteIntensity = 0.0f;
            pp.glowIntensity     = 0.0f;
            pp.bloomStrength     = 0.0f;
        });
    }
    auto& camera    = engine.GetCamera();
    camera.position = JPH::Vec3(0.0f, 0.0f, 7.0f); // Fidelity Generator's khronos-UnlitTest orbit radius.
    camera.yaw      = -90.0f;
    camera.pitch    = 0.0f;
    camera.fov      = 45.0f;
}

[[nodiscard]] auto ReadUnlitFixture() -> std::vector<uint8_t> {
    std::ifstream file(std::string(ZHLN_TEST_SOURCE_DIR) + "/tests/render/assets/UnlitTest.glb", std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The fixture meshes are centred at world X=-1.2 and X=+1.2, with unit radius;
// at camera Z=7 a face normal points forward (y=0.50), up (y=0.32), and out
// towards the side (x=0.10 / 0.90). All windows are well inside their faces,
// not on polygon edges where raster coverage/antialiasing would interfere.
constexpr std::array<NormalizedRect, 3> kOrangeFaces {{
    {.x0 = 0.25, .y0 = 0.47, .x1 = 0.27, .y1 = 0.53}, // Front.
    {.x0 = 0.25, .y0 = 0.32, .x1 = 0.27, .y1 = 0.33}, // Top bevel.
    {.x0 = 0.10, .y0 = 0.47, .x1 = 0.11, .y1 = 0.53}, // Side bevel.
}};
constexpr std::array<NormalizedRect, 3> kBlueFaces {{
    {.x0 = 0.73, .y0 = 0.47, .x1 = 0.75, .y1 = 0.53},
    {.x0 = 0.73, .y0 = 0.32, .x1 = 0.75, .y1 = 0.33},
    {.x0 = 0.89, .y0 = 0.47, .x1 = 0.90, .y1 = 0.53},
}};
constexpr NormalizedRect kLitControl {.x0 = 0.49, .y0 = 0.77, .x1 = 0.51, .y1 = 0.79};

} // namespace

struct UnlitMaterialsTestSuite {
    UnlitMaterialsTestSuite() { ZHLN::Test::Headless::BeginSession(); }
    ~UnlitMaterialsTestSuite() { ZHLN::Test::Headless::EndSession(); }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> official_meshes_are_uniform_and_ignore_the_sun() {
            const auto bytes = ReadUnlitFixture();
            if (bytes.empty()) return std::unexpected(UnlitMaterialError::AssetUnavailable);
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless Khronos UnlitTest", 512, 512);
            if (engine == nullptr) return std::unexpected(UnlitMaterialError::EngineInitFailed);
            ConfigureUnlitCapture(*engine);

            const auto* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(
                engine->GetRenderContext(), engine->GetAssetManager(), std::span {bytes}, "gpu_unlit_test.glb"
            );
            if (prefab == nullptr || prefab->parts.size() != 2) return std::unexpected(UnlitMaterialError::PrefabLoadFailed);
            std::array<ZHLN::Entity, 3> entities {};
            if (ZHLN::PrefabFactory::InstantiatePrefab(
                    *engine, *prefab, {.createPhysics = false, .emissiveVirtualLights = false}, entities.data(), static_cast<uint32_t>(entities.size())
                ) < 3) {
                return std::unexpected(UnlitMaterialError::PrefabLoadFailed);
            }

            auto& reg = engine->GetRegistry();
            const ZHLN::Entity sun = reg.Create();
            reg.Add(sun, ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 0.0f, 8.0f)},
                    ZHLN::Components::LightComponent {.type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f),
                                                      .intensity = 0.0f, .direction = JPH::Vec3(0.0f, 0.0f, 1.0f)});
            const auto controlMaterial = engine->GetRenderContext().CreateMaterial(
                ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.5f, 0.5f, 0.5f, 1.0f}, .anisotropyStrength = 1.0f}
            );
            if (!controlMaterial) return std::unexpected(UnlitMaterialError::MaterialCreationFailed);
            ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(0.45f, 0.45f, 0.45f),
                {.position = JPH::RVec3(0.0, -1.6, 0.0), .createPhysics = false, .materialOverride = *controlMaterial});

            ZHLN::Test::Headless::TickFrames(*engine, 6);
            const auto dark = ZHLN::Test::Headless::Capture(*engine, "khronos_unlit_dark.ppm");
            if (!dark.Valid()) return std::unexpected(UnlitMaterialError::CaptureFailed);
            reg.Patch<ZHLN::Components::LightComponent>(sun, [](auto& light) { light.intensity = 220.0f; });
            ZHLN::Test::Headless::TickFrames(*engine, 4);
            const auto bright = ZHLN::Test::Headless::Capture(*engine, "khronos_unlit_bright.ppm");
            if (!bright.Valid()) return std::unexpected(UnlitMaterialError::CaptureFailed);

            const MeanRgb orange = Mean(bright, kOrangeFaces[0]);
            const MeanRgb blue   = Mean(bright, kBlueFaces[0]);
            ZHLN::Println("    [INFO] UnlitTest front orange=({:.1f},{:.1f},{:.1f}) blue=({:.1f},{:.1f},{:.1f}) lit box R dark={:.1f} bright={:.1f}",
                          orange.r, orange.g, orange.b, blue.r, blue.g, blue.b,
                          Mean(dark, kLitControl).r, Mean(bright, kLitControl).r);
            if (orange.r < 90.0 || orange.r < 1.6 * orange.g || orange.b > 0.25 * orange.r ||
                blue.b < 90.0 || blue.b < 1.6 * blue.g || blue.r > 0.25 * blue.b) {
                return std::unexpected(UnlitMaterialError::WrongBaseColor);
            }
            for (size_t face = 0; face < kOrangeFaces.size(); ++face) {
                const MeanRgb orangeFace = Mean(bright, kOrangeFaces[face]);
                const MeanRgb blueFace   = Mean(bright, kBlueFaces[face]);
                if (MaxChannelDifference(orange, orangeFace) > 10.0 || MaxChannelDifference(blue, blueFace) > 10.0) {
                    return std::unexpected(UnlitMaterialError::FaceShaded);
                }
                if (MaxChannelDifference(Mean(dark, kOrangeFaces[face]), orangeFace) > 8.0 ||
                    MaxChannelDifference(Mean(dark, kBlueFaces[face]), blueFace) > 8.0) {
                    return std::unexpected(UnlitMaterialError::LightAffectedUnlit);
                }
            }
            if (Mean(bright, kLitControl).r < Mean(dark, kLitControl).r + 12.0) {
                return std::unexpected(UnlitMaterialError::LightDidNotChange);
            }
            return {};
        }

        // Unlit changes the shading model, not glTF's independent alpha mode.
        // The left pane has a texture-driven MASK, the right pane has BLEND
        // with half coverage. Both tint the same alpha-striped white base-color
        // texture cyan over a red wall; only BLEND may show both colors at once.
        std::expected<void, ZHLN::ErrorCode> masked_and_blended_unlit_keep_texture_coverage() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless unlit texture coverage");
            if (engine == nullptr) return std::unexpected(UnlitMaterialError::EngineInitFailed);
            ConfigureUnlitCapture(*engine);
            auto& camera = engine->GetCamera();
            camera.position = JPH::Vec3(0.0f, 0.0f, 5.0f);
            camera.fov = 60.0f;
            auto& rc = engine->GetRenderContext();

            constexpr uint32_t side = 64;
            std::vector<uint8_t> rgba(side * side * 4u, 255u);
            for (uint32_t y = 0; y < side / 2; ++y) {
                for (uint32_t x = 0; x < side; ++x) {
                    rgba[(static_cast<size_t>(y) * side + x) * 4u + 3u] = 0u;
                }
            }
            const auto texture = rc.CreateTexture(std::as_bytes(std::span {rgba}), {side, side}, true);
            if (!texture) return std::unexpected(UnlitMaterialError::TextureCreationFailed);
            ZHLN::defer _([&] {
                engine->ClearScene(); // Release scene-owned mesh buffers before their texture.
                rc.UnloadTexture(*texture);
            });

            const auto wall = rc.CreateMaterial(ZHLN::MaterialDesc {
                .metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.0f, 0.0f, 0.0f, 1.0f}, .emissive = {0.8f, 0.0f, 0.0f, 1.0f}
            });
            const auto mask = rc.CreateMaterial(ZHLN::MaterialDesc {
                .doubleSided = true, .unlit = true, .alphaMode = 1, .alphaCutoff = 0.5f,
                .baseColor = {0.0f, 1.0f, 1.0f, 1.0f}, .albedoMap = *texture
            });
            const auto blend = rc.CreateMaterial(ZHLN::MaterialDesc {
                .doubleSided = true, .unlit = true, .alphaBlend = true, .alphaMode = 2,
                .baseColor = {0.0f, 1.0f, 1.0f, 0.5f}, .albedoMap = *texture
            });
            if (!wall || !mask || !blend) return std::unexpected(UnlitMaterialError::MaterialCreationFailed);
            ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(4.0f, 3.0f, 0.04f),
                {.position = JPH::RVec3(0.0, 0.0, -1.0), .createPhysics = false, .materialOverride = *wall});
            ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(0.55f, 0.6f, 0.02f),
                {.position = JPH::RVec3(-0.7, 0.0, 0.0), .createPhysics = false, .materialOverride = *mask});
            ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(0.55f, 0.6f, 0.02f),
                {.position = JPH::RVec3(0.7, 0.0, 0.0), .createPhysics = false, .materialOverride = *blend});

            ZHLN::Test::Headless::TickFrames(*engine, 6);
            const auto frame = ZHLN::Test::Headless::Capture(*engine, "unlit_alpha_coverage.ppm");
            if (!frame.Valid()) return std::unexpected(UnlitMaterialError::CaptureFailed);
            const MeanRgb hole    = Mean(frame, {.x0 = 0.40, .y0 = 0.44, .x1 = 0.42, .y1 = 0.46});
            const MeanRgb solid   = Mean(frame, {.x0 = 0.40, .y0 = 0.54, .x1 = 0.42, .y1 = 0.56});
            const MeanRgb clear   = Mean(frame, {.x0 = 0.58, .y0 = 0.44, .x1 = 0.60, .y1 = 0.46});
            const MeanRgb covered = Mean(frame, {.x0 = 0.58, .y0 = 0.54, .x1 = 0.60, .y1 = 0.56});
            ZHLN::Println("    [INFO] unlit alpha mask R/B hole={:.1f}/{:.1f} solid={:.1f}/{:.1f}; blend clear={:.1f}/{:.1f} covered={:.1f}/{:.1f}",
                          hole.r, hole.b, solid.r, solid.b, clear.r, clear.b, covered.r, covered.b);
            if (hole.r < 80.0 || solid.b < 80.0 || hole.r < solid.r + 30.0 || solid.b < hole.b + 30.0) {
                return std::unexpected(UnlitMaterialError::MaskCoverageIgnored);
            }
            if (clear.r < 80.0 || covered.r < 30.0 || covered.b < clear.b + 30.0 ||
                covered.b > solid.b - 20.0 || covered.r > clear.r - 15.0) {
                return std::unexpected(UnlitMaterialError::BlendedCoverageIgnored);
            }
            return {};
        }
    };
};

auto RunUnlitMaterialsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UnlitMaterialsTestSuite>();
}
