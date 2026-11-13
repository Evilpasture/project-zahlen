// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// A small render of TextureTransformMultiTest's three-column UV arrangement.
// The two transformed quads must sample the same part of an atlas as the
// pre-transformed Sample quad. This covers Material -> draw InstanceData ->
// vertex/fragment shaders, which testing the imported Material alone cannot.

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

enum class TextureTransformError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not initialize the headless texture-transform test.">{}) = 1,
    ResourceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the texture-transform test's atlas, materials or meshes.">{}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not read back the texture-transform test frame.">{}),
    ControlMissing ZHLN_ANNOTATION(ZHLN::Description<"The untransformed Sample column did not show all four atlas regions.">{}),
    TransformedUV0Mismatch ZHLN_ANNOTATION(ZHLN::Description<"TEXCOORD_0 plus KHR_texture_transform did not match the Sample column.">{}),
    TransformedUV1Mismatch ZHLN_ANNOTATION(ZHLN::Description<"TEXCOORD_1 plus KHR_texture_transform did not match the Sample column.">{}),
};

namespace {

// CTAD gives std::span<T, 4> for std::array<T, 4>; all typed buffer helpers
// must accept it (including a const element type), not only dynamic spans.
static_assert(requires(ZHLN::RenderContext& rc, ZHLN::BufferID handle,
                       std::array<ZHLN::VertexPosition, 4>& vertices,
                       const std::array<ZHLN::VertexPosition, 4>& immutable) {
    rc.CreateBuffer<ZHLN::BufferUsage::Storage>(std::span {vertices});
    rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {immutable});
    rc.UpdateBuffer(handle, std::span {vertices});
});

constexpr std::array<float, 3> kPanelX {-1.2f, 0.0f, 1.2f};
constexpr float                kCameraDistance = 4.0f;
constexpr float                kUvLow          = 0.1884253f;
constexpr float                kUvHigh         = 0.8115747f;

// TEXCOORD_0 or TEXCOORD_1 on the test columns of the Khronos asset.
constexpr std::array kTestUV {
    std::array {kUvLow, kUvLow}, std::array {kUvLow, kUvHigh},
    std::array {kUvHigh, kUvHigh}, std::array {kUvHigh, kUvLow}
};
// Its Sample column uses pre-transformed UVs that select the checkmark atlas
// region without KHR_texture_transform. They differ slightly from the exact
// transformed coordinates because the control has its own border margins.
constexpr std::array kSampleUV {
    std::array {0.7709488f, 0.2250512f}, std::array {0.9890512f, 0.2250512f},
    std::array {0.9890512f, 0.0069488f}, std::array {0.7709488f, 0.0069488f}
};

[[nodiscard]] auto MakeAtlas() -> std::vector<uint8_t> {
    // Four distinct colours in the checkmark region; the old rotation sign
    // instead lands in the magenta middle of the image (the diagonal stripe
    // region in the real model). The quadrant pattern also detects a mirrored
    // or transposed result rather than accepting any single solid colour.
    std::vector<uint8_t> rgba(128u * 128u * 4u);
    for (uint32_t y = 0; y < 128; ++y) {
        for (uint32_t x = 0; x < 128; ++x) {
            std::array<uint8_t, 3> color {200, 15, 180};
            if (x >= 96 && y < 32) {
                if (x < 112 && y < 16) {
                    color = {240, 20, 20};
                } else if (x < 112) {
                    color = {20, 240, 20};
                } else if (y < 16) {
                    color = {20, 30, 240};
                } else {
                    color = {240, 220, 30};
                }
            }
            const size_t pixel = (static_cast<size_t>(y) * 128u + x) * 4u;
            rgba[pixel + 0] = color[0];
            rgba[pixel + 1] = color[1];
            rgba[pixel + 2] = color[2];
            rgba[pixel + 3] = 255;
        }
    }
    return rgba;
}

[[nodiscard]] auto MakePanel(ZHLN::GraphicsDevice rc, uint32_t column) -> ZHLN::Mesh {
    std::array<ZHLN::VertexPosition, 4> positions {{
        {{-0.45f, 0.45f, 0.0f}}, {{-0.45f, -0.45f, 0.0f}},
        {{0.45f, -0.45f, 0.0f}}, {{0.45f, 0.45f, 0.0f}}
    }};
    std::array<ZHLN::VertexTangentFrame, 4> frames {};
    std::array<ZHLN::VertexSurface, 4>      surfaces {};
    for (size_t i = 0; i < positions.size(); ++i) {
        frames[i] = {.normal = ZHLN::Math::PackNormal(0.0f, 0.0f, 1.0f),
                     .tangent = ZHLN::Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f)};
        const std::array<float, 2> uv0 = column == 2 ? kSampleUV[i] : column == 0 ? kTestUV[i] : std::array {0.0f, 0.0f};
        const std::array<float, 2> uv1 = column == 1 ? kTestUV[i] : std::array {0.0f, 0.0f};
        surfaces[i] = {.uv = ZHLN::Math::PackUV(uv0[0], uv0[1]),
                       .color = ZHLN::Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f),
                       .uv1 = ZHLN::Math::PackUV(uv1[0], uv1[1])};
    }
    constexpr std::array<uint32_t, 6> indices {0, 1, 2, 0, 2, 3};
    return {
        .posBuffer          = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {positions}),
        .tangentFrameBuffer = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {frames}),
        .surfaceBuffer      = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {surfaces}),
        .indexBuffer        = rc.CreateBuffer<ZHLN::BufferUsage::Index>(std::span<const uint32_t> {indices}),
        .vertexCount        = 4,
        .indexCount         = 6,
    };
}

[[nodiscard]] auto PixelAt(const ZHLN::Test::Image::RgbImage& image, float x, float y) -> std::array<int, 3> {
    const auto ix = std::clamp(static_cast<int>(std::lround(x)), 0, image.width - 1);
    const auto iy = std::clamp(static_cast<int>(std::lround(y)), 0, image.height - 1);
    const size_t offset = (static_cast<size_t>(iy) * static_cast<size_t>(image.width) + static_cast<size_t>(ix)) * 3u;
    return {image.rgb[offset], image.rgb[offset + 1], image.rgb[offset + 2]};
}

[[nodiscard]] auto ChannelDifference(std::span<const int, 3> a, std::span<const int, 3> b) -> int {
    return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
}

} // namespace

namespace {

struct TextureTransformsTestSuite {
    TextureTransformsTestSuite() { ZHLN::Test::Headless::BeginSession(); }
    ~TextureTransformsTestSuite() { ZHLN::Test::Headless::EndSession(); }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> rotated_uv0_and_uv1_match_the_sample_atlas() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless Khronos UV transforms", 480, 320);
            if (!engine.Valid()) return std::unexpected(TextureTransformError::EngineInitFailed);
            auto rc = engine->Graphics();
            auto reg = engine->Entities();
            ZHLN::Test::Headless::DisableAA(*engine);

            engine->Graphics().PatchSettings([](auto& gfx) {
                gfx.environment.fullBright = 1;
                gfx.post.vignetteIntensity = 0.0f;
                gfx.post.glowIntensity     = 0.0f;
                gfx.post.bloomStrength     = 0.0f;
            });
            ZHLN::DegF cameraFov {};
            if (!ZHLN::Test::ExpectTrue(reg.PatchSingleton<ZHLN::Components::CameraComponent>([&](auto& c) {
                    c.camera.position = {0.0f, 0.0f, kCameraDistance};
                    c.camera.yaw      = ZHLN::DegF{-90.0f};
                    c.camera.pitch    = ZHLN::DegF{0.0f};
                    cameraFov         = c.camera.fov;
                }))) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            camera.fov      = ZHLN::DegF{45.0f};

            const auto atlas = MakeAtlas();
            const auto texture = rc.CreateTexture(std::span {atlas}, {128, 128});
            if (!texture) return std::unexpected(TextureTransformError::ResourceCreationFailed);

            constexpr std::array<std::string_view, 3> meshKeys {"uv_transform_0_mesh", "uv_transform_1_mesh", "uv_transform_sample_mesh"};
            constexpr std::array<std::string_view, 3> materialKeys {"uv_transform_0_mat", "uv_transform_1_mat", "uv_transform_sample_mat"};
            std::array<ZHLN::MaterialID, 3> registeredMaterials {};
            size_t registeredCount = 0;
            ZHLN::defer _([&] {
                engine->ClearScene(); // releases the three scene-owned mesh buffers
                for (size_t i = 0; i < registeredCount; ++i) {
                    rc.UnregisterMaterial(registeredMaterials[i]);
                }
                rc.UnloadTexture(*texture);
            });

            for (uint32_t column = 0; column < 3; ++column) {
                ZHLN::MaterialDesc desc {
                    .doubleSided = true, .metallic = 0.0f, .roughness = 1.0f, .albedoMap = *texture
                };
                if (column != 2) {
                    desc.textureTransforms[static_cast<size_t>(ZHLN::MaterialTextureSlot::Albedo)] = {
                        .offset = {0.705f, 0.285f}, .scale = {0.35f, 0.35f}, .rotation = 1.57079637f, .texCoord = column
                    };
                }
                const auto material = rc.CreateMaterial(desc);
                if (!material) return std::unexpected(TextureTransformError::ResourceCreationFailed);
                const auto materialID = ZHLN::HashAssetID(materialKeys[column]);
                rc.RegisterMaterial(materialID, *material);
                registeredMaterials[registeredCount++] = materialID;

                ZHLN::Mesh mesh = MakePanel(rc, column);
                if (mesh.posBuffer == ZHLN::BufferID::Invalid || mesh.tangentFrameBuffer == ZHLN::BufferID::Invalid ||
                    mesh.surfaceBuffer == ZHLN::BufferID::Invalid || mesh.indexBuffer == ZHLN::BufferID::Invalid) {
                    rc.DestroyMesh(mesh);
                    return std::unexpected(TextureTransformError::ResourceCreationFailed);
                }
                const auto meshID = ZHLN::HashAssetID(meshKeys[column]);
                rc.RegisterMesh(meshID, mesh);
                const ZHLN::FVec3 offset{kPanelX[column], 0.0f, 0.0f};
                const auto world = ZHLN::CreateTranslation(offset);
                const ZHLN::Entity e = reg.Spawn(
                    ZHLN::Components::TransformComponent {.position = offset},
                    ZHLN::Components::WorldTransformComponent {.world = world, .previous = world},
                    ZHLN::Components::MeshComponent {.meshAsset = meshID, .materialAsset = materialID, .cullRadius = 1.0f}
                );
                engine->Entities().Attach<ZHLN::Components::OwnedMeshComponent>(e, {.meshAsset = meshID, .mesh = mesh});
            }

            ZHLN::Test::Headless::TickFrames(*engine, 5);
            const auto image = ZHLN::Test::Headless::Capture(*engine, "texture_transform_columns.ppm");
            if (!image.Valid()) return std::unexpected(TextureTransformError::CaptureFailed);

            // Project the same four interior positions in each quad. No probe
            // crosses an atlas quadrant or a mesh edge, so bilinear filtering
            // and small camera/exposure differences cannot mask a reversed UV.
            const float pixelsPerUnit = static_cast<float>(image.height) /
                                        (2.0f * kCameraDistance * std::tan(cameraFov.ToRadians().value * 0.5f));
            std::array<std::array<std::array<int, 3>, 4>, 3> samples {};
            for (size_t column = 0; column < 3; ++column) {
                for (size_t probe = 0; probe < 4; ++probe) {
                    const float localX = probe & 1u ? 0.22f : -0.22f;
                    const float localY = probe & 2u ? -0.22f : 0.22f;
                    samples[column][probe] = PixelAt(image, 0.5f * image.width + (kPanelX[column] + localX) * pixelsPerUnit,
                                                     0.5f * image.height - localY * pixelsPerUnit);
                }
            }

            for (size_t a = 0; a < 4; ++a) {
                for (size_t b = a + 1; b < 4; ++b) {
                    if (ChannelDifference(samples[2][a], samples[2][b]) < 90) {
                        return std::unexpected(TextureTransformError::ControlMissing);
                    }
                }
            }
            for (size_t column = 0; column < 2; ++column) {
                for (size_t probe = 0; probe < 4; ++probe) {
                    if (ChannelDifference(samples[column][probe], samples[2][probe]) > 95) {
                        return std::unexpected(column == 0 ? TextureTransformError::TransformedUV0Mismatch :
                                                          TextureTransformError::TransformedUV1Mismatch);
                    }
                }
            }
            return {};
        }
    };
};

} // namespace
