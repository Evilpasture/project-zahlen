// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

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

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <glTF/GLTFAssets.hpp>
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
using ZHLN::FVec3;

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

[[nodiscard]] std::pair<std::string, std::vector<uint8_t>> ReadModelBytes() {
    const std::string dir = SampleAssetsDir();
    if (dir.empty()) {
        return {};
    }
    for (const char* candidate: kModelCandidates) {
        std::string   path = dir + "/" + candidate;
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            continue;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (bytes.size() < 4 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "glTF") {
            continue;
        }
        return {std::move(path), std::move(bytes)};
    }
    return {};
}

// Frames whatever the prefab contains: scene radius from part offsets plus
// their bounding spheres, camera on +Z at the fov fit distance. Khronos
// models are origin-centered, so the target is the origin.
void FitCameraToPrefab(ZHLN::Camera& camera, const ZHLN::COM::PrefabBoundsInfo& bounds, float fovDegrees) {
    const ZHLN::FVec3 ext = (bounds.max - bounds.min) * 0.5f;
    double radius = bounds.hasParts ? static_cast<double>(ext.Length() + ((bounds.min + bounds.max) * 0.5f).Length()) : 0.0;
    if (!(radius > 0.5)) {
        radius = 2.0; // degenerate bounds: fall back to a room-sized frame
    }
    const double halfFov  = std::max(0.1, static_cast<double>(((3.14159265358979323846f / 180.0f) * fovDegrees)) * 0.5);
    double       distance = radius / std::tan(halfFov) * 1.25;
    distance              = std::clamp(distance, radius * 1.2 + 1.0, 80.0);
    camera.position = {0.0f, static_cast<float>(radius * 0.35), static_cast<float>(distance)};
    camera.yaw            = ZHLN::DegF{-90.0f};
    camera.pitch          = ZHLN::DegF{static_cast<float>(-((180.0f / 3.14159265358979323846f) * std::atan(radius * 0.35 / distance)))};
    camera.fov            = ZHLN::DegF{fovDegrees};
    camera.nearZ          = 0.05f;
    camera.farZ           = std::max(100.0f, static_cast<float>(distance * 4.0));
}

} // namespace

namespace {

struct GLTFSampleAssetsTestSuite {
    GLTFSampleAssetsTestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~GLTFSampleAssetsTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> khronos_sample_model_imports_spawns_and_renders() {
            const auto [modelPath, bytes] = ReadModelBytes();
            if (bytes.empty()) {
                ZHLN::Println("    [SKIP] No glTF-Sample-Assets checkout (set $ZHLN_SAMPLE_ASSETS or configure with -DZHLN_FETCH_SAMPLE_ASSETS=ON).");
                return {};
            }

            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Sample Assets", 640, 480);
            if (!engine.Valid()) {
                return std::unexpected(SampleAssetsError::EngineInitFailed);
            }
            auto& rc = *engine->Graphics();
            auto& assets = *engine->Assets();
            ZHLN::Test::Headless::DisableAA(*engine);

            // Fidelity import units: authored linear emission, full texture
            // detail -- the same options the FidelityHarness renders with.
            constexpr ZHLN::GLTF::ImportOptions fidelity {.emissiveFactorScale = 1.0f, .maxTextureDimension = 2048};
            const auto doc = ZHLN::GLTF::Decode(bytes, modelPath, modelPath, fidelity, &assets);
            if (!doc || !ZHLN::Test::ExpectGt(doc->parts.size(), size_t {0})) {
                return std::unexpected(SampleAssetsError::ImportFailed);
            }
            const auto uploaded = ZHLN::GLTF::Upload(assets, rc, *doc);
            if (!uploaded) {
                return std::unexpected(SampleAssetsError::ImportFailed);
            }
            ZHLN::COM::PrefabBoundsInfo bounds {};
            (void) assets.GetPrefabBounds(uploaded->hash, ZHLN::COM::OutChain(bounds));

            // Empty-scene baseline first: the model-visibility check below is
            // a differential A/B, so exposure and sky changes cancel out.
            ZHLN::Test::Headless::TickFrames(*engine, 5);
            const auto empty = ZHLN::Test::Headless::Capture(*engine, "sample_assets_empty.ppm");
            if (!empty.Valid()) {
                return std::unexpected(SampleAssetsError::RenderDegenerate);
            }

            if (!ZHLN::Test::ExpectTrue(engine->Entities()->PatchSingleton<ZHLN::Components::CameraComponent>(
                    [&](auto& c) { FitCameraToPrefab(c.camera, bounds, 50.0f); }))) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }

            const std::vector<ZHLN::Entity> instances = assets.Instantiate(uploaded->hash, ZHLN::PrefabFactory::SpawnParams {});
            const uint32_t                  spawned   = static_cast<uint32_t>(instances.size());
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
                modelPath, doc->parts.size(), spawned, litShare, visibility.frac32
            );
            if (!ZHLN::Test::ExpectGt(visibility.frac32, 0.001)) {
                return std::unexpected(SampleAssetsError::ModelNotVisible);
            }

            ZHLN::Println("    [PASS] {}: {} parts spawned and visible (frac32 {:.4f} vs empty scene).", modelPath, spawned, visibility.frac32);
            return {};
        }
    };
};

} // namespace
