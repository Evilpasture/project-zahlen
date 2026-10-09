// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Render the same indexed triangle mesh with both signs of its world-transform
// determinant. One-sided front/back panels test culling; two-sided lit panels
// test the SV_IsFrontFace normal orientation. The vertex/indirect and meshlet
// paths must agree without making mirrored materials double-sided.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SceneResources.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>


enum class NegativeScaleError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not create the headless negative-scale test engine.">{}) = 1,
    ResourceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not upload the indexed panel and its materials/meshlets.">{}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not capture the negative-scale test frame.">{}),
    MirroredFrontMissing ZHLN_ANNOTATION(ZHLN::Description<"A single-sided mirrored front face was culled or rendered incorrectly.">{}),
    MirroredBackVisible ZHLN_ANNOTATION(ZHLN::Description<"A single-sided mirrored back face was not culled.">{}),
    MirroredNormalReversed ZHLN_ANNOTATION(ZHLN::Description<"Two-sided panels changed illumination under negative scale.">{}),
    MeshPathInactive ZHLN_ANNOTATION(ZHLN::Description<"Mesh shading is supported but the mesh-shader test did not select it.">{}),
};

namespace {

using ZHLN::Test::Image::RgbImage;

struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct CaptureResult {
    std::array<Rgb, 5> samples {};
    bool meshShadingSupported = false;
};

// Indices and meshlets both use these CCW (+Z) faces. Each entity below uses
// this *same* mesh. No parity-dependent copies of its indices or pipelines.
[[nodiscard]] auto MakePanel(ZHLN::RenderContext& rc) -> ZHLN::Mesh {
    const std::array<ZHLN::VertexPosition, 4> positions {{
        {{-0.45f, 0.45f, 0.0f}}, {{-0.45f, -0.45f, 0.0f}},
        {{0.45f, -0.45f, 0.0f}}, {{0.45f, 0.45f, 0.0f}},
    }};
    std::array<ZHLN::VertexTangentFrame, 4> frames {};
    std::array<ZHLN::VertexSurface, 4> surfaces {};
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i] = {.normal = ZHLN::Math::PackNormal(0.0f, 0.0f, 1.0f),
                     .tangent = ZHLN::Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f)};
        surfaces[i] = {.uv = ZHLN::Math::PackUV(0.5f, 0.5f),
                       .color = ZHLN::Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f)};
    }
    constexpr std::array<uint32_t, 6> indices {0, 1, 2, 0, 2, 3};
    const auto meshlets = ZHLN::BuildMeshlets(std::span {indices}, std::span {positions});
    const auto packedMeshlets = ZHLN::PackMeshlets(meshlets.meshlets);

    return {
        .posBuffer           = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {positions}),
        .tangentFrameBuffer  = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {frames}),
        .surfaceBuffer       = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span {surfaces}),
        .indexBuffer         = rc.CreateBuffer<ZHLN::BufferUsage::Index>(std::span<const uint32_t> {indices}),
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = static_cast<uint32_t>(indices.size()),
        .meshletBuffer       = rc.CreateBuffer(ZHLN::BufferDesc {.usage = ZHLN::BufferUsage::Storage, .data = std::as_bytes(std::span {packedMeshlets}), .stride = ZHLN::kMeshletPackedBytes}),
        .meshletVertexBuffer = rc.CreateBuffer<ZHLN::BufferUsage::Storage>(std::span {meshlets.vertices}),
        .meshletTriBuffer    = rc.CreateBuffer<ZHLN::BufferUsage::Storage>(std::span {meshlets.triangles}),
        .meshletCount        = static_cast<uint32_t>(meshlets.meshlets.size()),
    };
}

[[nodiscard]] auto Complete(const ZHLN::Mesh& mesh) noexcept -> bool {
    using enum ZHLN::BufferHandle;
    return mesh.posBuffer != Invalid && mesh.tangentFrameBuffer != Invalid && mesh.surfaceBuffer != Invalid && mesh.indexBuffer != Invalid &&
           mesh.meshletBuffer != Invalid && mesh.meshletVertexBuffer != Invalid && mesh.meshletTriBuffer != Invalid && mesh.meshletCount != 0;
}

constexpr float kCameraDistance = 5.0f;
const std::array<JPH::Vec3, 5> kCenters {
    JPH::Vec3(-1.45f, 0.85f, 0.0f), JPH::Vec3(0.0f, 0.85f, 0.0f), JPH::Vec3(1.45f, 0.85f, 0.0f),
    JPH::Vec3(-0.75f, -0.85f, 0.0f), JPH::Vec3(0.75f, -0.85f, 0.0f),
};

[[nodiscard]] auto CaptureParity(bool meshShading) -> std::expected<CaptureResult, ZHLN::ErrorCode> {
    auto engine = ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
        .appName = "Headless NegativeScale Winding",
        .width = 640,
        .height = 480,
        .enableMeshShading = meshShading,
    });
    if (engine == nullptr) return std::unexpected(NegativeScaleError::EngineInitFailed);

    auto& rc  = engine->GetRenderContext();
    auto& reg = engine->GetRegistry();
    const auto info = rc.GetInfo();
    if (meshShading && !info.meshShadingActive) return std::unexpected(NegativeScaleError::MeshPathInactive);

    ZHLN::Test::Headless::DisableTAA(*engine);
    for (const auto e: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(e, [](auto& pp) {
            pp.fullBright = 0; // Must exercise front/back normal orientation.
            pp.giMode = 0;
            pp.enableSSR = 0;
            pp.enableRTR = 0;
            pp.ambientExposure = 0.0f;
            pp.exposure = 1.0f;
            pp.tonemapper = 3;
            pp.vignetteIntensity = 0.0f;
            pp.glowIntensity = 0.0f;
            pp.bloomStrength = 0.0f;
            pp.skyZenith = JPH::Vec4(0, 0, 0, 1);
            pp.skyHorizon = JPH::Vec4(0, 0, 0, 1);
            pp.skyGround = JPH::Vec4(0, 0, 0, 1);
        });
    }
    const auto cameraComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
    if (!ZHLN::Test::ExpectTrue(cameraComp.has_value())) {
        return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
    }
    ZHLN::Camera& camera = cameraComp->camera;
    camera.position = JPH::Vec3(0.0f, 0.0f, kCameraDistance);
    camera.yaw      = -90.0f;
    camera.pitch    = 0.0f;
    camera.fov      = 45.0f;

    const ZHLN::Entity sun = reg.Create();
    reg.Add(sun, ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 0.0f, 8.0f)},
            ZHLN::Components::LightComponent {.type = ZHLN::LightType::Sun, .color = JPH::Vec3(1, 1, 1),
                                               .intensity = 220.0f, .direction = JPH::Vec3(0, 0, 1)});

    constexpr auto meshID = ZHLN::HashAssetID("negative_scale_shared_panel");
    constexpr auto oneSidedID = ZHLN::HashAssetID("negative_scale_one_sided");
    constexpr auto doubleSidedID = ZHLN::HashAssetID("negative_scale_double_sided");
    const auto oneSided = rc.CreateMaterial(ZHLN::MaterialDesc {
        .unlit = true, .metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.05f, 0.9f, 0.05f, 1.0f}
    });
    if (!oneSided) return std::unexpected(NegativeScaleError::ResourceCreationFailed);
    rc.RegisterGPUMaterial(oneSidedID, *oneSided);
    bool hasDoubleSided = false;
    ZHLN::defer _([&] {
        engine->ClearScene(); // Releases the sole owner of the mesh before unregistering the materials.
        rc.UnregisterGPUMaterial(oneSidedID);
        if (hasDoubleSided) rc.UnregisterGPUMaterial(doubleSidedID);
    });
    const auto doubleSided = rc.CreateMaterial(ZHLN::MaterialDesc {
        .doubleSided = true, .metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}
    });
    if (!doubleSided) return std::unexpected(NegativeScaleError::ResourceCreationFailed);
    rc.RegisterGPUMaterial(doubleSidedID, *doubleSided);
    hasDoubleSided = true;

    ZHLN::Mesh mesh = MakePanel(rc);
    if (!Complete(mesh)) {
        rc.DestroyMesh(mesh);
        return std::unexpected(NegativeScaleError::ResourceCreationFailed);
    }
    rc.RegisterGPUMesh(meshID, mesh);

    for (size_t i = 0; i < kCenters.size(); ++i) {
        // Panel 2 reverses its surface direction with a *proper* rotation;
        // adding the reflection makes its world winding CCW before the fix,
        // exactly the red-X back-face failure in the Khronos model.
        const bool mirrored = i == 1 || i == 2 || i == 4;
        const auto rotation = i == 2 ? JPH::Quat::sRotation(JPH::Vec3::sAxisY(), JPH::JPH_PI) : JPH::Quat::sIdentity();
        const JPH::Vec3 scale(mirrored ? -1.0f : 1.0f, 1.0f, 1.0f);
        const JPH::Mat44 world = ZHLN::Math::CreateTransform(kCenters[i], rotation, scale);
        const ZHLN::Entity e = reg.Create();
        reg.Add(e, ZHLN::Components::TransformComponent {.position = kCenters[i], .rotation = rotation, .scale = scale});
        reg.Add(e, ZHLN::Components::WorldTransformComponent {.world = world, .previous = world});
        reg.Add(e, ZHLN::Components::MeshComponent {
            .meshAsset = meshID,
            .materialAsset = i < 3 ? oneSidedID : doubleSidedID,
            .cullRadius = 0.65f,
        });
        if (i == 0) { // Other instances only borrow the registered mesh.
            ZHLN::SceneResources::Attach<ZHLN::Components::OwnedMeshComponent>(*engine, e, {.meshAsset = meshID, .mesh = mesh});
        }
    }

    ZHLN::Test::Headless::TickFrames(*engine, 6);
    const RgbImage image = ZHLN::Test::Headless::Capture(*engine, meshShading ? "negative_scale_mesh.ppm" : "negative_scale_vertex.ppm");
    if (!image.Valid()) return std::unexpected(NegativeScaleError::CaptureFailed);

    CaptureResult result {.meshShadingSupported = info.meshShadingSupported};
    const double halfHeight = kCameraDistance * std::tan(JPH::DegreesToRadians(camera.fov) * 0.5);
    const double halfWidth = halfHeight * image.width / image.height;
    for (size_t i = 0; i < kCenters.size(); ++i) {
        const double u = 0.5 + kCenters[i].GetX() / (2.0 * halfWidth);
        const double v = 0.5 - kCenters[i].GetY() / (2.0 * halfHeight);
        const auto region = ZHLN::Test::Image::MeasureSubRegion(image, {u - 0.015, v - 0.015, u + 0.015, v + 0.015});
        result.samples[i] = {region.meanR, region.meanG, region.meanB};
    }
    return result;
}

[[nodiscard]] auto CheckResult(const CaptureResult& result) -> std::expected<void, ZHLN::ErrorCode> {
    const auto& [front, mirroredFront, mirroredBack, lit, mirroredLit] = result.samples;
    ZHLN::Println("    [INFO] negative-scale front G={:.1f}/{:.1f}, back G={:.1f}, lit R={:.1f}/{:.1f}",
                  front.g, mirroredFront.g, mirroredBack.g, lit.r, mirroredLit.r);
    if (front.g < 60.0 || front.g < front.r + 30.0 || mirroredFront.g < 60.0 ||
        std::abs(front.g - mirroredFront.g) > 30.0) {
        return std::unexpected(NegativeScaleError::MirroredFrontMissing);
    }
    if (mirroredBack.g > front.g * 0.3) return std::unexpected(NegativeScaleError::MirroredBackVisible);
    if (lit.r < 35.0 || mirroredLit.r < 35.0 ||
        std::max({std::abs(lit.r - mirroredLit.r), std::abs(lit.g - mirroredLit.g), std::abs(lit.b - mirroredLit.b)}) > 30.0) {
        return std::unexpected(NegativeScaleError::MirroredNormalReversed);
    }
    return {};
}

} // namespace

struct NegativeScaleTestSuite {
    NegativeScaleTestSuite() { ZHLN::Test::Headless::BeginSession(); }
    ~NegativeScaleTestSuite() { ZHLN::Test::Headless::EndSession(); }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> mirrored_culling_and_two_sided_normals_match_on_both_paths() {
            const auto vertex = CaptureParity(false);
            if (!vertex) return std::unexpected(vertex.error());
            if (auto checked = CheckResult(*vertex); !checked) return checked;
            if (!vertex->meshShadingSupported) {
                ZHLN::Println("    [SKIP] VK_EXT_mesh_shader unavailable; vertex/indirect path verified.");
                return {};
            }
            const auto mesh = CaptureParity(true);
            if (!mesh) return std::unexpected(mesh.error());
            return CheckResult(*mesh);
        }
    };
};

auto RunNegativeScaleSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<NegativeScaleTestSuite>();
}
