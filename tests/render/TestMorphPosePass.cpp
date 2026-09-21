// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestMorphPosePass.cpp
//
// Guards the morph half of AnimationSystem's parallel pose pass -- the only
// part of it that touches the registry.
//
// UpdateAnimations evaluates one animated root per TaskSystem chunk, and until
// recently each chunk called `reg.Add<Components::MorphTargetComponent>` for
// every mesh child whose node carried a weights channel. Add is not
// synchronized and cannot be (see the contract on Registry::Add: Create() calls
// it while holding sync.shadowLock, and that lock panics on re-entry), so chunks
// inserted into one SparseSet concurrently -- one count, one dense array, one
// sparse table, plus ResizeDense's realloc underneath a writer another chunk was
// already using. The fix was deletion, not deferral: an EntityCommandBuffer
// records through a plain std::vector, so recording from a chunk races the same
// way, and nothing needed the insert -- the factory is the only writer that can
// set MorphTargetComponent::offset and it attaches the component in the same
// breath as the deltas it points at.
//
// This suite pins three claims, all of them things that deletion could have
// broken quietly:
//
//   1. The pose pass writes weights only into a MorphTargetComponent that
//      already exists. It never inserts one, on any entity, however many
//      instances and frames are in flight. Half the instances below have their
//      component removed before ticking: if an Add comes back, those components
//      reappear and the dense array grows during a tick.
//   2. A weights channel with no key times is skipped, not divided by. That
//      expression was `keyValues.size() / keyTimes.size()` -- integer division
//      by an empty vector's size. UB, and SIGFPE on x86: the process died
//      instead of failing a case.
//   3. The weights the pass writes still reach the frame. A write-only pass that
//      silently stops writing looks exactly like a pass with nothing to do, so
//      the last case renders a fully-weighted mesh and demands the silhouette
//      actually change.
//
// The prefab is built by hand rather than imported, and that is a statement
// about the tree rather than a shortcut: extras/glTF parses `targets` into
// PrimitiveJob::tempDeltas / activeMorphCount but nothing ever assigns either,
// so no prefab the importer can produce has morph deltas, and every model in
// resources/ is imported without them. A test that went through the importer
// would be measuring the absence of a producer. Building the ModelPrefab
// directly runs the same instantiate -> pose -> extract -> shader path with the
// fields a producer will fill in; when the importer (or the cooker) starts
// setting activeMorphCount, this suite starts covering it for free, because a
// factory-attached component is already what it asserts on.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

enum class MorphPosePassTestError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for the morph pose pass test."> {}) = 1,
    MaterialCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"RenderContext::CreateMaterial failed while building the morph material."> {}),
    InstanceSpawnFailed ZHLN_ANNOTATION(ZHLN::Description<"InstantiatePrefab produced no mesh child under the animator root it spawned."> {}),
    DegenerateClipMissing ZHLN_ANNOTATION(ZHLN::Description<"The fixture that is supposed to carry the keyless weights channel does not have it."> {}),
    MorphComponentMissing ZHLN_ANNOTATION(ZHLN::Description<"The factory did not attach MorphTargetComponent to a part with activeMorphCount > 0."> {}),
    PosePassAttachedMorphComponent ZHLN_ANNOTATION(
        ZHLN::Description<"AnimationSystem inserted a MorphTargetComponent from its parallel pose pass; it must write only into components the factory attached."> {}
    ),
    MorphWeightsNotWritten ZHLN_ANNOTATION(ZHLN::Description<"A valid weights channel did not advance MorphTargetComponent's weights."> {}),
    DegenerateWeightsChannelWritten ZHLN_ANNOTATION(ZHLN::Description<"A weights channel with no key times changed morph state instead of being skipped."> {}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"Frame capture failed during the morph deformation test."> {}),
    MorphDeformationNotVisible ZHLN_ANNOTATION(ZHLN::Description<"Fully weighted morph targets did not change the rendered frame."> {}),
};

struct MorphPosePassSuite {
    // Morph targets per part. The pose pass clamps a node's channel to four;
    // two is enough to prove per-target weights are not crossed.
    static constexpr uint32_t kMorphTargets = 2;

    // Deltas large enough to move the silhouette by tens of pixels at the
    // framing the last case uses, so "the frame changed" cannot be satisfied by
    // antialiasing noise.
    static constexpr float kMorphDelta = 0.8f;

    // Second key of the weights channel, in seconds of clip time. Long enough
    // that a handful of settle frames leave the weights near zero (the first
    // captured frame is the undeformed box) and short enough that a couple of
    // seconds of ticking clamps to full weight.
    static constexpr float kKeySpanSeconds = 2.0f;

    static constexpr uint32_t kStressInstances = 32;
    // 2.5 s against a 2.0 s last key: the sample clamps, so the expected
    // weights are exactly 1.0 rather than approximately.
    static constexpr uint32_t kStressFrames = 150;

    static constexpr uint32_t kDegenerateInstances = 8;
    static constexpr uint32_t kDegenerateFrames    = 60;

    // What the factory writes into a fresh MorphTargetComponent. Distinctive on
    // purpose: a skipped channel must leave these untouched.
    static constexpr float kDefaultMorphWeight0 = 0.2f;
    static constexpr float kDefaultMorphWeight1 = 0.4f;

    MorphPosePassSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~MorphPosePassSuite() { ZHLN::Test::Headless::EndSession(); }

    // Pooled, like every other suite in this group: the scene is what gets
    // thrown away between tests, and each test below spawns its own instances.
    [[nodiscard]] static auto CreateTestEngine() -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(
            ZHLN::Test::Headless::EngineOptions {.appName = "Headless Morph Pose Pass", .width = 640, .height = 480}
        );
    }

    [[nodiscard]] static auto CreateMorphMaterial(ZHLN::RenderContext& rc) -> std::expected<ZHLN::Material, ZHLN::ErrorCode> {
        return rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.8f, .baseColor = {0.85f, 0.45f, 0.2f, 1.0f}});
    }

    struct BuiltPrefab {
        ZHLN::ModelPrefab prefab;
    };

    // One box part under one root node, `kMorphTargets` morph targets with real
    // deltas in the shared pool, and a weights clip.
    //
    // `degenerateClip` appends the channel claim 2 is about: same path, same
    // target node, keyValues but no keyTimes.
    [[nodiscard]] static auto BuildMorphPrefab(ZHLN::RenderContext& rc, bool degenerateClip) -> BuiltPrefab {
        BuiltPrefab built;

        const ZHLN::Mesh box = ZHLN::CreativeWorksFactory::CreateBoxMesh(rc, JPH::Vec3(0.6f, 0.6f, 0.6f), JPH::Vec4(0.85f, 0.45f, 0.2f, 1.0f));

        // Deltas are float4-per-vertex blocks laid out target-major:
        // common.slang's GetMorphDisplacement reads
        // `scene.g_morphDeltas[morphOffset + i * vertexCount + vertexId]`, and
        // AllocateMorphDeltas counts those float4s, not floats.
        std::vector<float> deltas(static_cast<size_t>(box.vertexCount) * kMorphTargets * 4, 0.0f);
        for (uint32_t vertex = 0; vertex < box.vertexCount; ++vertex) {
            deltas[(static_cast<size_t>(vertex) * 4) + 1]                     = kMorphDelta; // target 0: +Y
            deltas[((static_cast<size_t>(box.vertexCount) + vertex) * 4) + 0] = kMorphDelta; // target 1: +X
        }

        ZHLN::ModelPart part {};
        part.name                   = ZHLN::String64("MorphBody");
        part.nodeIndex              = 1;
        part.localTransform         = JPH::Mat44::sIdentity();
        part.mesh                   = box;
        part.meshAsset              = ZHLN::HashAssetID("zahlen_test_morph_pose_box_mesh");
        part.materialAsset          = ZHLN::HashAssetID("zahlen_test_morph_pose_box_material");
        part.boundingRadius         = 2.0f;
        part.morphOffset            = rc.AllocateMorphDeltas(static_cast<uint32_t>(box.vertexCount) * kMorphTargets, deltas.data());
        part.activeMorphCount       = kMorphTargets;
        part.defaultMorphWeights[0] = kDefaultMorphWeight0;
        part.defaultMorphWeights[1] = kDefaultMorphWeight1;

        built.prefab.virtualPath = "zahlen_test_morph_pose_prefab";
        built.prefab.parts.push_back(part);

        built.prefab.nodes.resize(2);
        built.prefab.nodes[0].name           = ZHLN::String64("MorphRoot");
        built.prefab.nodes[0].parentIndex    = -1;
        built.prefab.nodes[0].localTransform = JPH::Mat44::sIdentity();
        built.prefab.nodes[0].hasMesh        = false;
        built.prefab.nodes[1].name           = ZHLN::String64("MorphBody");
        built.prefab.nodes[1].parentIndex    = 0;
        built.prefab.nodes[1].localTransform = JPH::Mat44::sIdentity();
        built.prefab.nodes[1].hasMesh        = true;

        ZHLN::AnimationClip weightsClip;
        weightsClip.name     = ZHLN::String64("MorphWeights");
        weightsClip.duration = 10.0f;
        weightsClip.channels.push_back(
            ZHLN::AnimationChannel {
                .targetNodeIndex = 1,
                .path            = ZHLN::AnimationPathType::Weights,
                .interpolation   = ZHLN::InterpolationType::Linear,
                .keyTimes        = {0.0f, kKeySpanSeconds},
                .keyValues       = {0.0f, 0.0f, 1.0f, 1.0f}, // key 0: no morph, key 1: both targets fully weighted
            }
        );
        built.prefab.animations.push_back(std::move(weightsClip));

        if (degenerateClip) {
            ZHLN::AnimationClip keyless;
            keyless.name     = ZHLN::String64("MorphWeightsNoKeys");
            keyless.duration = 1.0f;
            keyless.channels.push_back(
                ZHLN::AnimationChannel {
                    .targetNodeIndex = 1,
                    .path            = ZHLN::AnimationPathType::Weights,
                    .interpolation   = ZHLN::InterpolationType::Linear,
                    .keyTimes        = {},             // <- the denominator that used to be zero
                    .keyValues       = {0.25f, 0.75f}, // weights with no times to interpolate them at
                }
            );
            built.prefab.animations.push_back(std::move(keyless));
        }

        return built;
    }

    struct Instance {
        ZHLN::Entity root = ZHLN::Entity::Null();
        ZHLN::Entity mesh = ZHLN::Entity::Null();
    };

    // Spawns `count` instances of `prefab`, laid out in a row so the stress case
    // is not 32 meshes fighting over one world position.
    [[nodiscard]] static auto SpawnInstances(
        ZHLN::Engine& engine, const ZHLN::ModelPrefab& prefab, const ZHLN::Material& material, uint32_t count
    ) -> std::vector<Instance> {
        auto& reg = engine.GetRegistry();

        for (uint32_t i = 0; i < count; ++i) {
            const float x = (static_cast<float>(i) - (static_cast<float>(count) - 1.0f) * 0.5f) * 1.5f;
            (void) ZHLN::CreativeWorksFactory::InstantiatePrefab(
                engine, prefab,
                ZHLN::CreativeWorksFactory::SpawnParams {
                    .position = JPH::RVec3(x, 1.0f, 0.0f), .createPhysics = false, .isAnimated = true, .materialOverride = material
                }
            );
        }

        // Pair each animator root with its mesh child by walking the mesh
        // entities and keeping the ones whose parent carries *this* prefab.
        // Spawn order is not a contract; the prefab pointer is.
        std::vector<Instance> instances;
        for (ZHLN::Entity mesh: reg.GetEntitiesWith<ZHLN::Components::MeshComponent>()) {
            auto* hier = reg.Get<ZHLN::Components::HierarchyComponent>(mesh);
            if (hier == nullptr || hier->parent == ZHLN::Entity::Null()) {
                continue;
            }
            auto* animator = reg.Get<ZHLN::Components::AnimatorComponent>(hier->parent);
            if (animator == nullptr || animator->prefab != &prefab) {
                continue;
            }
            instances.push_back(Instance {.root = hier->parent, .mesh = mesh});
        }
        return instances;
    }

    struct Tests {
        // ====================================================================
        // 1. The pose pass writes weights; it never inserts components.
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> parallel_pose_pass_writes_weights_without_inserting_components() {
            auto engine = MorphPosePassSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(MorphPosePassTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            auto material = MorphPosePassSuite::CreateMorphMaterial(rc);
            if (!material) {
                return std::unexpected(MorphPosePassTestError::MaterialCreationFailed);
            }

            auto built     = MorphPosePassSuite::BuildMorphPrefab(rc, /*degenerateClip=*/false);
            auto instances = MorphPosePassSuite::SpawnInstances(*engine, built.prefab, *material, MorphPosePassSuite::kStressInstances);
            if (instances.size() != MorphPosePassSuite::kStressInstances) {
                return std::unexpected(MorphPosePassTestError::InstanceSpawnFailed);
            }

            const size_t attachedBefore = reg.GetRawArray<ZHLN::Components::MorphTargetComponent>().size();
            ZHLN::Test::ExpectEq(attachedBefore, instances.size());
            if (attachedBefore != instances.size()) {
                return std::unexpected(MorphPosePassTestError::MorphComponentMissing);
            }

            // Strip half of them. Before the fix the pose pass re-attached
            // exactly these, one insert per chunk into one SparseSet: the array
            // size below is the guard, and those inserts are the corruption.
            const size_t stripped = instances.size() / 2;
            for (size_t i = 0; i < stripped; ++i) {
                reg.Remove<ZHLN::Components::MorphTargetComponent>(instances[i].mesh);
            }
            ZHLN::Test::ExpectEq(reg.GetRawArray<ZHLN::Components::MorphTargetComponent>().size(), attachedBefore - stripped);

            ZHLN::Test::Headless::TickFrames(*engine, MorphPosePassSuite::kStressFrames);

            const size_t attachedAfter = reg.GetRawArray<ZHLN::Components::MorphTargetComponent>().size();
            ZHLN::Println(
                "    [INFO] MorphTargetComponent dense array across {} frames x {} instances: {} entries before, {} after ({} stripped).",
                MorphPosePassSuite::kStressFrames, MorphPosePassSuite::kStressInstances, attachedBefore, attachedAfter, stripped
            );
            ZHLN::Test::ExpectEq(attachedAfter, attachedBefore - stripped);
            if (attachedAfter != attachedBefore - stripped) {
                return std::unexpected(MorphPosePassTestError::PosePassAttachedMorphComponent);
            }

            for (size_t i = 0; i < instances.size(); ++i) {
                auto* morph = reg.Get<ZHLN::Components::MorphTargetComponent>(instances[i].mesh);

                if (i < stripped) {
                    ZHLN::Test::ExpectTrue(morph == nullptr);
                    if (morph != nullptr) {
                        return std::unexpected(MorphPosePassTestError::PosePassAttachedMorphComponent);
                    }
                    continue;
                }

                if (!ZHLN::Test::ExpectTrue(morph != nullptr)) {
                    return std::unexpected(MorphPosePassTestError::MorphComponentMissing);
                }

                // 2.5 s of clip time against a 2.0 s last key: the sample clamps,
                // so this is exact arithmetic, not a tolerance.
                ZHLN::Test::ExpectEq(morph->activeCount, MorphPosePassSuite::kMorphTargets);
                ZHLN::Test::ExpectEq(morph->weights[0], 1.0f);
                ZHLN::Test::ExpectEq(morph->weights[1], 1.0f);
                ZHLN::Test::ExpectEq(morph->weights[2], 0.0f);
                ZHLN::Test::ExpectEq(morph->weights[3], 0.0f);
                if (morph->weights[0] != 1.0f || morph->weights[1] != 1.0f) {
                    return std::unexpected(MorphPosePassTestError::MorphWeightsNotWritten);
                }
            }

            return {};
        }

        // ====================================================================
        // 2. A weights channel with no key times is skipped, not divided by.
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> weights_channel_without_key_times_is_skipped_not_divided() {
            auto engine = MorphPosePassSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(MorphPosePassTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            auto material = MorphPosePassSuite::CreateMorphMaterial(rc);
            if (!material) {
                return std::unexpected(MorphPosePassTestError::MaterialCreationFailed);
            }

            auto built = MorphPosePassSuite::BuildMorphPrefab(rc, /*degenerateClip=*/true);
            auto instances =
                MorphPosePassSuite::SpawnInstances(*engine, built.prefab, *material, MorphPosePassSuite::kDegenerateInstances);
            if (instances.size() != MorphPosePassSuite::kDegenerateInstances) {
                return std::unexpected(MorphPosePassTestError::InstanceSpawnFailed);
            }
            if (built.prefab.animations.size() != 2) {
                return std::unexpected(MorphPosePassTestError::DegenerateClipMissing);
            }

            // Half stay on the valid clip as the control: whatever the degenerate
            // half looks like afterwards has to be the channel, not the scene.
            for (size_t i = 0; i < instances.size(); ++i) {
                if (i < instances.size() / 2) {
                    ZHLN::Test::ExpectTrue(reg.Patch<ZHLN::Components::AnimatorComponent>(instances[i].root, [](auto& animator) -> void {
                        animator.currentTrackIdx = 1;
                    }));
                }
            }

            // Before the fix this tick was where the process died: keyValues.size()
            // / keyTimes.size() with an empty keyTimes is integer division by zero.
            ZHLN::Test::Headless::TickFrames(*engine, MorphPosePassSuite::kDegenerateFrames);

            const size_t degenerateCount = instances.size() / 2;
            for (size_t i = 0; i < instances.size(); ++i) {
                auto* morph = reg.Get<ZHLN::Components::MorphTargetComponent>(instances[i].mesh);
                if (!ZHLN::Test::ExpectTrue(morph != nullptr)) {
                    return std::unexpected(MorphPosePassTestError::MorphComponentMissing);
                }

                if (i < degenerateCount) {
                    // The channel is skipped, so the component keeps what the
                    // factory wrote -- count and weights alike. Nothing divides,
                    // nothing NaNs, and nothing resets the pose mid-clip.
                    ZHLN::Test::ExpectEq(morph->activeCount, MorphPosePassSuite::kMorphTargets);
                    ZHLN::Test::ExpectEq(morph->weights[0], MorphPosePassSuite::kDefaultMorphWeight0);
                    ZHLN::Test::ExpectEq(morph->weights[1], MorphPosePassSuite::kDefaultMorphWeight1);
                    ZHLN::Test::ExpectEq(morph->weights[2], 0.0f);
                    ZHLN::Test::ExpectEq(morph->weights[3], 0.0f);
                    if (morph->weights[0] != MorphPosePassSuite::kDefaultMorphWeight0 ||
                        morph->weights[1] != MorphPosePassSuite::kDefaultMorphWeight1) {
                        return std::unexpected(MorphPosePassTestError::DegenerateWeightsChannelWritten);
                    }
                    continue;
                }

                // 1.0 s into a 2.0 s key span: half-way through the smoothstep,
                // so the band is wide enough to survive float accumulation.
                ZHLN::Test::ExpectEq(morph->activeCount, MorphPosePassSuite::kMorphTargets);
                ZHLN::Test::ExpectInRange(morph->weights[0], 0.3f, 0.7f);
                ZHLN::Test::ExpectInRange(morph->weights[1], 0.3f, 0.7f);
                if (morph->weights[0] < 0.3f || morph->weights[0] > 0.7f) {
                    return std::unexpected(MorphPosePassTestError::MorphWeightsNotWritten);
                }
            }

            return {};
        }

        // ====================================================================
        // 3. The weights the pass writes reach the frame.
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> morph_weights_deform_the_rendered_frame() {
            auto engine = MorphPosePassSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(MorphPosePassTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Straight-on view of the only mesh in the scene, so the frame is a
            // silhouette comparison and nothing else moves between captures.
            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;
            ZHLN::Test::Headless::DisableTAA(*engine);

            auto material = MorphPosePassSuite::CreateMorphMaterial(rc);
            if (!material) {
                return std::unexpected(MorphPosePassTestError::MaterialCreationFailed);
            }

            auto built     = MorphPosePassSuite::BuildMorphPrefab(rc, /*degenerateClip=*/false);
            auto instances = MorphPosePassSuite::SpawnInstances(*engine, built.prefab, *material, 1);
            if (instances.empty()) {
                return std::unexpected(MorphPosePassTestError::InstanceSpawnFailed);
            }

            // Settle culling with the clip near its first key: 10 frames is
            // 0.167 s of a 2.0 s span, so the sampled weights are under a percent
            // and this frame is the box at its authored size.
            ZHLN::Test::Headless::TickFrames(*engine, 10);
            auto* settled = reg.Get<ZHLN::Components::MorphTargetComponent>(instances[0].mesh);
            if (!ZHLN::Test::ExpectTrue(settled != nullptr)) {
                return std::unexpected(MorphPosePassTestError::MorphComponentMissing);
            }
            ZHLN::Test::ExpectInRange(settled->weights[0], 0.0f, 0.1f);

            const auto before = ZHLN::Test::Headless::Capture(*engine, "morph_pose_weights_zero.ppm");
            if (!ZHLN::Test::ExpectTrue(before.Valid())) {
                return std::unexpected(MorphPosePassTestError::CaptureFailed);
            }

            // Past the last key. A fully weighted box is half again as large on
            // two axes, so a frame that does not change here means the weights
            // never reached the draw path.
            ZHLN::Test::Headless::TickFrames(*engine, MorphPosePassSuite::kStressFrames);
            auto* weighted = reg.Get<ZHLN::Components::MorphTargetComponent>(instances[0].mesh);
            if (!ZHLN::Test::ExpectTrue(weighted != nullptr)) {
                return std::unexpected(MorphPosePassTestError::MorphComponentMissing);
            }
            ZHLN::Test::ExpectGt(weighted->weights[0], 0.9f);

            const auto after = ZHLN::Test::Headless::Capture(*engine, "morph_pose_weights_one.ppm");
            if (!ZHLN::Test::ExpectTrue(after.Valid())) {
                return std::unexpected(MorphPosePassTestError::CaptureFailed);
            }

            uint64_t     changed = 0;
            const size_t pixels  = std::min(before.rgb.size(), after.rgb.size());
            for (size_t i = 0; i < pixels; ++i) {
                const int delta = static_cast<int>(before.rgb[i]) - static_cast<int>(after.rgb[i]);
                if (delta < -24 || delta > 24) {
                    changed++;
                }
            }

            const uint64_t total = pixels / 3;
            ZHLN::Println("    [INFO] Morph deformation: {} / {} pixels changed between weight 0 and weight 1.", changed, total);
            ZHLN::Test::ExpectGt(changed, total / 100); // 1% of the frame: the resize moves a sixth of it
            if (changed <= total / 100) {
                return std::unexpected(MorphPosePassTestError::MorphDeformationNotVisible);
            }

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunMorphPosePassSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<MorphPosePassSuite>();
}
