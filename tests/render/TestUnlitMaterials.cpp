// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Render two chamfered boxes built straight from engine API calls, not an
// engine-authored flat-color stand-in and not a binary blob: the authored
// mesh carries many different face normals, and both the front and the
// sloping faces must show the same authored base color. A fully anisotropic
// lit box verifies both that lighting actually changed and that full-strength
// lit anisotropy never aliases the reserved unlit code.

#include "TestsFramework.hpp"
#include "helpers/ChamferedBoxMesh.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

enum class UnlitMaterialError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the headless unlit test engine.">{}) = 1,
    AssetUnavailable ZHLN_ANNOTATION(ZHLN::Description<"Authored chamfered mesh failed to upload.">{}),
    UnlitMaterialFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the orange and blue unlit materials.">{}),
    MaterialCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the lit control material.">{}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"The unlit frame could not be read back.">{}),
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
    const auto cameraComp = engine.GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
    if (!ZHLN::Test::ExpectTrue(cameraComp.has_value())) {
        return;
    }
    ZHLN::Camera& camera = cameraComp->camera;
    camera.position = JPH::Vec3(0.0f, 0.0f, 7.0f); // Same orbit the retired Khronos sample used, so the face windows below hold.
    camera.yaw      = -90.0f;
    camera.pitch    = 0.0f;
    camera.fov      = 45.0f;
}

// Uploads the authored chamfered solid with white vertex colors, so the
// rendered pixels equal the materials' base colors exactly: the shader
// multiplies albedo x baseColor x vertex color, and unlit has no albedo map.
// Meshlets and the BLAS mirror MeshBuilder so every draw path stays valid.
[[nodiscard]] auto UploadChamferedMesh(ZHLN::RenderContext& rc) -> ZHLN::Mesh {
    const auto box = ZHLN::Test::GltfFixtures::BuildChamferedBox();
    std::vector<ZHLN::VertexPosition>     positions;
    std::vector<ZHLN::VertexTangentFrame> frames;
    std::vector<ZHLN::VertexSurface>      surfaces;
    std::vector<uint32_t>                 indices;
    positions.reserve(ZHLN::Test::GltfFixtures::ChamferedBoxMesh::kVertexCount);
    frames.reserve(ZHLN::Test::GltfFixtures::ChamferedBoxMesh::kVertexCount);
    surfaces.reserve(ZHLN::Test::GltfFixtures::ChamferedBoxMesh::kVertexCount);
    indices.reserve(ZHLN::Test::GltfFixtures::ChamferedBoxMesh::kIndexCount);

    const auto white = ZHLN::Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    const auto uv0   = ZHLN::Math::PackUV(0.0f, 0.0f);
    for (uint32_t i = 0; i < ZHLN::Test::GltfFixtures::ChamferedBoxMesh::kVertexCount; ++i) {
        const float px = box.positions[i * 3 + 0];
        const float py = box.positions[i * 3 + 1];
        const float pz = box.positions[i * 3 + 2];
        const float nx = box.normals[i * 3 + 0];
        const float ny = box.normals[i * 3 + 1];
        const float nz = box.normals[i * 3 + 2];
        positions.push_back({{px, py, pz}});
        // Any stable perpendicular: unlit materials never sample the tangent.
        const float ax = std::abs(nx);
        const float ay = std::abs(ny);
        float       tx;
        float       ty;
        float       tz;
        if (ay <= ax && ay <= std::abs(nz)) {
            tx = -nz;
            ty = 0.0f;
            tz = nx;
        } else if (ax <= std::abs(nz)) {
            tx = 0.0f;
            ty = nz;
            tz = -ny;
        } else {
            tx = ny;
            ty = -nx;
            tz = 0.0f;
        }
        const float length = std::sqrt(tx * tx + ty * ty + tz * tz);
        frames.push_back(
            {.normal = ZHLN::Math::PackNormal(nx, ny, nz), .tangent = ZHLN::Math::PackNormal(tx / length, ty / length, tz / length, 1.0f)}
        );
        surfaces.push_back({.uv = uv0, .color = white, .uv1 = uv0});
    }
    for (uint16_t index: box.indices) {
        indices.push_back(index);
    }

    ZHLN::Mesh mesh {
        .posBuffer          = rc.CreateVertexBuffer(std::span {positions}),
        .tangentFrameBuffer = rc.CreateVertexBuffer(std::span {frames}),
        .surfaceBuffer      = rc.CreateVertexBuffer(std::span {surfaces}),
        .skinBuffer         = ZHLN::BufferHandle::Invalid,
        .indexBuffer        = rc.CreateIndexBuffer(std::span {indices}),
        .vertexCount        = static_cast<uint32_t>(positions.size()),
        .indexCount         = static_cast<uint32_t>(indices.size()),
    };
    if (const ZHLN::MeshletBuildResult built = ZHLN::BuildMeshlets(std::span {indices}, std::span {positions}); !built.Empty()) {
        mesh.meshletBuffer       = rc.CreateMeshletBuffer(built.meshlets);
        mesh.meshletVertexBuffer = rc.CreateStorageBuffer(std::span {built.vertices});
        mesh.meshletTriBuffer    = rc.CreateStorageBuffer(std::span {built.triangles});
        if (mesh.meshletBuffer == ZHLN::BufferHandle::Invalid || mesh.meshletVertexBuffer == ZHLN::BufferHandle::Invalid ||
            mesh.meshletTriBuffer == ZHLN::BufferHandle::Invalid) {
            rc.DestroyBuffer(mesh.meshletBuffer);
            rc.DestroyBuffer(mesh.meshletVertexBuffer);
            rc.DestroyBuffer(mesh.meshletTriBuffer);
            mesh.meshletBuffer       = ZHLN::BufferHandle::Invalid;
            mesh.meshletVertexBuffer = ZHLN::BufferHandle::Invalid;
            mesh.meshletTriBuffer    = ZHLN::BufferHandle::Invalid;
            mesh.meshletCount        = 0;
        } else {
            mesh.meshletCount = static_cast<uint32_t>(built.meshlets.size());
        }
    }
    if (auto built = rc.BuildMeshBLAS(mesh); !built) {
        if (!built.error().Is(ZHLN::RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("UploadChamferedMesh: Failed to build mesh BLAS: {}", built.error());
        }
    }
    return mesh;
}

// Mirrors PrefabFactory::CreateBox for a caller-built mesh: same registered
// assets and components, but Shape::None because no rebuild recipe exists.
auto SpawnUnlitChamfer(ZHLN::Engine& engine, float x, const ZHLN::Material& material) -> ZHLN::Entity {
    auto&            rc   = engine.GetRenderContext();
    auto&            reg  = engine.GetRegistry();
    const ZHLN::Mesh mesh = UploadChamferedMesh(rc);
    if (mesh.posBuffer == ZHLN::BufferHandle::Invalid || mesh.indexBuffer == ZHLN::BufferHandle::Invalid) {
        return ZHLN::Entity::Null();
    }

    const ZHLN::Entity e   = reg.Create();
    const std::string  tag = std::to_string(e.index);
    const ZHLN::AssetID    meshAsset = ZHLN::HashAssetID("unlit_chamfer_mesh_" + tag);
    const ZHLN::MaterialID matAsset  = ZHLN::HashAssetID("unlit_chamfer_mat_" + tag);
    rc.RegisterGPUMesh(meshAsset, mesh);
    rc.RegisterGPUMaterial(matAsset, material);

    const JPH::Vec3  position = JPH::Vec3(x, 0.0f, 0.0f);
    const JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(position, JPH::Quat::sIdentity(), JPH::Vec3::sReplicate(1.0f));
    reg.Add(e, ZHLN::Components::NameComponent {.name = ZHLN::String64("UnlitChamfer_" + tag)});
    reg.Add(e, ZHLN::Components::TransformComponent {.position = position, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)});
    reg.Add(e, ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
    reg.Add(e, ZHLN::Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = 2.0f});
    reg.Add(
        e, ZHLN::Components::OwnedMeshComponent {.meshAsset = meshAsset,
                                                 .mesh      = mesh,
                                                 .shape     = ZHLN::Components::OwnedMeshComponent::Shape::None,
                                                 .dimensions = JPH::Vec3::sZero(),
                                                 .color      = JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f)}
    );
    reg.Add(e, ZHLN::Components::PBRComponent {.roughness = material.roughnessFactor, .metallic = material.metallicFactor});
    return e;
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
        std::expected<void, ZHLN::ErrorCode> authored_meshes_are_uniform_and_ignore_the_sun() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless Authored Unlit", 512, 512);
            if (engine == nullptr) return std::unexpected(UnlitMaterialError::EngineInitFailed);
            ConfigureUnlitCapture(*engine);

            auto&      rc       = engine->GetRenderContext();
            const auto orangeMat = rc.CreateMaterial(ZHLN::MaterialDesc {.unlit = true, .baseColor = {1.0f, 0.21763764f, 0.0f, 1.0f}});
            const auto blueMat   = rc.CreateMaterial(ZHLN::MaterialDesc {.unlit = true, .baseColor = {0.0f, 0.21763764f, 1.0f, 1.0f}});
            if (!orangeMat || !blueMat) return std::unexpected(UnlitMaterialError::UnlitMaterialFailed);
            if (SpawnUnlitChamfer(*engine, -1.2f, *orangeMat) == ZHLN::Entity::Null() ||
                SpawnUnlitChamfer(*engine, 1.2f, *blueMat) == ZHLN::Entity::Null()) {
                return std::unexpected(UnlitMaterialError::AssetUnavailable);
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
            const auto dark = ZHLN::Test::Headless::Capture(*engine, "authored_unlit_dark.ppm");
            if (!dark.Valid()) return std::unexpected(UnlitMaterialError::CaptureFailed);
            reg.Patch<ZHLN::Components::LightComponent>(sun, [](auto& light) { light.intensity = 220.0f; });
            ZHLN::Test::Headless::TickFrames(*engine, 4);
            const auto bright = ZHLN::Test::Headless::Capture(*engine, "authored_unlit_bright.ppm");
            if (!bright.Valid()) return std::unexpected(UnlitMaterialError::CaptureFailed);

            const MeanRgb orange = Mean(bright, kOrangeFaces[0]);
            const MeanRgb blue   = Mean(bright, kBlueFaces[0]);
            ZHLN::Println("    [INFO] unlit front orange=({:.1f},{:.1f},{:.1f}) blue=({:.1f},{:.1f},{:.1f}) lit box R dark={:.1f} bright={:.1f}",
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
            const auto cameraComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(cameraComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            ZHLN::Camera& camera = cameraComp->camera;
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
