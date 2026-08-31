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
#include <Zahlen/JSON.hpp>
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
#include <cstring>
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
    ExtensionMismatch[[= ZHLN::Description<"A Khronos glTF extension was not applied the way the importer documents it.">{}]],
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

// ---------------------------------------------------------------------------
// Typed glTF fixture documents.
//
// These are declarations, not text: ZHLN::Reflect::SerializeJSON turns each
// struct into the JSON chunk, so field names are the glTF keys and the
// compiler checks every value's type. Nothing here hand-writes a brace, a
// comma or an escape.
//
// glTF is optional-by-omission, and SerializeJSON emits an empty std::optional
// as null rather than dropping the key -- which cgltf would then read as
// "mesh": 0 rather than "no mesh". So each shape a fixture needs is its own
// type (a node with a mesh, a node with only a light) and the document is
// templated over them. Empty structs are not an option either: the serializer
// static_asserts on FieldCount<T>() == 0.
// ---------------------------------------------------------------------------

struct GltfAsset {
    std::string_view version = "2.0";
};

struct GltfScene {
    std::vector<int32_t> nodes;
};

struct GltfPrimitiveAttributes {
    int32_t POSITION = 0;
};

struct GltfPrimitive {
    GltfPrimitiveAttributes attributes;
    int32_t                 indices  = 1;
    int32_t                 material = 0;
};

struct GltfMesh {
    std::string_view           name;
    std::vector<GltfPrimitive> primitives;
};

struct GltfPbrMetallicRoughness {
    std::array<float, 4> baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    float                metallicFactor  = 0.0f;
    float                roughnessFactor = 1.0f;
};

struct KhrMaterialsEmissiveStrength {
    float emissiveStrength = 1.0f;
};

struct GltfMaterialExtensions {
    KhrMaterialsEmissiveStrength KHR_materials_emissive_strength;
};

struct GltfPlainMaterial {
    std::string_view         name;
    GltfPbrMetallicRoughness pbrMetallicRoughness;
    std::array<float, 3>     emissiveFactor;
};

struct GltfEmissiveStrengthMaterial {
    std::string_view         name;
    GltfPbrMetallicRoughness pbrMetallicRoughness;
    std::array<float, 3>     emissiveFactor;
    GltfMaterialExtensions   extensions;
};

// min/max are carried on both accessors so one type covers the position and
// the index accessor; the spec allows them on either.
struct GltfAccessor {
    int32_t            bufferView    = 0;
    int32_t            componentType = 5126;
    int32_t            count         = 0;
    std::string_view   type          = "VEC3";
    std::vector<float> min;
    std::vector<float> max;
};

struct GltfBufferView {
    int32_t buffer     = 0;
    int32_t byteOffset = 0;
    int32_t byteLength = 0;
    int32_t target     = 34962;
};

struct GltfBuffer {
    int32_t byteLength = 0;
};

struct KhrLightsPunctualRef {
    int32_t light = 0;
};

struct GltfNodeExtensions {
    KhrLightsPunctualRef KHR_lights_punctual;
};

struct GltfMeshNode {
    std::string_view     name;
    int32_t              mesh = 0;
    std::array<float, 3> translation {0.0f, 0.0f, 0.0f};
};

struct GltfMeshNodeWithLight {
    std::string_view     name;
    int32_t              mesh = 0;
    std::array<float, 3> translation {0.0f, 0.0f, 0.0f};
    GltfNodeExtensions   extensions;
};

struct GltfLightNode {
    std::string_view     name;
    std::array<float, 3> translation {0.0f, 0.0f, 0.0f};
    GltfNodeExtensions   extensions;
};

struct KhrPunctualLight {
    std::string_view     type = "point";
    std::string_view     name;
    std::array<float, 3> color {1.0f, 1.0f, 1.0f};
    float                intensity = 1.0f;
};

struct KhrLightsPunctual {
    std::vector<KhrPunctualLight> lights;
};

struct GltfRootExtensions {
    KhrLightsPunctual KHR_lights_punctual;
};

template <typename NodeT, typename MaterialT>
struct GltfDocument {
    GltfAsset                     asset;
    std::vector<std::string_view> extensionsUsed;
    int32_t                       scene = 0;
    std::vector<GltfScene>        scenes;
    std::vector<NodeT>            nodes;
    std::vector<GltfMesh>         meshes;
    std::vector<MaterialT>        materials;
    std::vector<GltfAccessor>     accessors;
    std::vector<GltfBufferView>   bufferViews;
    std::vector<GltfBuffer>       buffers;
};

/// Same document with a root `extensions` object. A separate type rather than
/// an optional member, for the omission reason above.
template <typename NodeT, typename MaterialT>
struct GltfLightDocument {
    GltfAsset                     asset;
    std::vector<std::string_view> extensionsUsed;
    GltfRootExtensions            extensions;
    int32_t                       scene = 0;
    std::vector<GltfScene>        scenes;
    std::vector<NodeT>            nodes;
    std::vector<GltfMesh>         meshes;
    std::vector<MaterialT>        materials;
    std::vector<GltfAccessor>     accessors;
    std::vector<GltfBufferView>   bufferViews;
    std::vector<GltfBuffer>       buffers;
};

/// Assembles a GLB container around a serialized JSON chunk and a binary chunk.
///
/// Synthesizing the input is not the same as reimplementing the importer: this
/// only produces bytes a conformant loader must accept, so the extension
/// behaviour under test stays the importer's own.
[[nodiscard]] auto MakeGlb(const std::string& json, std::span<const uint8_t> bin) -> std::vector<uint8_t> {
    std::string paddedJson = json;
    while (paddedJson.size() % 4 != 0) {
        paddedJson.push_back(' ');
    }
    std::vector<uint8_t> paddedBin(bin.begin(), bin.end());
    while (paddedBin.size() % 4 != 0) {
        paddedBin.push_back(0);
    }

    std::vector<uint8_t> glb;
    auto                 append32 = [&glb](uint32_t value) {
        for (uint32_t byte = 0; byte < 4; ++byte) {
            glb.push_back(static_cast<uint8_t>((value >> (8u * byte)) & 0xFFu));
        }
    };
    auto appendBytes = [&glb](const auto& source) {
        for (const auto element: source) {
            glb.push_back(static_cast<uint8_t>(element));
        }
    };

    const size_t binChunkSize = paddedBin.empty() ? 0u : 8u + paddedBin.size();
    append32(0x46546C67u); // "glTF"
    append32(2u);
    append32(static_cast<uint32_t>(12u + 8u + paddedJson.size() + binChunkSize));
    append32(static_cast<uint32_t>(paddedJson.size()));
    append32(0x4E4F534Au); // "JSON"
    appendBytes(paddedJson);
    if (!paddedBin.empty()) {
        append32(static_cast<uint32_t>(paddedBin.size()));
        append32(0x004E4942u); // "BIN\0"
        appendBytes(paddedBin);
    }
    return glb;
}

// One triangle: 3 VEC3 positions then 3 uint32 indices.
constexpr float    kTrianglePositions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
constexpr uint32_t kTriangleIndices[3]   = {0u, 1u, 2u};
constexpr int32_t  kPositionBytes        = static_cast<int32_t>(sizeof(kTrianglePositions));
constexpr int32_t  kIndexBytes           = static_cast<int32_t>(sizeof(kTriangleIndices));

// emissiveFactor is deliberately below 1 in every channel so a 4x strength
// stays representable and cannot be confused with a clamp to white.
constexpr std::array<float, 3> kAuthoredEmissive {0.25f, 0.5f, 0.125f};
constexpr float                kEmissiveStrength = 4.0f;

[[nodiscard]] auto TriangleBin() -> std::vector<uint8_t> {
    std::vector<uint8_t> bin(static_cast<size_t>(kPositionBytes + kIndexBytes));
    std::memcpy(bin.data(), kTrianglePositions, sizeof(kTrianglePositions));
    std::memcpy(bin.data() + sizeof(kTrianglePositions), kTriangleIndices, sizeof(kTriangleIndices));
    return bin;
}

[[nodiscard]] auto TriangleMeshes() -> std::vector<GltfMesh> {
    return {GltfMesh {.name = "Tri", .primitives = {GltfPrimitive {}}}};
}

[[nodiscard]] auto TriangleAccessors() -> std::vector<GltfAccessor> {
    return {
        GltfAccessor {.bufferView = 0, .componentType = 5126, .count = 3, .type = "VEC3", .min = {0.0f, 0.0f, 0.0f}, .max = {1.0f, 1.0f, 0.0f}},
        GltfAccessor {.bufferView = 1, .componentType = 5125, .count = 3, .type = "SCALAR", .min = {0.0f}, .max = {2.0f}},
    };
}

[[nodiscard]] auto TriangleBufferViews() -> std::vector<GltfBufferView> {
    return {
        GltfBufferView {.buffer = 0, .byteOffset = 0, .byteLength = kPositionBytes, .target = 34962},
        GltfBufferView {.buffer = 0, .byteOffset = kPositionBytes, .byteLength = kIndexBytes, .target = 34963},
    };
}

[[nodiscard]] auto TriangleBuffers() -> std::vector<GltfBuffer> {
    return {GltfBuffer {.byteLength = kPositionBytes + kIndexBytes}};
}

/// Triangle whose material carries KHR_materials_emissive_strength.
[[nodiscard]] auto MakeEmissiveStrengthFixture() -> std::vector<uint8_t> {
    const GltfDocument<GltfMeshNode, GltfEmissiveStrengthMaterial> document {
        .extensionsUsed = {"KHR_materials_emissive_strength"},
        .scenes         = {GltfScene {.nodes = {0}}},
        .nodes          = {GltfMeshNode {.name = "EmissiveTriangle"}},
        .meshes         = TriangleMeshes(),
        .materials      = {GltfEmissiveStrengthMaterial {
                 .name              = "Emissive",
                 .emissiveFactor    = kAuthoredEmissive,
                 .extensions        = {.KHR_materials_emissive_strength = {.emissiveStrength = kEmissiveStrength}},
        }},
        .accessors      = TriangleAccessors(),
        .bufferViews    = TriangleBufferViews(),
        .buffers        = TriangleBuffers(),
    };
    return MakeGlb(ZHLN::Reflect::SerializeJSON(document), TriangleBin());
}

/// The same triangle and the same emissiveFactor, extension absent.
[[nodiscard]] auto MakePlainEmissiveFixture() -> std::vector<uint8_t> {
    const GltfDocument<GltfMeshNode, GltfPlainMaterial> document {
        .scenes      = {GltfScene {.nodes = {0}}},
        .nodes       = {GltfMeshNode {.name = "EmissiveTriangle"}},
        .meshes      = TriangleMeshes(),
        .materials   = {GltfPlainMaterial {.name = "Emissive", .emissiveFactor = kAuthoredEmissive}},
        .accessors   = TriangleAccessors(),
        .bufferViews = TriangleBufferViews(),
        .buffers     = TriangleBuffers(),
    };
    return MakeGlb(ZHLN::Reflect::SerializeJSON(document), TriangleBin());
}

/// A mesh node that also carries a punctual light, alongside the emissive
/// extension: two extensions on one document, one of them unread.
[[nodiscard]] auto MakeLitMeshFixture() -> std::vector<uint8_t> {
    const GltfLightDocument<GltfMeshNodeWithLight, GltfEmissiveStrengthMaterial> document {
        .extensionsUsed = {"KHR_materials_emissive_strength", "KHR_lights_punctual"},
        .extensions     = {.KHR_lights_punctual = {.lights = {KhrPunctualLight {.name = "TestPoint", .color = {1.0f, 0.5f, 0.25f}, .intensity = 42.0f}}}},
        .scenes         = {GltfScene {.nodes = {0}}},
        .nodes          = {GltfMeshNodeWithLight {.name = "LitTriangle", .translation = {1.0f, 2.0f, 3.0f}}},
        .meshes         = TriangleMeshes(),
        .materials      = {GltfEmissiveStrengthMaterial {
                 .name              = "Emissive",
                 .emissiveFactor    = kAuthoredEmissive,
                 .extensions        = {.KHR_materials_emissive_strength = {.emissiveStrength = kEmissiveStrength}},
        }},
        .accessors      = TriangleAccessors(),
        .bufferViews    = TriangleBufferViews(),
        .buffers        = TriangleBuffers(),
    };
    return MakeGlb(ZHLN::Reflect::SerializeJSON(document), TriangleBin());
}

/// Geometry-free document carrying only a punctual light -- the shape zcook
/// emits for a cooked scene light (src/zcook/GLB.cpp:1077).
[[nodiscard]] auto MakeLightOnlyFixture() -> std::vector<uint8_t> {
    const GltfLightDocument<GltfLightNode, GltfPlainMaterial> document {
        .extensionsUsed = {"KHR_lights_punctual"},
        .extensions     = {.KHR_lights_punctual = {.lights = {KhrPunctualLight {.name = "TestPoint", .color = {1.0f, 0.5f, 0.25f}, .intensity = 42.0f}}}},
        .scenes         = {GltfScene {.nodes = {0}}},
        .nodes          = {GltfLightNode {.name = "PunctualLight", .translation = {1.0f, 2.0f, 3.0f}}},
    };
    return MakeGlb(ZHLN::Reflect::SerializeJSON(document), {});
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

        /**
         * Khronos extensions. The importer consumes exactly one today --
         * KHR_materials_emissive_strength (GLTFImporter.cpp:291) -- so that one
         * is asserted properly, and the rest are pinned as the behaviour they
         * actually have rather than the behaviour a reader might assume.
         *
         * KHR_lights_punctual in particular is exported by zcook
         * (src/zcook/GLB.cpp:1077) but never read back: ModelPrefab has no
         * light representation at all, so a cooked light survives the round
         * trip only through the cooker's own manifest. What is enforced here is
         * that such a file still imports cleanly instead of failing or
         * corrupting the node graph.
         */
        std::expected<void, ZHLN::Error> importer_applies_supported_khronos_extensions() {
            const auto engine = ZHLN::Test::Headless::CreateEngine("Headless glTF Extensions");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            // 1. KHR_materials_emissive_strength scales the authored emissive factor.
            const std::vector<uint8_t> strengthBytes = MakeEmissiveStrengthFixture();
            const ZHLN::ModelPrefab*   withStrength  = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, strengthBytes, "ext_emissive_strength.glb");
            if (withStrength == nullptr || withStrength->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                const float expected = kAuthoredEmissive[channel] * kEmissiveStrength;
                if (std::abs(withStrength->parts[0].defaultMaterial.emissiveFactor[channel] - expected) > 0.0001f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // 2. The same material without the extension keeps the authored value,
            //    so the scale above is attributable to the extension and not to a
            //    constant the importer applies to every emissive material.
            const std::vector<uint8_t> plainBytes = MakePlainEmissiveFixture();
            const ZHLN::ModelPrefab*   plain      = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, plainBytes, "ext_emissive_plain.glb");
            if (plain == nullptr || plain->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(plain->parts[0].defaultMaterial.emissiveFactor[channel] - kAuthoredEmissive[channel]) > 0.0001f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // 3. An unread extension on a mesh-bearing node must not disturb the
            //    node it sits on, the part it produces, or the extension that is
            //    read from the same document.
            const std::vector<uint8_t> litBytes = MakeLitMeshFixture();
            const ZHLN::ModelPrefab*   litMesh  = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, litBytes, "ext_lit_mesh.glb");
            if (litMesh == nullptr || litMesh->nodes.size() != 1 || litMesh->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            const ZHLN::ModelNode& litNode = litMesh->nodes[0];
            if (std::string_view(litNode.name) != "LitTriangle" || !litNode.hasMesh || litNode.parentIndex != -1 ||
                !litNode.localTransform.GetTranslation().IsClose(JPH::Vec3(1.0f, 2.0f, 3.0f), 0.0001f) || litMesh->parts[0].nodeIndex != 0) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                const float expected = kAuthoredEmissive[channel] * kEmissiveStrength;
                if (std::abs(litMesh->parts[0].defaultMaterial.emissiveFactor[channel] - expected) > 0.0001f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // 4. A geometry-free light document -- what zcook emits for a scene
            //    light -- imports as a bare transform node. The light itself is
            //    dropped: ModelPrefab has nowhere to put it. Pinning that keeps
            //    the gap visible instead of implied.
            const std::vector<uint8_t> lightOnlyBytes = MakeLightOnlyFixture();
            const ZHLN::ModelPrefab*   lightOnly      = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(*engine, lightOnlyBytes, "ext_light_only.glb");
            if (lightOnly == nullptr || lightOnly->nodes.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            const ZHLN::ModelNode& lightNode = lightOnly->nodes[0];
            if (std::string_view(lightNode.name) != "PunctualLight" || lightNode.hasMesh || lightNode.parentIndex != -1 ||
                !lightNode.localTransform.GetTranslation().IsClose(JPH::Vec3(1.0f, 2.0f, 3.0f), 0.0001f)) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            if (!lightOnly->parts.empty() || !lightOnly->skeletons.empty() || !lightOnly->animations.empty()) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
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
