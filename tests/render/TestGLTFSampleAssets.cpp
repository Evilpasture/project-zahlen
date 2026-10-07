// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestGLTFSampleAssets.cpp
//
// Renders real Khronos glTF-Sample-Assets models without committing any GLB:
// the checkout lives outside the repo (build/sample-assets/ via
// -DZHLN_FETCH_SAMPLE_ASSETS=ON, or any path in $ZHLN_SAMPLE_ASSETS) and the
// tests SKIP when it is absent -- the same skip pattern TestGLTFImport uses
// for an unresolved LFS pointer.
//
// Only invariants are asserted: the import succeeds under the fidelity
// options (emissiveFactorScale = 1, 2048px textures), the spawn produces
// entities, the frame is non-degenerate, and the model is actually visible
// (an A/B capture against the empty scene differs). No pixel goldens, no
// absolute counts: a fidelity improvement must not fail this suite.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <glTF/GLTFImporter.hpp>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class SampleAssetsError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize the headless Engine the sample-asset test imports through."> {}) = 1,
    ImportFailed ZHLN_ANNOTATION(ZHLN::Description<"The Khronos sample GLB did not import to a non-empty prefab."> {}),
    SpawnFailed ZHLN_ANNOTATION(ZHLN::Description<"InstantiatePrefab produced no entities for the imported sample model."> {}),
    ModelNotVisible ZHLN_ANNOTATION(ZHLN::Description<"The spawned sample model did not change the frame versus the empty scene."> {}),
    RenderDegenerate ZHLN_ANNOTATION(ZHLN::Description<"The sample-asset frame was invalid or had no lit pixels."> {}),
};

namespace {

// Candidates in preference order, relative to the sample-assets root. The
// first file that exists and parses as a GLB wins; when none does the test
// skips. Keep this list to origin-centered static models: the camera fit
// below assumes the model sits at the origin.
constexpr std::array kModelCandidates {
    "Models/MetalRoughSpheres/glTF-Binary/MetalRoughSpheres.glb",
    "Models/MetalRoughSpheresNoTextures/glTF-Binary/MetalRoughSpheresNoTextures.glb",
};

[[nodiscard]] std::string SampleAssetsDir() {
    if (const char* env = std::getenv("ZHLN_SAMPLE_ASSETS"); env != nullptr && *env != '\0') {
        return std::string {env};
    }
#ifdef ZHLN_SAMPLE_ASSETS_DIR
    return std::string {ZHLN_SAMPLE_ASSETS_DIR};
#else
    return {};
#endif
}

[[nodiscard]] std::vector<uint8_t> ReadModelBytes(std::string& outPath) {
    const std::string dir = SampleAssetsDir();
    if (dir.empty()) {
        return {};
    }
    for (const char* candidate: kModelCandidates) {
        const std::string path = dir + "/" + candidate;
        std::ifstream     stream(path, std::ios::binary);
        if (!stream.is_open()) {
            continue;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (bytes.size() < 4 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "glTF") {
            continue;
        }
        outPath = path;
        return bytes;
    }
    return {};
}

// Frames whatever the prefab contains: scene radius from part offsets plus
// their bounding spheres, camera on +Z at the fov fit distance. Khronos
// models are origin-centered, so the target is the origin.
void FitCameraToPrefab(ZHLN::Camera& camera, const ZHLN::ModelPrefab& prefab, float fovDegrees) {
    double radius = 0.0;
    for (const ZHLN::ModelPart& part: prefab.parts) {
        const float offset = part.localTransform.GetTranslation().Length();
        radius             = std::max(radius, static_cast<double>(offset + part.boundingRadius));
    }
    if (!(radius > 0.5)) {
        radius = 2.0; // degenerate bounds: fall back to a room-sized frame
    }
    const double halfFov  = std::max(0.1, static_cast<double>(JPH::DegreesToRadians(fovDegrees)) * 0.5);
    double       distance = radius / std::tan(halfFov) * 1.25;
    distance              = std::clamp(distance, radius * 1.2 + 1.0, 80.0);
    camera.position       = JPH::Vec3(0.0f, static_cast<float>(radius * 0.35), static_cast<float>(distance));
    camera.yaw            = -90.0f;
    camera.pitch          = static_cast<float>(-JPH::RadiansToDegrees(std::atan(radius * 0.35 / distance)));
    camera.fov            = fovDegrees;
    camera.nearZ          = 0.05f;
    camera.farZ           = std::max(100.0f, static_cast<float>(distance * 4.0));
}

} // namespace

struct GLTFSampleAssetsTestSuite {
    GLTFSampleAssetsTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~GLTFSampleAssetsTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> khronos_sample_model_imports_spawns_and_renders() {
            std::string              modelPath;
            const std::vector<uint8_t> bytes = ReadModelBytes(modelPath);
            if (bytes.empty()) {
                ZHLN::Println("    [SKIP] No glTF-Sample-Assets checkout (set $ZHLN_SAMPLE_ASSETS or configure with -DZHLN_FETCH_SAMPLE_ASSETS=ON).");
                return {};
            }

            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Sample Assets", 640, 480);
            if (engine == nullptr) {
                return std::unexpected(SampleAssetsError::EngineInitFailed);
            }
            auto& rc     = engine->GetRenderContext();
            auto& assets = engine->GetAssetManager();
            ZHLN::Test::Headless::DisableTAA(*engine);

            // Fidelity import units: authored linear emission, full texture
            // detail -- the same options the FidelityHarness renders with.
            constexpr ZHLN::GLTF::ImportOptions fidelity {.emissiveFactorScale = 1.0f, .maxTextureDimension = 2048};
            const auto prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, bytes, modelPath, modelPath, fidelity);
            if (!prefab || !ZHLN::Test::ExpectGt(prefab->parts.size(), size_t {0})) {
                return std::unexpected(SampleAssetsError::ImportFailed);
            }

            // Empty-scene baseline first: the model-visibility check below is
            // a differential A/B, so exposure and sky changes cancel out.
            ZHLN::Test::Headless::TickFrames(*engine, 5);
            const auto empty = ZHLN::Test::Headless::Capture(*engine, "sample_assets_empty.ppm");
            if (!empty.Valid()) {
                return std::unexpected(SampleAssetsError::RenderDegenerate);
            }

            const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            FitCameraToPrefab(camComp->camera, *prefab, 50.0f);

            std::vector<ZHLN::Entity> instances(1u + prefab->parts.size());
            const uint32_t            spawned = ZHLN::PrefabFactory::InstantiatePrefab(
                *engine, *prefab, ZHLN::PrefabFactory::SpawnParams {}, instances.data(), static_cast<uint32_t>(instances.size())
            );
            if (!ZHLN::Test::ExpectGt(spawned, 0u)) {
                return std::unexpected(SampleAssetsError::SpawnFailed);
            }

            ZHLN::Test::Headless::TickFrames(*engine, 8);
            const auto frame = ZHLN::Test::Headless::Capture(*engine, "sample_assets_model.ppm");
            if (!frame.Valid()) {
                return std::unexpected(SampleAssetsError::RenderDegenerate);
            }

            const auto   metrics  = ZHLN::Test::Image::MeasureImage(frame);
            const double litShare = metrics.total > 0 ? static_cast<double>(metrics.lit) / static_cast<double>(metrics.total) : 0.0;
            if (!ZHLN::Test::ExpectGt(litShare, 0.005)) {
                return std::unexpected(SampleAssetsError::RenderDegenerate);
            }

            const auto visibility = ZHLN::Test::Image::CompareFrames(empty, frame);
            ZHLN::Println(
                "    [INFO] {}: {} parts, {} spawned, lit share {:.3f}, empty-vs-model frac32 {:.4f}.",
                modelPath, prefab->parts.size(), spawned, litShare, visibility.frac32
            );
            if (!ZHLN::Test::ExpectGt(visibility.frac32, 0.001)) {
                return std::unexpected(SampleAssetsError::ModelNotVisible);
            }

            ZHLN::Println("    [PASS] {}: {} parts spawned and visible (frac32 {:.4f} vs empty scene).", modelPath, spawned, visibility.frac32);
            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunGLTFSampleAssetsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GLTFSampleAssetsTestSuite>();
}
