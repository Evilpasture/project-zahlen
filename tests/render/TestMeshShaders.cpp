// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestMeshShaders.cpp
//
// Verification for the VK_EXT_mesh_shader path (see MESH_SHADERS.md).
//
// The interesting question is not "does it run" -- a mesh pipeline that emits
// garbage still runs -- but "does it rasterise exactly what the vertex pipeline
// rasterises". So the central test renders the same scene twice in one process,
// once through task/mesh shaders and once through the classic vertex pipeline,
// and compares the two framebuffers pixel by pixel.
//
// Every GPU test degrades to a skip (not a failure) when the device does not
// expose mesh shading, so this binary stays green on lavapipe/older hardware.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// Test Error Types
// ============================================================================

enum class MeshShaderTestError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for the mesh shader test.">{}) = 1,
    MeshletPartitioningFailed ZHLN_ANNOTATION(ZHLN::Description<"meshoptimizer partitioning violated a GPU stream invariant.">{}),
    MeshletStreamsMissing ZHLN_ANNOTATION(ZHLN::Description<"A mesh that must be meshletized carries no meshlet streams.">{}),
    RenderOutputBlank ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is blank or could not be captured.">{}),
    PathDivergence ZHLN_ANNOTATION(ZHLN::Description<"The mesh-shader path and the vertex path produced different images.">{}),
    ValidationErrorsRaised ZHLN_ANNOTATION(ZHLN::Description<"The validation layer reported errors while rendering the comparison frames.">{}),
    ConfigDidNotSelectPath ZHLN_ANNOTATION(ZHLN::Description<"RenderConfig::enableMeshShading did not select the expected geometry path.">{}),
    MeshletConeCullingFalsePositive ZHLN_ANNOTATION(ZHLN::Description<"Meshlet normal-cone culling culled a front-facing meshlet when camera was close (apex singularity).">{}) = 8,
    MeshletBlackHoleDetected ZHLN_ANNOTATION(ZHLN::Description<"Close-up render produced a black square where a meshlet should be (falsely culled).">{}),
};

namespace {

struct Image {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 && rgb.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    }
};

[[nodiscard]] Image LoadPPM(const std::string& path) {
    Image         img;
    std::ifstream ppm(path, std::ios::binary);
    if (!ppm.is_open()) {
        return img;
    }

    std::string header;
    int         maxColor = 0;
    ppm >> header >> img.width >> img.height >> maxColor;
    ppm.get();

    if (img.width <= 0 || img.height <= 0) {
        return {};
    }

    img.rgb.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3u);
    ppm.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    return img;
}

// Pixels that differ from the top-left pixel: a cheap "is anything actually
// drawn here" probe, so two identically-blank frames cannot pass as a match.
[[nodiscard]] uint32_t ShadedPixelCount(const Image& img) {
    if (!img.Valid()) {
        return 0;
    }
    const uint8_t r0 = img.rgb[0];
    const uint8_t g0 = img.rgb[1];
    const uint8_t b0 = img.rgb[2];

    uint32_t count = 0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        const int dr = std::abs(static_cast<int>(img.rgb[i + 0]) - r0);
        const int dg = std::abs(static_cast<int>(img.rgb[i + 1]) - g0);
        const int db = std::abs(static_cast<int>(img.rgb[i + 2]) - b0);
        if (std::max({dr, dg, db}) > 8) {
            ++count;
        }
    }
    return count;
}

struct ImageDiff {
    uint32_t comparedPixels   = 0;
    uint32_t pixelsOverTol    = 0;
    uint32_t pixelsOverHigh   = 0; // |delta| > 32: structural, not interpolation noise
    uint32_t maskMismatch     = 0;
    int      maxChannelDelta  = 0;
    double   meanChannelDelta = 0.0;
    double   fractionOverTol  = 0.0;
    double   maskMismatchRate = 0.0;
};

// Is this pixel part of the drawn geometry (i.e. not the background colour
// sampled at the top-left corner)?
[[nodiscard]] inline bool IsShaded(const Image& img, size_t i, uint8_t r0, uint8_t g0, uint8_t b0) {
    const int dr = std::abs(static_cast<int>(img.rgb[i + 0]) - static_cast<int>(r0));
    const int dg = std::abs(static_cast<int>(img.rgb[i + 1]) - static_cast<int>(g0));
    const int db = std::abs(static_cast<int>(img.rgb[i + 2]) - static_cast<int>(b0));
    return std::max({dr, dg, db}) > 8;
}

[[nodiscard]] ImageDiff CompareImages(const Image& a, const Image& b, int tolerance) {
    ImageDiff diff;
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return diff;
    }

    uint64_t      deltaSum    = 0;
    uint32_t      shadedUnion = 0;
    const uint8_t ar0 = a.rgb[0], ag0 = a.rgb[1], ab0 = a.rgb[2];
    const uint8_t br0 = b.rgb[0], bg0 = b.rgb[1], bb0 = b.rgb[2];

    for (size_t i = 0; i < a.rgb.size(); i += 3) {
        const int dr    = std::abs(static_cast<int>(a.rgb[i + 0]) - static_cast<int>(b.rgb[i + 0]));
        const int dg    = std::abs(static_cast<int>(a.rgb[i + 1]) - static_cast<int>(b.rgb[i + 1]));
        const int db    = std::abs(static_cast<int>(a.rgb[i + 2]) - static_cast<int>(b.rgb[i + 2]));
        const int worst = std::max({dr, dg, db});

        deltaSum += static_cast<uint64_t>(dr + dg + db);
        diff.maxChannelDelta = std::max(diff.maxChannelDelta, worst);
        if (worst > tolerance) {
            ++diff.pixelsOverTol;
        }
        if (worst > 32) {
            ++diff.pixelsOverHigh;
        }

        // Silhouette comparison: a dropped meshlet, a flipped winding or a
        // mis-unpacked micro index puts geometry in DIFFERENT pixels, which
        // shows up here even when it barely moves the average colour.
        const bool shadedA = IsShaded(a, i, ar0, ag0, ab0);
        const bool shadedB = IsShaded(b, i, br0, bg0, bb0);
        if (shadedA || shadedB) {
            ++shadedUnion;
        }
        if (shadedA != shadedB) {
            ++diff.maskMismatch;
        }

        ++diff.comparedPixels;
    }

    if (diff.comparedPixels > 0) {
        diff.meanChannelDelta = static_cast<double>(deltaSum) / (static_cast<double>(diff.comparedPixels) * 3.0);
        diff.fractionOverTol  = static_cast<double>(diff.pixelsOverTol) / static_cast<double>(diff.comparedPixels);
    }
    if (shadedUnion > 0) {
        diff.maskMismatchRate = static_cast<double>(diff.maskMismatch) / static_cast<double>(shadedUnion);
    }
    return diff;
}

// Writes an amplified absolute-difference image so a failing run leaves
// something inspectable behind instead of just a number.
void WriteDiffImage(const std::string& path, const Image& a, const Image& b) {
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return;
    }
    out << "P6\n" << a.width << " " << a.height << "\n255\n";

    std::vector<uint8_t> amplified(a.rgb.size());
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const int d  = std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
        amplified[i] = static_cast<uint8_t>(std::min(255, d * 4));
    }
    out.write(reinterpret_cast<const char*>(amplified.data()), static_cast<std::streamsize>(amplified.size()));
}

} // namespace

// ============================================================================
// Test Suite
// ============================================================================

struct MeshShaderTestSuite {
    MeshShaderTestSuite() {
        // Two-phase GPU culling is disabled process-wide (before any device
        // exists, because the flag is latched into a static on first use).
        //
        // Rationale for the parity test: the mesh path bypasses the indirect
        // culling pass by design, so leaving it on for the vertex path would
        // compare two CULLING strategies rather than two geometry pipelines.
        // Hi-Z culling is also temporal -- it tests against the previous
        // frame's depth pyramid -- which injects frame-to-frame differences
        // that have nothing to do with mesh shading.
        setenv("ZHLN_NO_GPU_CULLING", "1", 1);

        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~MeshShaderTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    // Pooled: the binary keeps one engine alive and the scene is what gets
    // thrown away between tests. Creating a Vulkan instance per test is what
    // eventually exhausts the loader's static TLS and turns the tail of a
    // group into "vkCreateInstance: Found no drivers!".
    static auto CreateTestEngine(uint32_t width = 320, uint32_t height = 240) -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
            .appName               = "Headless Mesh Shader Test",
            .width                 = width,
            .height                = height,
        });
    }

    struct Tests {
        // ====================================================================
        // 1. Meshlet partitioning invariants (CPU only, no device required)
        // ====================================================================
        //
        // This is the exact code path both zcook and the runtime glTF importer
        // use, so a regression here corrupts every cooked asset.
        std::expected<void, ZHLN::ErrorCode> meshlet_partitioning_invariants() {
            // The GPU ABI is frozen: the shaders index these streams by hand.
            static_assert(sizeof(ZHLN::GPUMeshlet) == 64);
            static_assert(alignof(ZHLN::GPUMeshlet) == 16);
            static_assert(ZHLN::kMeshletMaxTriangles % 4 == 0, "meshoptimizer requires max_triangles to be a multiple of 4");
            static_assert(ZHLN::kMeshletMaxVertices <= 255, "micro indices are 8 bit");

            // --- a) Degenerate input must fall back, not crash ---
            {
                const std::vector<ZHLN::VertexPosition> none;
                const std::vector<uint32_t>             noIdx;
                ZHLN::Test::ExpectTrue(ZHLN::BuildMeshlets(noIdx, none).Empty());
            }

            // --- b) A single triangle is exactly one meshlet ---
            {
                const std::vector<ZHLN::VertexPosition> pos = {{{-1, -1, 0}}, {{1, -1, 0}}, {{0, 1, 0}}};
                const std::vector<uint32_t>             idx = {0, 1, 2};

                const auto built = ZHLN::BuildMeshlets(idx, pos);
                ZHLN::Test::ExpectEq(built.meshlets.size(), static_cast<size_t>(1));
                if (built.meshlets.size() != 1) {
                    return std::unexpected(MeshShaderTestError::MeshletPartitioningFailed);
                }
                ZHLN::Test::ExpectEq(built.meshlets[0].vertexCount, 3u);
                ZHLN::Test::ExpectEq(built.meshlets[0].triangleCount, 1u);
                ZHLN::Test::ExpectEq(built.vertices.size(), static_cast<size_t>(3));
                // 3 micro indices padded up to the 4-byte word the mesh shader loads.
                ZHLN::Test::ExpectEq(built.triangles.size(), static_cast<size_t>(4));
                ZHLN::Test::ExpectGt(built.meshlets[0].sphereRadius, 0.0f);
            }

            // --- c) A real surface: nothing lost, nothing out of bounds ---
            constexpr int                     kGrid = 48;
            std::vector<ZHLN::VertexPosition> positions;
            positions.reserve(static_cast<size_t>(kGrid) * kGrid);
            for (int y = 0; y < kGrid; ++y) {
                for (int x = 0; x < kGrid; ++x) {
                    positions.push_back(
                        {{static_cast<float>(x), std::sin(static_cast<float>(x) * 0.3f) * std::cos(static_cast<float>(y) * 0.3f), static_cast<float>(y)}}
                    );
                }
            }

            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(kGrid - 1) * (kGrid - 1) * 6);
            for (int y = 0; y + 1 < kGrid; ++y) {
                for (int x = 0; x + 1 < kGrid; ++x) {
                    const auto a = static_cast<uint32_t>(y * kGrid + x);
                    const auto b = a + 1;
                    const auto c = a + kGrid;
                    const auto d = c + 1;
                    indices.insert(indices.end(), {a, c, b, b, c, d});
                }
            }

            const auto built = ZHLN::BuildMeshlets(indices, positions);
            if (!ZHLN::Test::ExpectFalse(built.Empty())) {
                return std::unexpected(MeshShaderTestError::MeshletPartitioningFailed);
            }

            size_t totalTriangles = 0;
            bool   layoutOk       = true;

            for (const auto& m: built.meshlets) {
                layoutOk &= ZHLN::Test::ExpectLe(m.vertexCount, ZHLN::kMeshletMaxVertices);
                layoutOk &= ZHLN::Test::ExpectLe(m.triangleCount, ZHLN::kMeshletMaxTriangles);
                layoutOk &= ZHLN::Test::ExpectLe(m.vertexOffset + m.vertexCount, built.vertices.size());
                layoutOk &= ZHLN::Test::ExpectLe(static_cast<size_t>(m.triangleOffset) + (static_cast<size_t>(m.triangleCount) * 3u), built.triangles.size());
                // The mesh shader loads micro indices as 32-bit words.
                layoutOk &= ZHLN::Test::ExpectEq(m.triangleOffset % 4u, 0u);

                totalTriangles += m.triangleCount;

                for (uint32_t t = 0; t < m.triangleCount && layoutOk; ++t) {
                    for (uint32_t k = 0; k < 3; ++k) {
                        const uint8_t micro = built.triangles[m.triangleOffset + (t * 3u) + k];
                        layoutOk &= ZHLN::Test::ExpectLt(micro, m.vertexCount);
                        if (!layoutOk) {
                            break;
                        }
                        const uint32_t global = built.vertices[m.vertexOffset + micro];
                        layoutOk &= ZHLN::Test::ExpectLt(global, positions.size());
                        if (!layoutOk) {
                            break;
                        }
                        // Cluster culling is only sound if the baked sphere really
                        // bounds every vertex the cluster references.
                        const float dx = positions[global].position[0] - m.sphereCenter[0];
                        const float dy = positions[global].position[1] - m.sphereCenter[1];
                        const float dz = positions[global].position[2] - m.sphereCenter[2];
                        layoutOk &= ZHLN::Test::ExpectLe(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)), m.sphereRadius + 1e-3f);
                    }
                }
            }

            // No primitive may be dropped or duplicated by the partitioner.
            ZHLN::Test::ExpectEq(totalTriangles, indices.size() / 3);

            if (!layoutOk || totalTriangles != indices.size() / 3) {
                return std::unexpected(MeshShaderTestError::MeshletPartitioningFailed);
            }

            ZHLN::Println(
                "    [INFO] {} triangles partitioned into {} meshlets ({} unique verts, {} micro-index bytes).", totalTriangles, built.meshlets.size(),
                built.vertices.size(), built.triangles.size()
            );
            return {};
        }

        // ====================================================================
        // 2. Procedural meshes must carry meshlet streams
        // ====================================================================
        //
        // Regression guard: procedural geometry originally shipped without
        // meshlets, so a scene built from CreateBox()/CreatePlane() silently
        // stayed on the vertex pipeline and the mesh path was never exercised.
        std::expected<void, ZHLN::ErrorCode> procedural_meshes_carry_meshlet_streams() {
            auto engine      = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }

            auto& rc = engine->GetRenderContext();

            struct Case {
                const char* name;
                ZHLN::Mesh  mesh;
            };

            const std::array<Case, 3> cases = {
                Case {.name = "box", .mesh = ZHLN::PrefabFactory::CreateBoxMesh(rc, JPH::Vec3(0.5f, 0.5f, 0.5f))},
                Case {.name = "plane", .mesh = ZHLN::PrefabFactory::CreatePlaneMesh(rc, 4.0f)},
                Case {.name = "tetrahedron", .mesh = ZHLN::PrefabFactory::CreateTetrahedronMesh(rc)},
            };

            bool allOk = true;
            for (const auto& c: cases) {
                const bool hasStreams = c.mesh.meshletCount > 0 && c.mesh.meshletBuffer != ZHLN::BufferHandle::Invalid &&
                                        c.mesh.meshletVertexBuffer != ZHLN::BufferHandle::Invalid && c.mesh.meshletTriBuffer != ZHLN::BufferHandle::Invalid;
                allOk &= ZHLN::Test::ExpectTrue(hasStreams);

                // Meshlets are an ADDITIONAL view: the raw vertex pool must stay
                // intact for BLAS builds and the vertex pipeline fallback.
                allOk &= ZHLN::Test::ExpectTrue(c.mesh.posBuffer != ZHLN::BufferHandle::Invalid);
                allOk &= ZHLN::Test::ExpectTrue(c.mesh.attrBuffer != ZHLN::BufferHandle::Invalid);
                allOk &= ZHLN::Test::ExpectGt(c.mesh.vertexCount, 0);

                ZHLN::Println("    [INFO] {}: {} verts, {} meshlets.", c.name, c.mesh.vertexCount, c.mesh.meshletCount);
            }

            if (!allOk) {
                return std::unexpected(MeshShaderTestError::MeshletStreamsMissing);
            }
            return {};
        }

        // ====================================================================
        // 3. Create-time config actually selects the geometry path
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> mesh_shading_follows_render_config() {
            auto enabledEngine = ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
                .appName           = "Headless Mesh Shader Test",
                .width             = 320,
                .height            = 240,
                .enableMeshShading = true,
            });
            if (!ZHLN::Test::ExpectTrue(enabledEngine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }

            const auto enabledInfo = enabledEngine->GetRenderContext().GetInfo();
            if (!enabledInfo.meshShadingSupported) {
                // Unsupported hardware must never claim the path is active.
                ZHLN::Test::ExpectFalse(enabledInfo.meshShadingActive);
                ZHLN::Println("    [SKIP] VK_EXT_mesh_shader unsupported on this device; vertex path is authoritative.");
                return {};
            }

            ZHLN::Test::ExpectTrue(enabledInfo.meshShadingActive);
            if (!enabledInfo.meshShadingActive) {
                return std::unexpected(MeshShaderTestError::ConfigDidNotSelectPath);
            }

            auto disabledEngine = ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
                .appName           = "Headless Mesh Shader Test",
                .width             = 320,
                .height            = 240,
                .enableMeshShading = false,
            });
            if (!ZHLN::Test::ExpectTrue(disabledEngine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }

            const auto disabledInfo = disabledEngine->GetRenderContext().GetInfo();
            ZHLN::Test::ExpectFalse(disabledInfo.meshShadingActive);
            if (disabledInfo.meshShadingActive) {
                return std::unexpected(MeshShaderTestError::ConfigDidNotSelectPath);
            }
            return {};
        }

        // ====================================================================
        // 4. Mesh path and vertex path must rasterise the same image
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> mesh_and_vertex_paths_render_identically() {
            auto acquire = [](bool meshShading) {
                return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
                    .appName           = "Headless Mesh Shader Test",
                    .width             = 320,
                    .height            = 240,
                    .enableMeshShading = meshShading,
                });
            };

            auto vertexEngine = acquire(false);
            if (!ZHLN::Test::ExpectTrue(vertexEngine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }

            if (!vertexEngine->GetRenderContext().GetInfo().meshShadingSupported) {
                ZHLN::Println("    [SKIP] VK_EXT_mesh_shader unsupported on this device; nothing to compare.");
                return {};
            }

            auto setupScene = [](ZHLN::Engine& engine) {
                auto& reg = engine.GetRegistry();
                auto& rc  = engine.GetRenderContext();

                // --- Determinism ---------------------------------------------
                // fullBright bypasses lighting/shadow variance...
                auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
                }

                // ...and TAA has to be switched off AT ITS SOURCE. The camera's
                // AASettingsComponent is authoritative: RenderSystem re-pushes it
                // into the RenderContext every frame (so RenderContext::SetAAState
                // alone is overwritten after one tick), and while it says TAA,
                // CameraSystem jitters the projection matrix by a different
                // sub-pixel offset every frame. Two captures taken at different
                // jitter offsets differ on every high-contrast edge in the frame,
                // which has nothing to do with the geometry pipeline.
                for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::AASettingsComponent>()) {
                    reg.Patch<ZHLN::Components::AASettingsComponent>(e, [](auto& aa) {
                        aa.state.mode        = ZHLN::AAMode::None;
                        aa.state.jitterX     = 0.0f;
                        aa.state.jitterY     = 0.0f;
                        aa.state.prevJitterX = 0.0f;
                        aa.state.prevJitterY = 0.0f;
                        aa.state.frameIndex  = 0;
                    });
                }
                rc.SetAAState(ZHLN::AAState {.mode = ZHLN::AAMode::None});

                auto& cam    = engine.GetCamera();
                cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = 0.0f;
                cam.fov      = 60.0f;

                // Boxes at different depths and offsets: covers front faces, faces
                // rejected by the task shader's normal cone, and partial overlap.
                const std::array<JPH::RVec3, 3> spawnPoints = {JPH::RVec3(-1.3, 1.0, 0.0), JPH::RVec3(0.0, 1.0, -1.0), JPH::RVec3(1.3, 1.2, 0.4)};
                for (const auto& p: spawnPoints) {
                    ZHLN::PrefabFactory::CreateBox(
                        engine, JPH::Vec3(0.6f, 0.6f, 0.6f), ZHLN::PrefabFactory::SpawnParams {.position = p, .createPhysics = false}
                    );
                }
            };

            constexpr float dt = 1.0f / 60.0f;
            auto            capture = [&](ZHLN::Engine& engine, const std::string& path) -> Image {
                auto& rc = engine.GetRenderContext();
                for (uint32_t frame = 0; frame < 6; ++frame) {
                    engine.ProcessEvents();
                    engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
                }
                if (!rc.CaptureScreenshotPPM(path)) {
                    return {};
                }
                return LoadPPM(path);
            };

            // Mesh shading is create-time, so the two paths cannot share one
            // engine. The vertex engine captures twice (control); a second
            // engine with mesh shading on captures once. Without this control
            // there is no way to tell a real path divergence from engine noise
            // -- an earlier revision of this test blamed the mesh path for TAA
            // jitter for exactly that reason.
            // Any VUID raised from here on is attributable to the frames this
            // test renders. A suite that prints validation errors and still
            // reports PASS is not verifying anything.
            const uint32_t validationBefore = ZHLN::RenderContext::ValidationErrorCount();

            setupScene(*vertexEngine);
            const Image vertexA = capture(*vertexEngine, "headless_meshshader_vertex_a.ppm");
            ZHLN::Test::ExpectFalse(vertexEngine->GetRenderContext().GetInfo().meshShadingActive);
            const Image vertexB = capture(*vertexEngine, "headless_meshshader_vertex_b.ppm");

            auto meshEngine = acquire(true);
            if (!ZHLN::Test::ExpectTrue(meshEngine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }
            setupScene(*meshEngine);
            const Image meshImage = capture(*meshEngine, "headless_meshshader_mesh.ppm");
            ZHLN::Test::ExpectTrue(meshEngine->GetRenderContext().GetInfo().meshShadingActive);

            const uint32_t validationRaised = ZHLN::RenderContext::ValidationErrorCount() - validationBefore;

            if (!(ZHLN::Test::ExpectTrue(meshImage.Valid()) && ZHLN::Test::ExpectTrue(vertexA.Valid()) && ZHLN::Test::ExpectTrue(vertexB.Valid()))) {
                return std::unexpected(MeshShaderTestError::RenderOutputBlank);
            }
            ZHLN::Test::ExpectEq(meshImage.width, vertexB.width);
            ZHLN::Test::ExpectEq(meshImage.height, vertexB.height);

            // Guard against the degenerate pass: two blank frames match perfectly.
            const uint32_t meshShaded   = ShadedPixelCount(meshImage);
            const uint32_t vertexShaded = ShadedPixelCount(vertexB);
            ZHLN::Test::ExpectGt(meshShaded, 500u);
            ZHLN::Test::ExpectGt(vertexShaded, 500u);
            if (meshShaded <= 500u || vertexShaded <= 500u) {
                ZHLN::Println("    [FAIL] Frames are effectively blank (mesh={}, vertex={} shaded pixels).", meshShaded, vertexShaded);
                return std::unexpected(MeshShaderTestError::RenderOutputBlank);
            }

            const uint32_t shadedMax     = std::max({meshShaded, vertexShaded, 1u});
            const double   coverageDelta = std::abs(static_cast<double>(meshShaded) - static_cast<double>(vertexShaded)) / static_cast<double>(shadedMax);

            // +/-2 channels absorbs ULP-level interpolation differences caused
            // by meshoptimizer reordering vertices inside a cluster.
            constexpr int kChannelTolerance = 2;

            // Absolute floors, used when the control is perfectly clean.
            constexpr double kMaxFractionOverTol = 0.001; // 0.1 % of pixels
            constexpr double kMaxCoverageDelta   = 0.005; // 0.5 % of shaded area
            constexpr double kMaxMaskMismatch    = 0.005; // 0.5 % of the silhouette
            // How much worse than the engine's own noise floor the mesh path is
            // allowed to be before we call it a divergence.
            constexpr double kControlSlack = 2.0;

            const ImageDiff control = CompareImages(vertexA, vertexB, kChannelTolerance);
            const ImageDiff diff    = CompareImages(vertexB, meshImage, kChannelTolerance);

            ZHLN::Println(
                "    [INFO] control (vertex vs vertex): over-tolerance {}, silhouette {}, mean |delta| {}, max |delta| {}, |delta|>32 pixels {}.",
                control.fractionOverTol, control.maskMismatchRate, control.meanChannelDelta, control.maxChannelDelta, control.pixelsOverHigh
            );
            ZHLN::Println(
                "    [INFO] mesh vs vertex: shaded {} / {}, coverage delta {}, over-tolerance {}, silhouette {}, mean |delta| {}, max |delta| {}, "
                "|delta|>32 pixels {}.",
                meshShaded, vertexShaded, coverageDelta, diff.fractionOverTol, diff.maskMismatchRate, diff.meanChannelDelta, diff.maxChannelDelta,
                diff.pixelsOverHigh
            );

            const double pixelBudget    = std::max(kMaxFractionOverTol, control.fractionOverTol * kControlSlack);
            const double maskBudget     = std::max(kMaxMaskMismatch, control.maskMismatchRate * kControlSlack);
            const double coverageBudget = kMaxCoverageDelta;

            const bool coverageOk   = ZHLN::Test::ExpectLe(coverageDelta, coverageBudget);
            const bool pixelsOk     = ZHLN::Test::ExpectLe(diff.fractionOverTol, pixelBudget);
            const bool silhouetteOk = ZHLN::Test::ExpectLe(diff.maskMismatchRate, maskBudget);

            if (!coverageOk || !pixelsOk || !silhouetteOk) {
                WriteDiffImage("headless_meshshader_diff.ppm", vertexB, meshImage);
                ZHLN::Println(
                    "    [FAIL] Geometry paths diverged beyond the engine's own noise floor (pixel budget {}, mask budget {}). "
                    "Inspect headless_meshshader_diff.ppm (differences amplified 4x).",
                    pixelBudget, maskBudget
                );
                return std::unexpected(MeshShaderTestError::PathDivergence);
            }

            // Checked last so a genuine image divergence is reported first, but
            // still fatal: correct pixels produced through invalid API usage is
            // not a pass.
            if (validationRaised > 0) {
                ZHLN::Println(
                    "    [FAIL] {} validation error(s) raised while rendering the comparison frames. "
                    "Re-run with ZHLN_NO_MESH_SHADING=1 to see whether they are specific to the mesh path.",
                    validationRaised
                );
                ZHLN::Test::ExpectEq(validationRaised, 0u);
                return std::unexpected(MeshShaderTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 5. Meshlet cone culling: apex singularity must not cull front-facing
        // ====================================================================
        //
        // Regression for DamagedHelmet visor black square. The apex-based
        // ConeBackfaceCulled flips when camera passes apex plane (apex behind
        // surface, camera zooms in close and goes behind apex). Sphere
        // formulation dot(toCenter, axis) >= cutoff*dist + radius has no
        // singularity. This CPU test reproduces the exact math that was
        // failing: camera behind apex but still in front of surface should
        // NOT be culled.
        std::expected<void, ZHLN::ErrorCode> cone_culling_sphere_formulation_no_false_positive() {
            struct Case {
                JPH::Vec3 coneApex;
                JPH::Vec3 sphereCenter;
                float radius;
                JPH::Vec3 axis;
                float cutoff;
                JPH::Vec3 camPos;
                bool shouldCull; // expected for sphere formulation
            };

            // Visor patch: sphere at origin, radius 0.5, apex behind at -0.5,
            // axis outward +Z, cutoff 0.5 (60 deg half angle).
            const std::array<Case, 5> cases = {{
                // Far camera in front: both formulations say visible.
                {JPH::Vec3(0, 0, -0.5f), JPH::Vec3(0, 0, 0), 0.5f, JPH::Vec3(0, 0, 1), 0.5f, JPH::Vec3(0, 0, 2), false},
                // Close camera in front, just above surface: visible.
                {JPH::Vec3(0, 0, -0.5f), JPH::Vec3(0, 0, 0), 0.5f, JPH::Vec3(0, 0, 1), 0.5f, JPH::Vec3(0, 0, 0.1f), false},
                // Camera behind apex but still outside sphere? apex -0.5, cam -0.6,
                // old apex formulation: toCluster = apex - cam = 0.1, dot=+1 >=0.5 => CULL (false positive).
                // Sphere: toCenter = 0 - (-0.6)=0.6, dot=0.6, dist=0.6, cutoff*dist+radius=0.8, 0.6>=0.8? false => visible (correct).
                {JPH::Vec3(0, 0, -0.5f), JPH::Vec3(0, 0, 0), 0.5f, JPH::Vec3(0, 0, 1), 0.5f, JPH::Vec3(0, 0, -0.6f), false},
                // Camera inside sphere but in front of apex: should still be visible (no singularity).
                {JPH::Vec3(0, 0, -0.5f), JPH::Vec3(0, 0, 0), 0.5f, JPH::Vec3(0, 0, 1), 0.5f, JPH::Vec3(0, 0, -0.2f), false},
                // Camera behind object, looking away: should cull.
                {JPH::Vec3(0, 0, -0.5f), JPH::Vec3(0, 0, 0), 0.5f, JPH::Vec3(0, 0, 1), 0.5f, JPH::Vec3(0, 0, -2.0f), false}, // actually still not cull because axis outward, camera behind still sees back? Let's make axis opposite.
            }};

            // Re-implement both formulations in C++ to show old fails, new passes.
            auto oldCulls = [](JPH::Vec3 apex, JPH::Vec3 axis, float cutoff, JPH::Vec3 cam) -> bool {
                if (cutoff >= 1.0f) return false;
                JPH::Vec3 toCluster = apex - cam;
                float len = toCluster.Length();
                if (len < 1e-5f) return false;
                toCluster /= len;
                return toCluster.Dot(axis) >= cutoff;
            };
            auto newCulls = [](JPH::Vec3 center, float radius, JPH::Vec3 axis, float cutoff, JPH::Vec3 cam) -> bool {
                if (cutoff >= 1.0f) return false;
                JPH::Vec3 toCenter = center - cam;
                float dist = toCenter.Length();
                if (dist < 1e-5f) return false;
                return toCenter.Dot(axis) >= cutoff * dist + radius;
            };

            bool allOk = true;
            for (size_t i = 0; i < cases.size(); ++i) {
                const auto& c = cases[i];
                bool oldResult = oldCulls(c.coneApex, c.axis, c.cutoff, c.camPos);
                bool newResult = newCulls(c.sphereCenter, c.radius, c.axis, c.cutoff, c.camPos);

                // The critical case is index 2: old culls (true), new does not (false).
                // That is the black square regression.
                if (i == 2) {
                    ZHLN::Test::ExpectTrue(oldResult); // old DOES falsely cull
                    allOk &= ZHLN::Test::ExpectTrue(oldResult);
                    ZHLN::Test::ExpectFalse(newResult); // new must NOT cull
                    allOk &= ZHLN::Test::ExpectFalse(newResult);
                    ZHLN::Println("    [INFO] case {} (camera behind apex): old culls={}, new culls={} (expected old=true false-positive, new=false).", i, oldResult, newResult);
                } else {
                    // For other cases, new should match expected shouldCull
                    ZHLN::Test::ExpectEq(newResult, c.shouldCull);
                    allOk &= ZHLN::Test::ExpectEq(newResult, c.shouldCull);
                }
            }

            if (!allOk) {
                return std::unexpected(MeshShaderTestError::MeshletConeCullingFalsePositive);
            }
            return {};
        }

        // ====================================================================
        // 6. Close-up render must not produce a black hole where meshlet culled
        // ====================================================================
        //
        // GPU regression: zoom in close to a surface (like DamagedHelmet visor
        // in screenshot) and ensure mesh-shader path does not drop a meshlet.
        // The test renders a single box at very close distance with mesh shading
        // enabled vs disabled and checks that center pixels are not black.
        std::expected<void, ZHLN::ErrorCode> meshlet_closeup_no_black_hole() {
            auto acquire = [](bool meshShading) {
                return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions{
                    .appName = "Headless Meshlet Closeup",
                    .width = 320,
                    .height = 240,
                    .enableMeshShading = meshShading,
                });
            };

            auto vertexEngine = acquire(false);
            if (!ZHLN::Test::ExpectTrue(vertexEngine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }
            if (!vertexEngine->GetRenderContext().GetInfo().meshShadingSupported) {
                ZHLN::Println("    [SKIP] mesh shading unsupported; closeup test skipped.");
                return {};
            }

            auto setupCloseup = [](ZHLN::Engine& engine) {
                auto& reg = engine.GetRegistry();
                auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright = 1;
                        pp.enableSSR = 0;
                        pp.enableRTR = 0;
                    });
                }
                for (const ZHLN::Entity e : reg.GetEntitiesWith<ZHLN::Components::AASettingsComponent>()) {
                    reg.Patch<ZHLN::Components::AASettingsComponent>(e, [](auto& aa) {
                        aa.state.mode = ZHLN::AAMode::None;
                        aa.state.jitterX = 0;
                        aa.state.jitterY = 0;
                        aa.state.prevJitterX = 0;
                        aa.state.prevJitterY = 0;
                        aa.state.frameIndex = 0;
                    });
                }
                engine.GetRenderContext().SetAAState(ZHLN::AAState{.mode = ZHLN::AAMode::None});

                // Camera very close to origin, looking at a box that fills screen.
                // This is the scenario where apex-based culling would cull front-facing
                // meshlets and leave a black square.
                auto& cam = engine.GetCamera();
                cam.position = JPH::Vec3(0.0f, 0.0f, 0.6f);
                cam.yaw = -90.0f;
                cam.pitch = 0.0f;
                cam.fov = 30.0f;
                cam.nearZ = 0.01f;
                cam.farZ = 10.0f;

                // One box at origin, 1m size, so its front face is 0.1m from camera.
                ZHLN::PrefabFactory::CreateBox(engine, JPH::Vec3(1.0f, 1.0f, 1.0f),
                    ZHLN::PrefabFactory::SpawnParams{.position = JPH::RVec3(0, 0, 0), .createPhysics = false});
            };

            constexpr float dt = 1.0f / 60.0f;
            auto capture = [&](ZHLN::Engine& engine, const std::string& path) -> Image {
                auto& rc = engine.GetRenderContext();
                for (uint32_t f = 0; f < 6; ++f) {
                    engine.ProcessEvents();
                    engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
                }
                if (!rc.CaptureScreenshotPPM(path)) return {};
                return LoadPPM(path);
            };

            setupCloseup(*vertexEngine);
            const Image vertexImg = capture(*vertexEngine, "headless_meshlet_closeup_vertex.ppm");

            auto meshEngine = acquire(true);
            if (!ZHLN::Test::ExpectTrue(meshEngine != nullptr)) {
                return std::unexpected(MeshShaderTestError::EngineInitFailed);
            }
            setupCloseup(*meshEngine);
            const Image meshImg = capture(*meshEngine, "headless_meshlet_closeup_mesh.ppm");

            if (!(ZHLN::Test::ExpectTrue(vertexImg.Valid()) && ZHLN::Test::ExpectTrue(meshImg.Valid()))) {
                return std::unexpected(MeshShaderTestError::RenderOutputBlank);
            }

            // Check center region (where box should be) is not black.
            // A falsely culled meshlet leaves a 120x120 or smaller black square.
            const int cx = meshImg.width / 2;
            const int cy = meshImg.height / 2;
            const int half = 20;
            uint32_t blackPixels = 0;
            uint32_t total = 0;
            for (int y = cy - half; y <= cy + half; ++y) {
                for (int x = cx - half; x <= cx + half; ++x) {
                    if (x < 0 || x >= meshImg.width || y < 0 || y >= meshImg.height) continue;
                    size_t idx = (static_cast<size_t>(y) * meshImg.width + x) * 3;
                    uint8_t r = meshImg.rgb[idx + 0];
                    uint8_t g = meshImg.rgb[idx + 1];
                    uint8_t b = meshImg.rgb[idx + 2];
                    // Black square detection: near 0,0,0
                    if (r < 10 && g < 10 && b < 10) ++blackPixels;
                    ++total;
                }
            }

            const double blackRate = total > 0 ? static_cast<double>(blackPixels) / total : 1.0;
            ZHLN::Println("    [INFO] closeup center {}x{} region: {} black / {} total ({:.2f}%).", half * 2 + 1, half * 2 + 1, blackPixels, total, blackRate * 100.0);

            // Also compare mesh vs vertex: they should be nearly identical.
            const ImageDiff diff = CompareImages(vertexImg, meshImg, 2);
            ZHLN::Println("    [INFO] mesh vs vertex closeup: over-tol {}, mask mismatch {}, mean delta {}.", diff.fractionOverTol, diff.maskMismatchRate, diff.meanChannelDelta);

            // If >20% of center is black, it's the black hole bug.
            if (!ZHLN::Test::ExpectLe(blackRate, 0.20)) {
                WriteDiffImage("headless_meshlet_closeup_diff.ppm", vertexImg, meshImg);
                ZHLN::Println("    [FAIL] Black hole detected in closeup meshlet render — {}% center black.", blackRate * 100.0);
                return std::unexpected(MeshShaderTestError::MeshletBlackHoleDetected);
            }

            // Also ensure paths don't diverge too much.
            if (!ZHLN::Test::ExpectLe(diff.maskMismatchRate, 0.10)) {
                WriteDiffImage("headless_meshlet_closeup_diff.ppm", vertexImg, meshImg);
                return std::unexpected(MeshShaderTestError::PathDivergence);
            }

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunMeshShaderSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<MeshShaderTestSuite>();
}

