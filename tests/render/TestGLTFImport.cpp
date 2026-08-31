// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestGLTFImport.cpp
//
// Exercises the real importer -- ZHLN::CreativeWorksFactory::LoadModelPrefab-
// FromMemory, which reaches ZHLN::GLTF::BuildModelPrefab -- and checks the
// ModelPrefab it produces against the source document.
//
// The reference side is cgltf reading the same bytes independently. That is
// deliberate: the assertions describe what the glTF says, and the importer has
// to agree with it. Re-deriving the prefab with the importer's own algorithm
// would only prove the algorithm equals itself.
//
// This suite lives in a GPU group because BuildModelPrefab uploads textures
// and geometry through RenderContext, so it needs a real (headless) device.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstdint>
#include <expected>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class GLTFImportError : uint8_t {
    AssetUnavailable[[= ZHLN::Description<"The base rig GLB could not be read from the source tree.">{}]] = 1,
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize the headless Engine the importer uploads through.">{}]],
    PrefabLoadFailed[[= ZHLN::Description<"CreativeWorksFactory returned no prefab for a valid in-memory GLB.">{}]],
    NodeGraphMismatch[[= ZHLN::Description<"Imported node names, parents or transforms disagree with the source document.">{}]],
    SkeletonMismatch[[= ZHLN::Description<"Imported skin joints, parents or inverse bind matrices disagree with the source document.">{}]],
    AnimationMismatch[[= ZHLN::Description<"Imported animation channels disagree with the source document.">{}]],
    PartMismatch[[= ZHLN::Description<"Imported mesh parts do not reference the nodes and skins that carry them.">{}]],
    PrefabCacheMismatch[[= ZHLN::Description<"Reloading the same virtual path did not return the cached prefab.">{}]],
};

namespace {

constexpr std::string_view kVirtualPath = "ProceduralAnimationBaseRig.glb";

[[nodiscard]] auto ReadAssetBytes() -> std::vector<uint8_t> {
    const std::string path = std::string(ZHLN_TEST_SOURCE_DIR) + "/resources/assets/ProceduralAnimationBaseRig.glb";
    std::ifstream     stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    // An unresolved Git LFS pointer is a small ASCII file, not a GLB.
    if (bytes.size() < 4 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "glTF") {
        return {};
    }
    return bytes;
}

[[nodiscard]] JPH::Mat44 ColumnMajor(const float (&values)[16]) noexcept {
    return JPH::Mat44(
        JPH::Vec4(values[0], values[1], values[2], values[3]), JPH::Vec4(values[4], values[5], values[6], values[7]),
        JPH::Vec4(values[8], values[9], values[10], values[11]), JPH::Vec4(values[12], values[13], values[14], values[15])
    );
}

[[nodiscard]] JPH::Mat44 SourceLocal(const cgltf_node& node) noexcept {
    float matrix[16] {};
    cgltf_node_transform_local(&node, matrix);
    return ColumnMajor(matrix);
}

[[nodiscard]] JPH::Mat44 SourceWorld(const cgltf_node& node) noexcept {
    float matrix[16] {};
    cgltf_node_transform_world(&node, matrix);
    return ColumnMajor(matrix);
}

/// Independent cgltf view of the same bytes, used as the reference the
/// imported prefab is compared against.
struct SourceDocument {
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data {nullptr, &cgltf_free};

    [[nodiscard]] bool Parse(std::span<const uint8_t> bytes) {
        cgltf_options options {};
        cgltf_data*   raw = nullptr;
        if (cgltf_parse(&options, bytes.data(), bytes.size(), &raw) != cgltf_result_success || raw == nullptr) {
            return false;
        }
        data.reset(raw);
        return cgltf_load_buffers(&options, data.get(), nullptr) == cgltf_result_success && cgltf_validate(data.get()) == cgltf_result_success;
    }
};

} // namespace

struct GLTFImportTestSuite {
    GLTFImportTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~GLTFImportTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        /**
         * Loads the base rig through the shipping importer and checks the node
         * graph it produced: one prefab node per glTF node, parents resolved to
         * indices, mesh flags, and a local-transform convention that composes
         * back into cgltf's world transforms.
         */
        std::expected<void, ZHLN::Error> importer_flattens_node_graph_from_source_document() {
            const std::vector<uint8_t> bytes = ReadAssetBytes();
            if (bytes.empty()) {
                ZHLN::Println("    [SKIP] ProceduralAnimationBaseRig.glb is missing or an unresolved Git LFS pointer.");
                return {};
            }

            SourceDocument source;
            if (!source.Parse(bytes)) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }
            const cgltf_data& document = *source.data;

            const auto engine = ZHLN::Test::Headless::CreateEngine("Headless glTF Import");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            const ZHLN::ModelPrefab* prefab = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, bytes, kVirtualPath);
            if (prefab == nullptr) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            if (std::string_view(prefab->virtualPath) != kVirtualPath || prefab->nodes.size() != document.nodes_count || prefab->nodes.empty()) {
                return std::unexpected(GLTFImportError::NodeGraphMismatch);
            }

            size_t roots     = 0;
            size_t meshNodes = 0;
            for (size_t index = 0; index < document.nodes_count; ++index) {
                const cgltf_node&      sourceNode = document.nodes[index];
                const ZHLN::ModelNode& imported   = prefab->nodes[index];

                if (sourceNode.name != nullptr && std::string_view(imported.name) != std::string_view(sourceNode.name)) {
                    return std::unexpected(GLTFImportError::NodeGraphMismatch);
                }
                if (imported.hasMesh != (sourceNode.mesh != nullptr)) {
                    return std::unexpected(GLTFImportError::NodeGraphMismatch);
                }
                meshNodes += imported.hasMesh ? 1u : 0u;

                const int32_t expectedParent = sourceNode.parent != nullptr ? static_cast<int32_t>(sourceNode.parent - document.nodes) : -1;
                if (imported.parentIndex != expectedParent || imported.parentIndex == static_cast<int32_t>(index) ||
                    imported.parentIndex >= static_cast<int32_t>(prefab->nodes.size())) {
                    return std::unexpected(GLTFImportError::NodeGraphMismatch);
                }
                roots += imported.parentIndex < 0 ? 1u : 0u;

                // The source states children; the prefab states parents. Both
                // directions of the same edge must agree.
                for (size_t child = 0; child < sourceNode.children_count; ++child) {
                    const size_t childIndex = static_cast<size_t>(sourceNode.children[child] - document.nodes);
                    if (childIndex >= prefab->nodes.size() || prefab->nodes[childIndex].parentIndex != static_cast<int32_t>(index)) {
                        return std::unexpected(GLTFImportError::NodeGraphMismatch);
                    }
                }

                // Every chain terminates at a root: no cycles, no dangling parent.
                int32_t cursor = imported.parentIndex;
                size_t  depth  = 0;
                while (cursor >= 0 && depth <= prefab->nodes.size()) {
                    cursor = prefab->nodes[static_cast<size_t>(cursor)].parentIndex;
                    ++depth;
                }
                if (cursor != -1) {
                    return std::unexpected(GLTFImportError::NodeGraphMismatch);
                }

                if (!imported.localTransform.IsClose(SourceLocal(sourceNode), 0.0001f)) {
                    return std::unexpected(GLTFImportError::NodeGraphMismatch);
                }
            }
            if (roots == 0 || meshNodes == 0) {
                return std::unexpected(GLTFImportError::NodeGraphMismatch);
            }

            // Walking the flattened parents must reproduce the source world
            // transforms. This is what catches a transposed matrix load or a
            // reversed parent/child multiply that per-node comparisons miss.
            for (size_t index = 0; index < prefab->nodes.size(); ++index) {
                JPH::Mat44 world  = prefab->nodes[index].localTransform;
                int32_t    cursor = prefab->nodes[index].parentIndex;
                for (size_t depth = 0; cursor >= 0 && depth < prefab->nodes.size(); ++depth) {
                    world  = prefab->nodes[static_cast<size_t>(cursor)].localTransform * world;
                    cursor = prefab->nodes[static_cast<size_t>(cursor)].parentIndex;
                }
                if (!world.IsClose(SourceWorld(document.nodes[index]), 0.0001f)) {
                    return std::unexpected(GLTFImportError::NodeGraphMismatch);
                }
            }

            // Mesh parts must point back at the nodes and skins that carry them.
            if (prefab->parts.empty()) {
                return std::unexpected(GLTFImportError::PartMismatch);
            }
            for (const ZHLN::ModelPart& part: prefab->parts) {
                if (part.nodeIndex < 0 || static_cast<size_t>(part.nodeIndex) >= prefab->nodes.size()) {
                    return std::unexpected(GLTFImportError::PartMismatch);
                }
                const cgltf_node& owner = document.nodes[static_cast<size_t>(part.nodeIndex)];
                if (owner.mesh == nullptr || !prefab->nodes[static_cast<size_t>(part.nodeIndex)].hasMesh) {
                    return std::unexpected(GLTFImportError::PartMismatch);
                }
                const int32_t expectedSkeleton = owner.skin != nullptr ? static_cast<int32_t>(owner.skin - document.skins) : -1;
                // isSkinned needs both a skin on the node and JOINTS_0 on the
                // geometry; a skin alone does not make a part skinned.
                bool sourceHasJointWeights = false;
                for (size_t primitive = 0; primitive < owner.mesh->primitives_count; ++primitive) {
                    for (size_t attribute = 0; attribute < owner.mesh->primitives[primitive].attributes_count; ++attribute) {
                        sourceHasJointWeights = sourceHasJointWeights || owner.mesh->primitives[primitive].attributes[attribute].type == cgltf_attribute_type_joints;
                    }
                }
                if (part.skeletonIndex != expectedSkeleton || part.isSkinned != (owner.skin != nullptr && sourceHasJointWeights)) {
                    return std::unexpected(GLTFImportError::PartMismatch);
                }
                if (!(part.boundingRadius > 0.0f) || part.localMin[0] > part.localMax[0] || part.localMin[1] > part.localMax[1] ||
                    part.localMin[2] > part.localMax[2]) {
                    return std::unexpected(GLTFImportError::PartMismatch);
                }
            }

            // The loader is cache-backed: the same virtual path must not import twice.
            if (ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, bytes, kVirtualPath) != prefab) {
                return std::unexpected(GLTFImportError::PrefabCacheMismatch);
            }
            return {};
        }

        /**
         * Checks the two consumers the animation runtime actually reads: skin
         * joints (node indices, intra-skin parents, inverse binds) and animation
         * channels (target nodes, key counts, component widths, duration).
         */
        std::expected<void, ZHLN::Error> importer_builds_skins_and_animation_channels() {
            const std::vector<uint8_t> bytes = ReadAssetBytes();
            if (bytes.empty()) {
                ZHLN::Println("    [SKIP] ProceduralAnimationBaseRig.glb is missing or an unresolved Git LFS pointer.");
                return {};
            }

            SourceDocument source;
            if (!source.Parse(bytes)) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }
            const cgltf_data& document = *source.data;
            if (document.skins_count == 0 || document.animations_count == 0) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }

            const auto engine = ZHLN::Test::Headless::CreateEngine("Headless glTF Skin Import");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            // Each test builds its own engine, so this loads into a fresh prefab
            // cache; the distinct path just keeps the two imports easy to tell
            // apart in the engine log.
            const ZHLN::ModelPrefab* prefab = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, bytes, "ProceduralAnimationBaseRig_Skins.glb");
            if (prefab == nullptr) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }

            if (prefab->skeletons.size() != document.skins_count) {
                return std::unexpected(GLTFImportError::SkeletonMismatch);
            }
            for (size_t skinIndex = 0; skinIndex < document.skins_count; ++skinIndex) {
                const cgltf_skin&     skin     = document.skins[skinIndex];
                const ZHLN::Skeleton& skeleton = prefab->skeletons[skinIndex];
                if (skeleton.joints.size() != skin.joints_count || skeleton.joints.empty()) {
                    return std::unexpected(GLTFImportError::SkeletonMismatch);
                }
                if (skin.name != nullptr && std::string_view(skeleton.name) != std::string_view(skin.name)) {
                    return std::unexpected(GLTFImportError::SkeletonMismatch);
                }

                for (size_t jointIndex = 0; jointIndex < skin.joints_count; ++jointIndex) {
                    const ZHLN::Joint& joint      = skeleton.joints[jointIndex];
                    const cgltf_node*  sourceNode = skin.joints[jointIndex];
                    if (joint.nodeIndex < 0 || static_cast<size_t>(joint.nodeIndex) != static_cast<size_t>(sourceNode - document.nodes)) {
                        return std::unexpected(GLTFImportError::SkeletonMismatch);
                    }
                    if (sourceNode->name != nullptr && std::string_view(joint.name) != std::string_view(sourceNode->name)) {
                        return std::unexpected(GLTFImportError::SkeletonMismatch);
                    }

                    // parentIndex indexes the joints array, not the node array,
                    // and is -1 when the parent node is outside this skin.
                    int32_t expectedParentJoint = -1;
                    for (size_t candidate = 0; candidate < skin.joints_count; ++candidate) {
                        if (skin.joints[candidate] == sourceNode->parent) {
                            expectedParentJoint = static_cast<int32_t>(candidate);
                            break;
                        }
                    }
                    if (joint.parentIndex != expectedParentJoint || joint.parentIndex == static_cast<int32_t>(jointIndex)) {
                        return std::unexpected(GLTFImportError::SkeletonMismatch);
                    }

                    // The inverse bind matrix must undo the bind-pose world transform.
                    if (skin.inverse_bind_matrices != nullptr && !(SourceWorld(*sourceNode) * joint.inverseBindMatrix).IsClose(JPH::Mat44::sIdentity(), 0.0001f)) {
                        return std::unexpected(GLTFImportError::SkeletonMismatch);
                    }
                }
            }

            if (prefab->animations.size() != document.animations_count) {
                return std::unexpected(GLTFImportError::AnimationMismatch);
            }
            float longestClip = 0.0f;
            for (size_t clipIndex = 0; clipIndex < document.animations_count; ++clipIndex) {
                const cgltf_animation&     sourceClip = document.animations[clipIndex];
                const ZHLN::AnimationClip& clip       = prefab->animations[clipIndex];
                if (sourceClip.name != nullptr && std::string_view(clip.name) != std::string_view(sourceClip.name)) {
                    return std::unexpected(GLTFImportError::AnimationMismatch);
                }
                if (clip.channels.size() != sourceClip.channels_count || clip.channels.empty()) {
                    return std::unexpected(GLTFImportError::AnimationMismatch);
                }

                float latestKey = 0.0f;
                for (size_t channelIndex = 0; channelIndex < sourceClip.channels_count; ++channelIndex) {
                    const cgltf_animation_channel& sourceChannel = sourceClip.channels[channelIndex];
                    const ZHLN::AnimationChannel&  channel       = clip.channels[channelIndex];
                    if (sourceChannel.target_node == nullptr || sourceChannel.sampler == nullptr) {
                        continue;
                    }
                    if (channel.targetNodeIndex < 0 ||
                        static_cast<size_t>(channel.targetNodeIndex) != static_cast<size_t>(sourceChannel.target_node - document.nodes)) {
                        return std::unexpected(GLTFImportError::AnimationMismatch);
                    }

                    const bool expectedRotation = sourceChannel.target_path == cgltf_animation_path_type_rotation;
                    if (expectedRotation != (channel.path == ZHLN::AnimationPathType::Rotation)) {
                        return std::unexpected(GLTFImportError::AnimationMismatch);
                    }

                    // Rotations are stored as four components per key, everything
                    // else as three; a mismatch here silently shears the pose.
                    const size_t components = expectedRotation ? 4u : 3u;
                    if (channel.keyTimes.size() != sourceChannel.sampler->input->count || channel.keyTimes.empty() ||
                        channel.keyValues.size() != sourceChannel.sampler->output->count * components || !std::ranges::is_sorted(channel.keyTimes) ||
                        channel.keyTimes.front() < 0.0f) {
                        return std::unexpected(GLTFImportError::AnimationMismatch);
                    }

                    float sourceTime = 0.0f;
                    cgltf_accessor_read_float(sourceChannel.sampler->input, sourceChannel.sampler->input->count - 1, &sourceTime, 1);
                    if (std::abs(channel.keyTimes.back() - sourceTime) > 0.0001f) {
                        return std::unexpected(GLTFImportError::AnimationMismatch);
                    }
                    latestKey = std::max(latestKey, channel.keyTimes.back());
                }

                // A single-key pose clip legitimately has duration 0, so the
                // invariant is "duration is the largest key time".
                if (std::abs(clip.duration - latestKey) > 0.0001f) {
                    return std::unexpected(GLTFImportError::AnimationMismatch);
                }
                longestClip = std::max(longestClip, clip.duration);
            }
            if (longestClip <= 0.0f) {
                return std::unexpected(GLTFImportError::AnimationMismatch);
            }

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunGLTFImportSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GLTFImportTestSuite>();
}
