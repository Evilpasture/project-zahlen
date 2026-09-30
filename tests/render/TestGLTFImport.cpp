// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestGLTFImport.cpp
//
// Exercises the real importer -- ZHLN::GLTF::LoadGLBPrefabFromMemory in
// extras/glTF -- and checks the ModelPrefab it produces against the source
// document. Core only ever sees the resulting ModelPrefab, which the importer
// caches under its virtual path; nothing here goes through core's loader.
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
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Vertex.hpp>
#include <algorithm>
#include <array>
#include <cgltf.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <glTF/GLTFImporter.hpp>
#include <glTF/TangentGenerator.hpp>
#include <ios>
#include <iterator>
#include <json/JSONSchema.hpp>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class GLTFImportError : uint8_t {
    AssetUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The base rig GLB could not be read from the source tree.">{}) = 1,
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize the headless Engine the importer uploads through.">{}),
    PrefabLoadFailed ZHLN_ANNOTATION(ZHLN::Description<"PrefabFactory returned no prefab for a valid in-memory GLB.">{}),
    NodeGraphMismatch ZHLN_ANNOTATION(ZHLN::Description<"Imported node names, parents or transforms disagree with the source document.">{}),
    SkeletonMismatch ZHLN_ANNOTATION(ZHLN::Description<"Imported skin joints, parents or inverse bind matrices disagree with the source document.">{}),
    AnimationMismatch ZHLN_ANNOTATION(ZHLN::Description<"Imported animation channels disagree with the source document.">{}),
    PartMismatch ZHLN_ANNOTATION(ZHLN::Description<"Imported mesh parts do not reference the nodes and skins that carry them.">{}),
    PrefabCacheMismatch ZHLN_ANNOTATION(ZHLN::Description<"Reloading the same virtual path did not return the cached prefab.">{}),
    ExtensionMismatch ZHLN_ANNOTATION(ZHLN::Description<"A Khronos glTF extension was not applied the way the importer documents it.">{}),
    EmissiveLightMismatch ZHLN_ANNOTATION(ZHLN::Description<"Emissive virtual point lights did not follow the prefab they were spawned for.">{}),
    TangentFrameMismatch ZHLN_ANNOTATION(ZHLN::Description<"Missing glTF tangents were not generated from the UV orientation and handedness.">{}),
    NegativeScaleMismatch ZHLN_ANNOTATION(ZHLN::Description<"NegativeScaleTest lost its authored single-/double-sided flags or shared mesh instances.">{}),
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

// The pinned Khronos fixture is tiny (3,992 bytes) and checked in with its
// attribution under tests/render/assets/, so this test must not silently skip.
[[nodiscard]] auto ReadUnlitAssetBytes() -> std::vector<uint8_t> {
    const std::string path = std::string(ZHLN_TEST_SOURCE_DIR) + "/tests/render/assets/UnlitTest.glb";
    std::ifstream     stream(path, std::ios::binary);
    if (!stream) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

[[nodiscard]] auto ReadNegativeScaleAssetBytes() -> std::vector<uint8_t> {
    const std::string path = std::string(ZHLN_TEST_SOURCE_DIR) + "/tests/render/assets/NegativeScaleTest.glb";
    std::ifstream     stream(path, std::ios::binary);
    if (!stream) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
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
// These are declarations, not text: ZHLN::ReflectJSON::SerializeJSON turns each
// struct into the JSON chunk, so field names are the glTF keys and the
// compiler checks every value's type. Nothing here hand-writes a brace, a
// comma or an escape.
//
// glTF is optional-by-omission, and SerializeJSON emits an empty std::optional
// as null rather than dropping the key -- which cgltf would then read as
// "mesh": 0 rather than "no mesh". So most shapes are separate types, and the
// document is templated over them. The anisotropy fixture opts into omitEmpty
// for one texture that has no sampler. Empty structs are not an option either:
// the serializer static_asserts on FieldCount<T>() == 0.
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

// The anisotropy fixture supplies a real tangent space (POSITION, NORMAL,
// TANGENT and TEXCOORD_0), as required by KHR_materials_anisotropy.
struct GltfAnisotropyAttributes {
    int32_t POSITION   = 0;
    int32_t NORMAL     = 1;
    int32_t TANGENT    = 2;
    int32_t TEXCOORD_0 = 3;
};

struct GltfAnisotropyPrimitive {
    GltfAnisotropyAttributes attributes;
    int32_t                  indices  = 4;
    int32_t                  material = 0;
};

struct GltfAnisotropyMesh {
    std::string_view                     name;
    std::vector<GltfAnisotropyPrimitive> primitives;
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

// Spec/gloss-only material: like the Khronos bottle, it supplies no core
// metallic-roughness fallback. Its tinted diffuseFactor must NOT be applied as
// baseColorFactor when KHR_materials_pbrSpecularGlossiness is unsupported.
struct KhrSpecGloss {
    std::array<float, 4> diffuseFactor {0.2f, 0.4f, 0.8f, 1.0f};
};
struct GltfSpecGlossExtensions {
    KhrSpecGloss KHR_materials_pbrSpecularGlossiness;
};
struct GltfSpecGlossMaterial {
    std::string_view name = "SpecGlossOnly";
    GltfSpecGlossExtensions extensions;
};

struct GltfAnisotropyTextureInfo {
    int32_t index = 0;
};

struct KhrMaterialsAnisotropy {
    float                     anisotropyStrength = 0.75f;
    float                     anisotropyRotation = 1.5707963f;
    GltfAnisotropyTextureInfo anisotropyTexture;
};

struct GltfAnisotropyExtensions {
    KhrMaterialsAnisotropy KHR_materials_anisotropy;
};

struct GltfAnisotropyPbr {
    std::array<float, 4>     baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    float                    metallicFactor  = 0.0f;
    float                    roughnessFactor = 1.0f;
    GltfAnisotropyTextureInfo baseColorTexture {.index = 1};
    GltfAnisotropyTextureInfo metallicRoughnessTexture {.index = 2};
};

struct GltfAnisotropyMaterial {
    std::string_view           name;
    GltfAnisotropyPbr          pbrMetallicRoughness;
    std::array<float, 3>       emissiveFactor {0.0f, 1.0f, 0.0f};
    GltfAnisotropyTextureInfo emissiveTexture;
    GltfAnisotropyExtensions  extensions;
};

struct GltfAnisotropyImage {
    int32_t          bufferView = 5;
    std::string_view mimeType   = "image/png";
};

struct GltfAnisotropyTexture {
    std::optional<int32_t> sampler;
    int32_t                source = 0;
};

struct GltfSamplerWrap {
    int32_t wrapS = 10497; // REPEAT
    int32_t wrapT = 10497;
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

// The image buffer view has no vertex/index target; the optional target may
// also be omitted for geometry views, so one shape covers all of them.
struct GltfAnisotropyBufferView {
    int32_t buffer     = 0;
    int32_t byteOffset = 0;
    int32_t byteLength = 0;
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

struct GltfSpecGlossDocument {
    GltfAsset                          asset;
    std::vector<std::string_view>      extensionsUsed {"KHR_materials_pbrSpecularGlossiness"};
    std::vector<std::string_view>      extensionsRequired {"KHR_materials_pbrSpecularGlossiness"};
    int32_t                            scene = 0;
    std::vector<GltfScene>             scenes;
    std::vector<GltfMeshNode>          nodes;
    std::vector<GltfMesh>              meshes;
    std::vector<GltfSpecGlossMaterial> materials;
    std::vector<GltfAccessor>          accessors;
    std::vector<GltfBufferView>        bufferViews;
    std::vector<GltfBuffer>            buffers;
};

[[nodiscard]] constexpr auto FindExtensionCapability(std::string_view name) -> const ZHLN::GLTF::Capability* {
    for (const auto& capability: ZHLN::GLTF::kCapabilities) {
        if (capability.kind == ZHLN::GLTF::CapabilityKind::Extension && capability.name == name) {
            return &capability;
        }
    }
    return nullptr;
}
static_assert(FindExtensionCapability("KHR_materials_unlit") != nullptr);
static_assert(FindExtensionCapability("KHR_texture_transform") != nullptr);
static_assert(FindExtensionCapability("KHR_materials_volume")->support == ZHLN::GLTF::CapabilitySupport::Partial);
static_assert(FindExtensionCapability("KHR_lights_punctual") == nullptr);
static_assert(FindExtensionCapability("KHR_materials_pbrSpecularGlossiness") == nullptr);

struct GltfAnisotropyDocument {
    GltfAsset                             asset;
    std::vector<std::string_view>         extensionsUsed;
    int32_t                               scene = 0;
    std::vector<GltfScene>                scenes;
    std::vector<GltfMeshNode>             nodes;
    std::vector<GltfAnisotropyMesh>       meshes;
    std::vector<GltfAnisotropyMaterial>   materials;
    std::vector<GltfAccessor>             accessors;
    std::vector<GltfAnisotropyBufferView> bufferViews;
    std::vector<GltfBuffer>               buffers;
    std::vector<GltfAnisotropyImage>      images;
    std::vector<GltfAnisotropyTexture>    textures;
    std::vector<GltfSamplerWrap>          samplers;
};

// TextureInfo owns the transform: several references to the SAME image below
// use different matrices, UV sets and color spaces.
struct GltfTextureTransform {
    std::array<float, 2> offset {0.0f, 0.0f};
    float rotation = 0.0f;
    std::array<float, 2> scale {1.0f, 1.0f};
    int32_t texCoord = 1;
};

struct GltfTextureInfoExtensions {
    GltfTextureTransform KHR_texture_transform;
};

struct GltfTransformedTextureInfo {
    int32_t index = 0;
    int32_t texCoord = 0;
    GltfTextureInfoExtensions extensions;
};

struct GltfOcclusionTextureInfo {
    int32_t index = 0;
    float strength = 0.4f;
    GltfTextureInfoExtensions extensions {.KHR_texture_transform = {
        .offset = {0.0f, 0.0f}, .rotation = 0.0f, .scale = {1.0f, 1.0f}, .texCoord = 0
    }};
};

struct GltfUv1TextureInfo {
    int32_t index = 0;
    int32_t texCoord = 1;
};

struct KhrMaterialsSheen {
    std::array<float, 3> sheenColorFactor {0.25f, 0.5f, 0.75f};
    GltfTransformedTextureInfo sheenColorTexture {.extensions = {.KHR_texture_transform = {
        .offset = {0.1f, 0.3f}, .rotation = 0.0f, .scale = {4.0f, -5.0f}, .texCoord = 0
    }}};
    float sheenRoughnessFactor = 0.35f;
    GltfUv1TextureInfo sheenRoughnessTexture;
};

struct GltfSheenExtensions {
    KhrMaterialsSheen KHR_materials_sheen;
};

struct GltfSheenPbr {
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.8f;
    GltfTransformedTextureInfo baseColorTexture {.extensions = {.KHR_texture_transform = {
        .offset = {0.2f, 0.4f}, .rotation = 1.5707963f, .scale = {2.0f, -3.0f}, .texCoord = 1
    }}};
    GltfTransformedTextureInfo metallicRoughnessTexture {.extensions = {.KHR_texture_transform = {
        .offset = {0.0f, 0.0f}, .rotation = 0.0f, .scale = {30.0f, -30.0f}, .texCoord = 0
    }}};
};

struct GltfSheenMaterial {
    std::string_view name = "Sheen/UV transform";
    GltfSheenPbr pbrMetallicRoughness;
    GltfTransformedTextureInfo normalTexture {.extensions = {.KHR_texture_transform = {
        .offset = {0.0f, 0.0f}, .rotation = 0.0f, .scale = {30.0f, -30.0f}, .texCoord = 0
    }}};
    GltfOcclusionTextureInfo occlusionTexture;
    GltfSheenExtensions extensions;
};

struct GltfSheenAttributes {
    int32_t POSITION = 0;
    int32_t TEXCOORD_0 = 1;
    int32_t TEXCOORD_1 = 2;
};

struct GltfSheenPrimitive {
    GltfSheenAttributes attributes;
    int32_t indices = 3;
    int32_t material = 0;
};

struct GltfSheenMesh {
    std::string_view name = "UV sets";
    std::vector<GltfSheenPrimitive> primitives {GltfSheenPrimitive {}};
};

struct GltfSheenImage {
    int32_t bufferView = 4;
    std::string_view mimeType = "image/png";
};

struct GltfSheenDocument {
    GltfAsset asset;
    std::vector<std::string_view> extensionsUsed {"KHR_texture_transform", "KHR_materials_sheen"};
    std::vector<std::string_view> extensionsRequired {"KHR_texture_transform"};
    int32_t scene = 0;
    std::vector<GltfScene> scenes {GltfScene {.nodes = {0}}};
    std::vector<GltfMeshNode> nodes {GltfMeshNode {.name = "SheenTriangle"}};
    std::vector<GltfSheenMesh> meshes {GltfSheenMesh {}};
    std::vector<GltfSheenMaterial> materials {GltfSheenMaterial {}};
    std::vector<GltfAccessor> accessors;
    std::vector<GltfAnisotropyBufferView> bufferViews;
    std::vector<GltfBuffer> buffers;
    std::vector<GltfSheenImage> images {GltfSheenImage {}};
    std::vector<GltfAnisotropyTexture> textures {GltfAnisotropyTexture {}};
};

struct GltfTransmissionPbr {
    std::array<float, 4> baseColorFactor {0.8f, 0.4f, 0.2f, 0.75f};
    float metallicFactor = 0.25f;
    float roughnessFactor = 0.35f;
    GltfAnisotropyTextureInfo baseColorTexture {.index = 0};
};

struct KhrMaterialsTransmission {
    float transmissionFactor = 0.625f;
    GltfTransformedTextureInfo transmissionTexture {.index = 1, .extensions = {.KHR_texture_transform = {
        .offset = {0.25f, -0.25f}, .rotation = 1.5707963f, .scale = {2.0f, 0.5f}, .texCoord = 1
    }}};
};

struct GltfTransmissionExtensions {
    KhrMaterialsTransmission KHR_materials_transmission;
};

struct GltfTransmissionMaterial {
    std::string_view name = "Masked transmitting glass";
    std::string_view alphaMode = "MASK";
    float alphaCutoff = 0.37f;
    GltfTransmissionPbr pbrMetallicRoughness;
    GltfTransmissionExtensions extensions;
};

struct GltfTransmissionDocument {
    GltfAsset asset;
    std::vector<std::string_view> extensionsUsed {"KHR_materials_transmission", "KHR_texture_transform"};
    int32_t scene = 0;
    std::vector<GltfScene> scenes {GltfScene {.nodes = {0}}};
    std::vector<GltfMeshNode> nodes {GltfMeshNode {.name = "TransmissionTriangle"}};
    std::vector<GltfSheenMesh> meshes {GltfSheenMesh {}};
    std::vector<GltfTransmissionMaterial> materials {GltfTransmissionMaterial {}};
    std::vector<GltfAccessor> accessors;
    std::vector<GltfAnisotropyBufferView> bufferViews;
    std::vector<GltfBuffer> buffers;
    std::vector<GltfSheenImage> images {GltfSheenImage {}};
    std::vector<GltfAnisotropyTexture> textures {GltfAnisotropyTexture {.sampler = 0}, GltfAnisotropyTexture {.sampler = 1}};
    std::vector<GltfSamplerWrap> samplers {GltfSamplerWrap {.wrapS = 33071, .wrapT = 33648},
                                           GltfSamplerWrap {.wrapS = 33648, .wrapT = 33071}};
};

// Same document with a root `extensions` object. A separate type rather than
// an optional member, for the omission reason above.
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

// Assembles a GLB container around a serialized JSON chunk and a binary chunk.
//
// Synthesizing the input is not the same as reimplementing the importer: this
// only produces bytes a conformant loader must accept, so the extension
// behaviour under test stays the importer's own.
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

// Triangle whose material carries KHR_materials_emissive_strength.
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
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document), TriangleBin());
}

// The same triangle and the same emissiveFactor, extension absent.
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
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document), TriangleBin());
}

[[nodiscard]] auto MakeRequiredSpecGlossFixture() -> std::vector<uint8_t> {
    const GltfSpecGlossDocument document {
        .scenes      = {GltfScene {.nodes = {0}}},
        .nodes       = {GltfMeshNode {.name = "SpecGlossBottle"}},
        .meshes      = TriangleMeshes(),
        .materials   = {GltfSpecGlossMaterial {}},
        .accessors   = TriangleAccessors(),
        .bufferViews = TriangleBufferViews(),
        .buffers     = TriangleBuffers(),
    };
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document), TriangleBin());
}

// A tangent-space triangle with a 1x1 *linear* anisotropy map (R=1, G=.5,
// B=.25). Base, PBR, emissive and anisotropy textures share the same image:
// color slots need sRGB views, data slots need linear views, and sampler modes
// differ per texture object rather than per image.
[[nodiscard]] auto MakeAnisotropyFixture() -> std::vector<uint8_t> {
    constexpr std::array<float, 9> normals {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<float, 12> tangents {1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<float, 6> uvs {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<uint8_t, 70> png {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0x00u, 0x00u, 0x00u, 0x0Du, 0x49u, 0x48u, 0x44u, 0x52u, 0x00u,
        0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0x1Fu, 0x15u, 0xC4u, 0x89u, 0x00u,
        0x00u, 0x00u, 0x0Du, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9Cu, 0x63u, 0xF8u, 0xDFu, 0xE0u, 0xF0u, 0x1Fu, 0x00u, 0x07u,
        0x00u, 0x02u, 0xBFu, 0x2Bu, 0xD7u, 0xC7u, 0xE2u, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4Eu, 0x44u, 0xAEu, 0x42u,
        0x60u, 0x82u
    };
    constexpr int32_t normalBytes  = static_cast<int32_t>(sizeof(normals));
    constexpr int32_t tangentBytes = static_cast<int32_t>(sizeof(tangents));
    constexpr int32_t uvBytes      = static_cast<int32_t>(sizeof(uvs));
    constexpr int32_t imageOffset  = kPositionBytes + normalBytes + tangentBytes + uvBytes + kIndexBytes;

    std::vector<uint8_t> bin(static_cast<size_t>(imageOffset) + png.size());
    std::memcpy(bin.data(), kTrianglePositions, kPositionBytes);
    std::memcpy(bin.data() + kPositionBytes, normals.data(), normalBytes);
    std::memcpy(bin.data() + kPositionBytes + normalBytes, tangents.data(), tangentBytes);
    std::memcpy(bin.data() + kPositionBytes + normalBytes + tangentBytes, uvs.data(), uvBytes);
    std::memcpy(bin.data() + imageOffset - kIndexBytes, kTriangleIndices, kIndexBytes);
    std::memcpy(bin.data() + imageOffset, png.data(), png.size());

    const GltfAnisotropyDocument document {
        .extensionsUsed = {"KHR_materials_anisotropy"},
        .scenes = {GltfScene {.nodes = {0}}},
        .nodes = {GltfMeshNode {.name = "AnisotropicTriangle"}},
        .meshes = {GltfAnisotropyMesh {.name = "Tri", .primitives = {GltfAnisotropyPrimitive {}}}},
        .materials = {GltfAnisotropyMaterial {
            .name = "Anisotropic",
            .pbrMetallicRoughness = {.metallicFactor = 1.0f, .roughnessFactor = 0.15f},
            .extensions = {.KHR_materials_anisotropy = {}}
        }},
        .accessors = {
            GltfAccessor {.bufferView = 0, .count = 3, .type = "VEC3", .min = {0.0f, 0.0f, 0.0f}, .max = {1.0f, 1.0f, 0.0f}},
            GltfAccessor {.bufferView = 1, .count = 3, .type = "VEC3", .min = {0.0f, 0.0f, 1.0f}, .max = {0.0f, 0.0f, 1.0f}},
            GltfAccessor {.bufferView = 2, .count = 3, .type = "VEC4", .min = {1.0f, 0.0f, 0.0f, 1.0f}, .max = {1.0f, 0.0f, 0.0f, 1.0f}},
            GltfAccessor {.bufferView = 3, .count = 3, .type = "VEC2", .min = {0.0f, 0.0f}, .max = {1.0f, 1.0f}},
            GltfAccessor {.bufferView = 4, .componentType = 5125, .count = 3, .type = "SCALAR", .min = {0.0f}, .max = {2.0f}}
        },
        .bufferViews = {
            {.byteOffset = 0, .byteLength = kPositionBytes},
            {.byteOffset = kPositionBytes, .byteLength = normalBytes},
            {.byteOffset = kPositionBytes + normalBytes, .byteLength = tangentBytes},
            {.byteOffset = kPositionBytes + normalBytes + tangentBytes, .byteLength = uvBytes},
            {.byteOffset = imageOffset - kIndexBytes, .byteLength = kIndexBytes},
            {.byteOffset = imageOffset, .byteLength = static_cast<int32_t>(png.size())}
        },
        .buffers = {{.byteLength = static_cast<int32_t>(bin.size())}},
        .images = {GltfAnisotropyImage {}},
        .textures = {GltfAnisotropyTexture {}, GltfAnisotropyTexture {.sampler = 0}, GltfAnisotropyTexture {.sampler = 1}},
        .samplers = {GltfSamplerWrap {.wrapS = 33071, .wrapT = 33648}, GltfSamplerWrap {.wrapS = 33648, .wrapT = 33071}}
    };
    // Texture 0 has no sampler at all, not a sampler explicitly set to repeat.
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document, 0, {.omitEmpty = true}), bin);
}

// The required transform repeats 30 times with a negative V scale, while
// albedo overrides TEXCOORD_0 with TEXCOORD_1 and rotates around the origin.
// Reusing one image in both color and data slots also checks color-space-aware
// upload (one sRGB handle and one linear handle, not a merged format).
[[nodiscard]] auto MakeSheenTransformFixture() -> std::vector<uint8_t> {
    constexpr std::array<float, 6> uv0 {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<float, 6> uv1 {0.5f, 0.2f, 0.75f, 0.2f, 0.5f, 0.8f};
    constexpr std::array<uint8_t, 70> png {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0x00u, 0x00u, 0x00u, 0x0Du, 0x49u, 0x48u, 0x44u, 0x52u, 0x00u,
        0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0x1Fu, 0x15u, 0xC4u, 0x89u, 0x00u,
        0x00u, 0x00u, 0x0Du, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9Cu, 0x63u, 0xF8u, 0xDFu, 0xE0u, 0xF0u, 0x1Fu, 0x00u, 0x07u,
        0x00u, 0x02u, 0xBFu, 0x2Bu, 0xD7u, 0xC7u, 0xE2u, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4Eu, 0x44u, 0xAEu, 0x42u,
        0x60u, 0x82u
    };
    constexpr int32_t uvSize = static_cast<int32_t>(sizeof(uv0));
    constexpr int32_t indexOffset = kPositionBytes + uvSize * 2;
    constexpr int32_t pngOffset = indexOffset + kIndexBytes;
    std::vector<uint8_t> bin(static_cast<size_t>(pngOffset) + png.size());
    std::memcpy(bin.data(), kTrianglePositions, kPositionBytes);
    std::memcpy(bin.data() + kPositionBytes, uv0.data(), uvSize);
    std::memcpy(bin.data() + kPositionBytes + uvSize, uv1.data(), uvSize);
    std::memcpy(bin.data() + indexOffset, kTriangleIndices, kIndexBytes);
    std::memcpy(bin.data() + pngOffset, png.data(), png.size());

    GltfSheenDocument document {};
    document.accessors = {
        GltfAccessor {.bufferView = 0, .count = 3, .type = "VEC3", .min = {0.0f, 0.0f, 0.0f}, .max = {1.0f, 1.0f, 0.0f}},
        GltfAccessor {.bufferView = 1, .count = 3, .type = "VEC2"},
        GltfAccessor {.bufferView = 2, .count = 3, .type = "VEC2"},
        GltfAccessor {.bufferView = 3, .componentType = 5125, .count = 3, .type = "SCALAR", .min = {0.0f}, .max = {2.0f}}
    };
    document.bufferViews = {
        {.byteOffset = 0, .byteLength = kPositionBytes},
        {.byteOffset = kPositionBytes, .byteLength = uvSize},
        {.byteOffset = kPositionBytes + uvSize, .byteLength = uvSize},
        {.byteOffset = indexOffset, .byteLength = kIndexBytes},
        {.byteOffset = pngOffset, .byteLength = static_cast<int32_t>(png.size())}
    };
    document.buffers = {{.byteLength = static_cast<int32_t>(bin.size())}};
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document, 0, {.omitEmpty = true}), bin);
}

// The same PNG is baseColor (sRGB, with alpha coverage) and transmission
// (linear R). The two textureInfo objects have independent samplers and UV
// transforms, as in Khronos TransmissionTest's blue masked spheres.
[[nodiscard]] auto MakeTransmissionFixture(std::string_view alphaMode) -> std::vector<uint8_t> {
    constexpr std::array<float, 6> uv0 {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<float, 6> uv1 {0.5f, 0.2f, 0.75f, 0.2f, 0.5f, 0.8f};
    constexpr std::array<uint8_t, 70> png {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0x00u, 0x00u, 0x00u, 0x0Du, 0x49u, 0x48u, 0x44u, 0x52u, 0x00u,
        0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0x1Fu, 0x15u, 0xC4u, 0x89u, 0x00u,
        0x00u, 0x00u, 0x0Du, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9Cu, 0x63u, 0xF8u, 0xDFu, 0xE0u, 0xF0u, 0x1Fu, 0x00u, 0x07u,
        0x00u, 0x02u, 0xBFu, 0x2Bu, 0xD7u, 0xC7u, 0xE2u, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4Eu, 0x44u, 0xAEu, 0x42u,
        0x60u, 0x82u
    };
    constexpr int32_t uvBytes = static_cast<int32_t>(sizeof(uv0));
    constexpr int32_t indexOffset = kPositionBytes + uvBytes * 2;
    constexpr int32_t imageOffset = indexOffset + kIndexBytes;

    std::vector<uint8_t> bin(static_cast<size_t>(imageOffset) + png.size());
    std::memcpy(bin.data(), kTrianglePositions, kPositionBytes);
    std::memcpy(bin.data() + kPositionBytes, uv0.data(), uvBytes);
    std::memcpy(bin.data() + kPositionBytes + uvBytes, uv1.data(), uvBytes);
    std::memcpy(bin.data() + indexOffset, kTriangleIndices, kIndexBytes);
    std::memcpy(bin.data() + imageOffset, png.data(), png.size());

    GltfTransmissionDocument document {};
    document.materials[0].alphaMode = alphaMode;
    document.accessors = {
        GltfAccessor {.bufferView = 0, .count = 3, .type = "VEC3", .min = {0.0f, 0.0f, 0.0f}, .max = {1.0f, 1.0f, 0.0f}},
        GltfAccessor {.bufferView = 1, .count = 3, .type = "VEC2"},
        GltfAccessor {.bufferView = 2, .count = 3, .type = "VEC2"},
        GltfAccessor {.bufferView = 3, .componentType = 5125, .count = 3, .type = "SCALAR", .min = {0.0f}, .max = {2.0f}}
    };
    document.bufferViews = {
        {.byteOffset = 0, .byteLength = kPositionBytes},
        {.byteOffset = kPositionBytes, .byteLength = uvBytes},
        {.byteOffset = kPositionBytes + uvBytes, .byteLength = uvBytes},
        {.byteOffset = indexOffset, .byteLength = kIndexBytes},
        {.byteOffset = imageOffset, .byteLength = static_cast<int32_t>(png.size())}
    };
    document.buffers = {{.byteLength = static_cast<int32_t>(bin.size())}};
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document, 0, {.omitEmpty = true}), bin);
}

// A mesh node that also carries a punctual light, alongside the emissive
// extension: two extensions on one document, one of them unread.
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
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document), TriangleBin());
}

// Geometry-free document carrying only a punctual light -- the shape zcook
// emits for a cooked scene light (tools/zcook/GLB.cpp).
[[nodiscard]] auto MakeLightOnlyFixture() -> std::vector<uint8_t> {
    const GltfLightDocument<GltfLightNode, GltfPlainMaterial> document {
        .extensionsUsed = {"KHR_lights_punctual"},
        .extensions     = {.KHR_lights_punctual = {.lights = {KhrPunctualLight {.name = "TestPoint", .color = {1.0f, 0.5f, 0.25f}, .intensity = 42.0f}}}},
        .scenes         = {GltfScene {.nodes = {0}}},
        .nodes          = {GltfLightNode {.name = "PunctualLight", .translation = {1.0f, 2.0f, 3.0f}}},
    };
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document), {});
}

// Independent cgltf view of the same bytes, used as the reference the
// imported prefab is compared against.
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
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~GLTFImportTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        /**
         * Loads the base rig through the shipping importer and checks the node
         * graph it produced: one prefab node per glTF node, parents resolved to
         * indices, mesh flags, and a local-transform convention that composes
         * back into cgltf's world transforms.
         */
        std::expected<void, ZHLN::ErrorCode> importer_flattens_node_graph_from_source_document() {
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

            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Import");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            const ZHLN::ModelPrefab* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), bytes, kVirtualPath);
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
                // The importer must upload three independently addressable
                // vertex streams, including the surface that carries UV1.
                const auto& mesh = part.mesh;
                if (mesh.posBuffer == ZHLN::BufferHandle::Invalid || mesh.tangentFrameBuffer == ZHLN::BufferHandle::Invalid ||
                    mesh.surfaceBuffer == ZHLN::BufferHandle::Invalid || mesh.posBuffer == mesh.tangentFrameBuffer || mesh.posBuffer == mesh.surfaceBuffer ||
                    mesh.tangentFrameBuffer == mesh.surfaceBuffer) {
                    return std::unexpected(GLTFImportError::PartMismatch);
                }
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
                        sourceHasJointWeights =
                            sourceHasJointWeights || owner.mesh->primitives[primitive].attributes[attribute].type == cgltf_attribute_type_joints;
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
            if (ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), bytes, kVirtualPath) != prefab) {
                return std::unexpected(GLTFImportError::PrefabCacheMismatch);
            }
            return {};
        }

        /**
         * Checks the two consumers the animation runtime actually reads: skin
         * joints (node indices, intra-skin parents, inverse binds) and animation
         * channels (target nodes, key counts, component widths, duration).
         */
        std::expected<void, ZHLN::ErrorCode> importer_builds_skins_and_animation_channels() {
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

            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Skin Import");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            // The prefab cache lives on the pooled engine and outlives the
            // test, so the distinct virtual path is what keeps the two imports
            // apart in the engine log.
            const ZHLN::ModelPrefab* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), bytes, "ProceduralAnimationBaseRig_Skins.glb");
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
                    if (skin.inverse_bind_matrices != nullptr &&
                        !(SourceWorld(*sourceNode) * joint.inverseBindMatrix).IsClose(JPH::Mat44::sIdentity(), 0.0001f)) {
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
         * Khronos extensions. Emissive strength is tested here against the
         * authored factor; anisotropy has its own fixture below. Unread light
         * extensions are pinned as the behaviour they actually have rather
         * than the behaviour a reader might assume.
         *
         * In the default engine presentation, imported factors carry the
         * kGLTFEmissiveDisplayScale boost for low-exposure scenes. The extension
         * is a relative multiplier on top of that boost, so the two fixtures
         * below differ by precisely kEmissiveStrength. The fidelity-mode
         * import with a 1:1 scale is tested separately below.
         *
         * KHR_lights_punctual in particular is exported by zcook
         * (tools/zcook/GLB.cpp) but never read back: ModelPrefab has no
         * light representation at all, so a cooked light survives the round
         * trip only through the cooker's own manifest. What is enforced here is
         * that such a file still imports cleanly instead of failing or
         * corrupting the node graph.
         */
        std::expected<void, ZHLN::ErrorCode> importer_applies_supported_khronos_extensions() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Extensions");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            // 1. KHR_materials_emissive_strength scales the authored emissive
            //    factor, on top of the import-time unit conversion.
            const std::vector<uint8_t> strengthBytes = MakeEmissiveStrengthFixture();
            const ZHLN::ModelPrefab*   withStrength =
                ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), strengthBytes, "ext_emissive_strength.glb");
            if (withStrength == nullptr || withStrength->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                const float expected = kAuthoredEmissive[channel] * kEmissiveStrength * ZHLN::kGLTFEmissiveDisplayScale;
                if (std::abs(withStrength->parts[0].defaultMaterial.emissiveFactor[channel] - expected) > 0.01f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // 2. The same material without the extension carries the unit
            //    conversion alone. Asserting both spellings pins the extension
            //    as a relative multiplier: an asset that writes
            //    emissiveStrength = 1 must import identically to one that omits
            //    the extension, rather than 100x darker.
            const std::vector<uint8_t> plainBytes = MakePlainEmissiveFixture();
            const ZHLN::ModelPrefab*   plain      = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), plainBytes, "ext_emissive_plain.glb");
            if (plain == nullptr || plain->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                const float expected = kAuthoredEmissive[channel] * ZHLN::kGLTFEmissiveDisplayScale;
                if (std::abs(plain->parts[0].defaultMaterial.emissiveFactor[channel] - expected) > 0.01f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
                // ... and the extension is exactly the ratio between the two.
                const float ratio = withStrength->parts[0].defaultMaterial.emissiveFactor[channel] / std::max(expected, 1e-6f);
                if (std::abs(ratio - kEmissiveStrength) > 0.001f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // 3. An unread extension on a mesh-bearing node must not disturb the
            //    node it sits on, the part it produces, or the extension that is
            //    read from the same document.
            const std::vector<uint8_t> litBytes = MakeLitMeshFixture();
            const ZHLN::ModelPrefab*   litMesh  = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), litBytes, "ext_lit_mesh.glb");
            if (litMesh == nullptr || litMesh->nodes.size() != 1 || litMesh->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            const ZHLN::ModelNode& litNode = litMesh->nodes[0];
            if (std::string_view(litNode.name) != "LitTriangle" || !litNode.hasMesh || litNode.parentIndex != -1 ||
                !litNode.localTransform.GetTranslation().IsClose(JPH::Vec3(1.0f, 2.0f, 3.0f), 0.0001f) || litMesh->parts[0].nodeIndex != 0) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                const float expected = kAuthoredEmissive[channel] * kEmissiveStrength * ZHLN::kGLTFEmissiveDisplayScale;
                if (std::abs(litMesh->parts[0].defaultMaterial.emissiveFactor[channel] - expected) > 0.01f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // 4. A geometry-free light document -- what zcook emits for a scene
            //    light -- imports as a bare transform node. The light itself is
            //    dropped: ModelPrefab has nowhere to put it. Pinning that keeps
            //    the gap visible instead of implied.
            const std::vector<uint8_t> lightOnlyBytes = MakeLightOnlyFixture();
            const ZHLN::ModelPrefab*   lightOnly      = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), lightOnlyBytes, "ext_light_only.glb");
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

        /**
         * The fidelity harness must import emission in glTF's authored linear
         * units, not the low-exposure game's boosted units. The extension
         * still multiplies the factor, and the emissive texture must use the
         * sRGB view of the same bytes used by the linear PBR data texture.
         */
        std::expected<void, ZHLN::ErrorCode> importer_preserves_conformance_emission_and_texture_color_space() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Emissive Fidelity");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }
            auto& rc = engine->GetRenderContext();
            auto& assets = engine->GetAssetManager();
            constexpr ZHLN::GLTF::ImportOptions conformant {.emissiveFactorScale = 1.0f};

            const auto plainBytes = MakePlainEmissiveFixture();
            const auto* plain = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, plainBytes, "fidelity_emissive_plain.glb", {}, conformant);
            const auto strengthBytes = MakeEmissiveStrengthFixture();
            const auto* strong = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, strengthBytes, "fidelity_emissive_strength.glb", {}, conformant);
            if (plain == nullptr || strong == nullptr || plain->parts.size() != 1 || strong->parts.size() != 1 ||
                plain->emissiveFactorScale != 1.0f || strong->emissiveFactorScale != 1.0f ||
                plain->maxTextureDimension != ZHLN::kGLTFDefaultMaxTextureDimension) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                if (std::abs(plain->parts[0].defaultMaterial.emissiveFactor[channel] - kAuthoredEmissive[channel]) > 1e-5f ||
                    std::abs(strong->parts[0].defaultMaterial.emissiveFactor[channel] - kAuthoredEmissive[channel] * kEmissiveStrength) > 1e-5f) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
            }

            // An emissive image and albedo share a source image. Both must
            // sample sRGB; a PBR data reference to the same image must not.
            const auto texturedBytes = MakeAnisotropyFixture();
            const auto* textured = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, texturedBytes, "fidelity_emissive_texture.glb", {}, conformant);
            if (textured == nullptr || textured->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            const auto& material = textured->parts[0].defaultMaterial;
            if (material.emissiveFactor != std::array<float, 4> {0.0f, 1.0f, 0.0f, 1.0f} ||
                material.emissiveMap == ZHLN::TextureHandle::Invalid || material.emissiveMap != material.albedoMap ||
                material.emissiveMap == material.pbrMap || rc.GetBindlessIndex(material.emissiveMap) <= 2u) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }

            // Both texture-resolution policy and emissive scale belong to the
            // prefab cache identity. Fidelity can retain 2048px images while
            // other import clients continue using the default 1024px cap.
            constexpr ZHLN::GLTF::ImportOptions detailed {.emissiveFactorScale = 1.0f, .maxTextureDimension = 2048};
            const auto* highRes = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, texturedBytes, "fidelity_emissive_highres.glb", {}, detailed);
            if (highRes == nullptr || highRes->maxTextureDimension != 2048 ||
                ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, texturedBytes, "fidelity_emissive_highres.glb", {}, detailed) != highRes ||
                ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, texturedBytes, "fidelity_emissive_highres.glb", {}, conformant) != nullptr ||
                ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, texturedBytes, "fidelity_emissive_highres.glb", {},
                                                    ZHLN::GLTF::ImportOptions {.emissiveFactorScale = 1.0f, .maxTextureDimension = 0}) != nullptr ||
                ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, plainBytes, "fidelity_emissive_plain.glb", {}, conformant) != plain ||
                ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, plainBytes, "fidelity_emissive_plain.glb") != nullptr) {
                return std::unexpected(GLTFImportError::PrefabCacheMismatch);
            }
            return {};
        }

        /**
         * KHR_materials_anisotropy and per-texture sampler wrapping survive
         * cgltf -> prefab -> Material. Three texture objects share one image:
         * base color clamps S / mirrors T, PBR mirrors S / clamps T, and the
         * anisotropy texture repeats both. Their image handle stays shared.
         */
        std::expected<void, ZHLN::ErrorCode> importer_preserves_anisotropy_material() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Anisotropy");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }
            const auto bytes = MakeAnisotropyFixture();
            SourceDocument source;
            if (!source.Parse(bytes) || source.data->materials_count != 1 || !source.data->materials[0].has_anisotropy ||
                source.data->textures_count != 3 || source.data->samplers_count != 2 || source.data->textures[0].sampler != nullptr ||
                source.data->textures[1].sampler == nullptr || source.data->textures[2].sampler == nullptr ||
                source.data->textures[0].image == nullptr || source.data->textures[0].image != source.data->textures[1].image ||
                source.data->textures[0].image != source.data->textures[2].image) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }

            auto& rc = engine->GetRenderContext();
            const ZHLN::ModelPrefab* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, engine->GetAssetManager(), bytes, "ext_anisotropy.glb");
            if (prefab == nullptr || prefab->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            const auto& expected = source.data->materials[0].anisotropy;
            const auto& material = prefab->parts[0].defaultMaterial;
            if (std::abs(material.anisotropyStrength - expected.anisotropy_strength) > 1e-5f ||
                std::abs(material.anisotropyRotation - expected.anisotropy_rotation) > 1e-5f ||
                material.anisotropyMap == ZHLN::TextureHandle::Invalid || rc.GetBindlessIndex(material.anisotropyMap) <= 2u ||
                material.metallicFactor != 1.0f || material.roughnessFactor != 0.15f || material.clearcoatFactor != 0.0f) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            using ZHLN::MaterialTextureSlot;
            using ZHLN::TextureWrap;
            ZHLN::MaterialSamplerAddresses expectedSamplers {};
            expectedSamplers[static_cast<size_t>(MaterialTextureSlot::Albedo)] = {TextureWrap::ClampToEdge, TextureWrap::MirroredRepeat};
            expectedSamplers[static_cast<size_t>(MaterialTextureSlot::Pbr)] = {TextureWrap::MirroredRepeat, TextureWrap::ClampToEdge};
            // Albedo is sRGB; PBR and anisotropy are linear data, even when
            // all three texture objects reference the same image bytes.
            if (material.albedoMap == material.pbrMap || material.pbrMap != material.anisotropyMap ||
                material.textureSamplers != expectedSamplers) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            const ZHLN::Material defaults {};
            if (defaults.anisotropyStrength != 0.0f || defaults.anisotropyRotation != 0.0f ||
                defaults.anisotropyMap != ZHLN::TextureHandle::Invalid || defaults.textureSamplers != ZHLN::MaterialSamplerAddresses {}) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            return {};
        }

        /**
         * Khronos NormalTangentTest omits TANGENT and rotates the normal-map
         * UVs on each flat panel. In glTF's OpenGL normal-map convention, a
         * positive green (+Y) texel on the top half of a sphere points UP even
         * though increasing UV V moves DOWN the image. The companion
         * NormalTangentMirrorTest's authored tangents likewise have w=+1 for
         * the ordinary UV-down quad and w=-1 on a mirrored quad. Check both
         * the frame and an actual sampled map direction across UV rotations.
         */
        std::expected<void, ZHLN::ErrorCode> missing_tangents_follow_rotated_and_mirrored_uvs() {
            constexpr std::array<ZHLN::VertexPosition, 3> positions {{{{0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f}}}};
            constexpr std::array<std::array<float, 3>, 3> normals {{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}};
            constexpr std::array<uint32_t, 3> indices {0, 1, 2};
            const auto check = [&](const std::array<std::array<float, 2>, 3>& uv, float tx, float ty, float bx, float by, float handedness,
                                   bool sampleConvex = true) {
                const auto tangents = ZHLN::GLTF::GenerateTangents(positions, normals, uv, indices);
                if (tangents.size() != positions.size()) return false;
                // At world (0.25, 0.75), sample a radial normal map centered at
                // UV (0.5, 0.5). R points toward +U; G points toward image-UP
                // (-V). All differently rotated UV charts must yield the same
                // world-space normal (-0.25, +0.25, z), not a concave sphere.
                const float u = 0.25f * uv[1][0] + 0.75f * uv[2][0];
                const float v = 0.25f * uv[1][1] + 0.75f * uv[2][1];
                const float mapX = u - 0.5f, mapY = 0.5f - v;
                for (const auto& t: tangents) {
                    const float actualBx = -t[1] * t[3], actualBy = t[0] * t[3];
                    if (std::abs(t[0] - tx) > 0.01f || std::abs(t[1] - ty) > 0.01f || std::abs(t[2]) > 0.01f ||
                        t[3] != handedness || std::abs(actualBx - bx) > 0.01f || std::abs(actualBy - by) > 0.01f ||
                        (sampleConvex && (std::abs(mapX * t[0] + mapY * actualBx + 0.25f) > 0.01f ||
                                          std::abs(mapX * t[1] + mapY * actualBy - 0.25f) > 0.01f))) {
                        return false;
                    }
                }
                return true;
            };
            if (!check({{{0, 1}, {1, 1}, {0, 0}}}, 1, 0, 0, 1, 1) ||
                !check({{{0, 0}, {0, 1}, {1, 0}}}, 0, 1, -1, 0, 1) ||
                !check({{{1, 0}, {0, 0}, {1, 1}}}, -1, 0, 0, -1, 1) ||
                !check({{{1, 1}, {1, 0}, {0, 1}}}, 0, -1, 1, 0, 1) ||
                !check({{{1, 1}, {0, 1}, {1, 0}}}, -1, 0, 0, 1, -1) ||
                !check({{{0, 0}, {0, 0}, {0, 0}}}, 1, 0, 0, 1, 1, false)) {
                return std::unexpected(GLTFImportError::TangentFrameMismatch);
            }
            return {};
        }

        /**
         * The official UnlitTest GLB requires KHR_materials_unlit. Its two
         * materials leave metallicFactor at the glTF default 1: altering the
         * fallback PBR fields to get a flat image is not implementing unlit.
         */
        std::expected<void, ZHLN::ErrorCode> importer_preserves_required_unlit_materials() {
            const auto bytes = ReadUnlitAssetBytes();
            SourceDocument source;
            if (bytes.empty() || !source.Parse(bytes) || source.data->materials_count != 2 || source.data->meshes_count != 2 ||
                !source.data->materials[0].unlit || !source.data->materials[1].unlit) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Unlit");
            if (engine == nullptr) return std::unexpected(GLTFImportError::EngineInitFailed);
            const auto* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(
                engine->GetRenderContext(), engine->GetAssetManager(), bytes, "KHR_materials_unlit.glb"
            );
            if (prefab == nullptr || prefab->parts.size() != 2) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            for (size_t i = 0; i < 2; ++i) {
                const auto& expected = source.data->materials[i];
                const auto& material = prefab->parts[i].defaultMaterial;
                if (!material.unlit || material.alphaMode != 0u || material.albedoMap != ZHLN::TextureHandle::Invalid ||
                    material.metallicFactor != expected.pbr_metallic_roughness.metallic_factor || material.metallicFactor != 1.0f ||
                    material.pipeline == ZHLN::PipelineHandle::Invalid) {
                    return std::unexpected(GLTFImportError::ExtensionMismatch);
                }
                for (size_t channel = 0; channel < 4; ++channel) {
                    if (std::abs(material.baseColorFactor[channel] - expected.pbr_metallic_roughness.base_color_factor[channel]) > 1e-6f) {
                        return std::unexpected(GLTFImportError::ExtensionMismatch);
                    }
                }
            }
            const ZHLN::Material defaults {};
            if (defaults.unlit) return std::unexpected(GLTFImportError::ExtensionMismatch);
            return {};
        }

        /**
         * An unsupported *required* extension is advisory, not an import
         * veto. A spec/gloss-only material has no core PBR fallback, so the
         * primitive still renders but keeps the renderer's gray defaults.
         */
        std::expected<void, ZHLN::ErrorCode> unsupported_required_specular_glossiness_keeps_gray_fallback() {
            const auto bytes = MakeRequiredSpecGlossFixture();
            SourceDocument source;
            if (!source.Parse(bytes) || source.data->extensions_required_count != 1 || source.data->materials_count != 1 ||
                !source.data->materials[0].has_pbr_specular_glossiness || source.data->materials[0].has_pbr_metallic_roughness) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Required SpecGloss");
            if (engine == nullptr) return std::unexpected(GLTFImportError::EngineInitFailed);
            const auto* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(
                engine->GetRenderContext(), engine->GetAssetManager(), bytes, "required_specular_glossiness.glb"
            );
            if (prefab == nullptr || prefab->nodes.size() != 1 || prefab->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }
            const auto& material = prefab->parts[0].defaultMaterial;
            if (material.baseColorFactor != std::array<float, 4> {1.0f, 1.0f, 1.0f, 1.0f} || material.metallicFactor != 1.0f ||
                material.roughnessFactor != 1.0f || material.albedoMap != ZHLN::TextureHandle::Invalid ||
                material.pipeline == ZHLN::PipelineHandle::Invalid) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            return {};
        }

        /**
         * Khronos NegativeScaleTest contains a single-sided check/X material
         * on a mirrored node and double-sided spheres instantiated under both
         * signs of their full parent-to-child transform. The primitive cache
         * must share geometry without letting the first node's parity change
         * its material (or the authored doubleSided flag).
         */
        std::expected<void, ZHLN::ErrorCode> negative_scale_keeps_authored_sidedness_on_shared_meshes() {
            const auto bytes = ReadNegativeScaleAssetBytes();
            SourceDocument source;
            if (bytes.empty() || !source.Parse(bytes) || source.data->nodes_count != 14 || source.data->materials_count != 6) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless Khronos NegativeScaleTest");
            if (engine == nullptr) return std::unexpected(GLTFImportError::EngineInitFailed);

            const auto* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(
                engine->GetRenderContext(), engine->GetAssetManager(), bytes, "khronos_negative_scale_test.glb"
            );
            if (prefab == nullptr || prefab->parts.size() != 11) return std::unexpected(GLTFImportError::PrefabLoadFailed);

            const auto findPart = [prefab](std::string_view name) -> const ZHLN::ModelPart* {
                const auto found = std::ranges::find_if(prefab->parts, [name](const auto& part) { return std::string_view(part.name) == name; });
                return found != prefab->parts.end() ? &*found : nullptr;
            };
            for (const auto& part: prefab->parts) {
                if (part.nodeIndex < 0 || static_cast<size_t>(part.nodeIndex) >= source.data->nodes_count) {
                    return std::unexpected(GLTFImportError::NegativeScaleMismatch);
                }
                const auto& node = source.data->nodes[static_cast<size_t>(part.nodeIndex)];
                if (node.mesh == nullptr || node.mesh->primitives_count != 1 || node.mesh->primitives[0].material == nullptr ||
                    part.defaultMaterial.doubleSided != node.mesh->primitives[0].material->double_sided ||
                    part.defaultMaterial.pipeline == ZHLN::PipelineHandle::Invalid) {
                    return std::unexpected(GLTFImportError::NegativeScaleMismatch);
                }
            }

            const auto* front = findPart("NegativeScaleFront");
            const auto* back  = findPart("NegativeScaleBack");
            if (front == nullptr || back == nullptr || front->defaultMaterial.doubleSided || back->defaultMaterial.doubleSided ||
                SourceWorld(source.data->nodes[static_cast<size_t>(front->nodeIndex)]).GetDeterminant3x3() >= 0.0f) {
                return std::unexpected(GLTFImportError::NegativeScaleMismatch);
            }

            // The same sphere primitive is drawn with different full-transform
            // parities, including a negative determinant inherited from its
            // parent. Both instances must retain the authored material.
            constexpr std::array pairs {
                std::pair {"NotShiny1", "NotShinyMinus1"},
                std::pair {"Shiny1", "ShinyMinus1"},
                std::pair {"Dark1", "DarkMinus1"},
            };
            for (const auto& [positiveName, negativeName]: pairs) {
                const auto* a = findPart(positiveName);
                const auto* b = findPart(negativeName);
                if (a == nullptr || b == nullptr || !a->defaultMaterial.doubleSided || !b->defaultMaterial.doubleSided ||
                    a->mesh.posBuffer == ZHLN::BufferHandle::Invalid || a->mesh.posBuffer != b->mesh.posBuffer ||
                    a->defaultMaterial.pipeline != b->defaultMaterial.pipeline) {
                    return std::unexpected(GLTFImportError::NegativeScaleMismatch);
                }
                const bool aMirrored = SourceWorld(source.data->nodes[static_cast<size_t>(a->nodeIndex)]).GetDeterminant3x3() < 0.0f;
                const bool bMirrored = SourceWorld(source.data->nodes[static_cast<size_t>(b->nodeIndex)]).GetDeterminant3x3() < 0.0f;
                if (aMirrored == bMirrored) return std::unexpected(GLTFImportError::NegativeScaleMismatch);
            }

            std::array<ZHLN::Entity, 16> spawned {};
            const auto count = ZHLN::PrefabFactory::InstantiatePrefab(
                *engine, *prefab, {.createPhysics = false, .emissiveVirtualLights = false},
                spawned.data(), static_cast<uint32_t>(spawned.size())
            );
            if (count != prefab->parts.size() + 1) return std::unexpected(GLTFImportError::NegativeScaleMismatch);
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            // The renderer derives the raster-winding flag from the *actual*
            // draw world matrix. Check the spawned hierarchy as well as cgltf's
            // source hierarchy, especially the negatively scaled sphere parents.
            auto& registry = engine->GetRegistry();
            size_t matched = 0;
            for (uint32_t i = 1; i < count; ++i) { // outBuffer[0] is the prefab root, not a mesh part.
                const auto* name = registry.Get<ZHLN::Components::NameComponent>(spawned[i]);
                const auto* world = registry.Get<ZHLN::Components::WorldTransformComponent>(spawned[i]);
                if (name == nullptr || world == nullptr) return std::unexpected(GLTFImportError::NegativeScaleMismatch);
                const auto* part = findPart(std::string_view(name->name));
                if (part == nullptr) return std::unexpected(GLTFImportError::NegativeScaleMismatch);
                const bool sourceMirrored = SourceWorld(source.data->nodes[static_cast<size_t>(part->nodeIndex)]).GetDeterminant3x3() < 0.0f;
                if ((world->world.GetDeterminant3x3() < 0.0f) != sourceMirrored) {
                    return std::unexpected(GLTFImportError::NegativeScaleMismatch);
                }
                ++matched;
            }
            if (matched != prefab->parts.size()) return std::unexpected(GLTFImportError::NegativeScaleMismatch);
            return {};
        }

        /**
         * Transmission is not alpha blending: glTF MASK still describes holes
         * in the glass, and transmissionTexture uses a separate linear R map.
         * The same image can have a different sampler/UV set as baseColor.
         */
        std::expected<void, ZHLN::ErrorCode> importer_preserves_transmission_texture_and_alpha_coverage() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Transmission");
            if (engine == nullptr) return std::unexpected(GLTFImportError::EngineInitFailed);
            const auto maskedBytes = MakeTransmissionFixture("MASK");
            const auto opaqueBytes = MakeTransmissionFixture("OPAQUE");
            SourceDocument source;
            if (!source.Parse(maskedBytes) || source.data->materials_count != 1 || !source.data->materials[0].has_transmission ||
                source.data->materials[0].alpha_mode != cgltf_alpha_mode_mask ||
                !source.data->materials[0].transmission.transmission_texture.has_transform ||
                source.data->textures_count != 2 || source.data->textures[0].image != source.data->textures[1].image) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }

            auto& rc = engine->GetRenderContext();
            auto& assets = engine->GetAssetManager();
            const auto* masked = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, maskedBytes, "masked_transmission.glb");
            const auto* opaque = ZHLN::GLTF::LoadGLBPrefabFromMemory(rc, assets, opaqueBytes, "opaque_transmission.glb");
            if (masked == nullptr || opaque == nullptr || masked->parts.size() != 1 || opaque->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }

            const auto& expected = source.data->materials[0];
            const auto& mat = masked->parts[0].defaultMaterial;
            const auto& opaqueMat = opaque->parts[0].defaultMaterial;
            if (mat.alphaMode != 1u || opaqueMat.alphaMode != 0u || std::abs(mat.alphaCutoff - expected.alpha_cutoff) > 1e-5f ||
                std::abs(mat.baseColorFactor[3] - expected.pbr_metallic_roughness.base_color_factor[3]) > 1e-5f ||
                std::abs(mat.metallicFactor - expected.pbr_metallic_roughness.metallic_factor) > 1e-5f ||
                std::abs(mat.transmissionFactor - expected.transmission.transmission_factor) > 1e-5f ||
                mat.albedoMap == ZHLN::TextureHandle::Invalid || mat.transmissionMap == ZHLN::TextureHandle::Invalid ||
                mat.albedoMap == mat.transmissionMap || rc.GetBindlessIndex(mat.transmissionMap) <= 2u ||
                opaqueMat.transmissionMap == ZHLN::TextureHandle::Invalid) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            const auto albedoSlot = static_cast<size_t>(ZHLN::MaterialTextureSlot::Albedo);
            const auto transSlot = static_cast<size_t>(ZHLN::MaterialTextureSlot::Transmission);
            if (mat.textureSamplers[albedoSlot] != ZHLN::TextureSamplerAddress {ZHLN::TextureWrap::ClampToEdge, ZHLN::TextureWrap::MirroredRepeat} ||
                mat.textureSamplers[transSlot] != ZHLN::TextureSamplerAddress {ZHLN::TextureWrap::MirroredRepeat, ZHLN::TextureWrap::ClampToEdge}) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            const auto& transform = mat.textureTransforms[transSlot];
            if (transform.texCoord != 1u || transform.offset != std::array<float, 2> {0.25f, -0.25f} ||
                transform.scale != std::array<float, 2> {2.0f, 0.5f} || std::abs(transform.rotation - 1.5707963f) > 1e-5f ||
                mat.textureTransforms[albedoSlot].texCoord != 0u) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> importer_preserves_texture_transforms_and_sheen() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless glTF Sheen UV transform");
            if (engine == nullptr) return std::unexpected(GLTFImportError::EngineInitFailed);
            const auto bytes = MakeSheenTransformFixture();
            SourceDocument source;
            if (!source.Parse(bytes) || source.data->materials_count != 1 || !source.data->materials[0].has_sheen ||
                !source.data->materials[0].pbr_metallic_roughness.base_color_texture.has_transform ||
                source.data->meshes[0].primitives[0].attributes_count != 3) {
                return std::unexpected(GLTFImportError::AssetUnavailable);
            }
            const auto* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(
                engine->GetRenderContext(), engine->GetAssetManager(), bytes, "ext_sheen_transform.glb"
            );
            if (prefab == nullptr || prefab->parts.size() != 1) return std::unexpected(GLTFImportError::PrefabLoadFailed);

            const auto& material = prefab->parts[0].defaultMaterial;
            using ZHLN::MaterialTextureSlot;
            const auto& albedo = material.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Albedo)];
            const auto& pbr = material.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Pbr)];
            const auto& normal = material.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Normal)];
            const auto& sheenColor = material.textureTransforms[static_cast<size_t>(MaterialTextureSlot::SheenColor)];
            const auto& sheenRoughness = material.textureTransforms[static_cast<size_t>(MaterialTextureSlot::SheenRoughness)];
            const auto& occlusion = material.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Occlusion)];
            if (albedo.texCoord != 1u || albedo.offset != std::array<float, 2> {0.2f, 0.4f} ||
                albedo.scale != std::array<float, 2> {2.0f, -3.0f} || std::abs(albedo.rotation - 1.5707963f) > 1e-5f ||
                pbr.texCoord != 0u || pbr.scale != std::array<float, 2> {30.0f, -30.0f} ||
                normal != pbr || sheenColor.texCoord != 0u ||
                sheenColor.scale != std::array<float, 2> {4.0f, -5.0f} ||
                sheenRoughness.texCoord != 1u || sheenRoughness.scale != std::array<float, 2> {1.0f, 1.0f} ||
                occlusion.scale != std::array<float, 2> {1.0f, 1.0f}) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            // A positive rotation in top-left-origin UV space moves +U toward
            // -V. The separate render test checks the actual instance rows and
            // shader against the untransformed Sample column.
            auto apply = [](const ZHLN::MaterialTextureTransform& t, std::array<float, 2> uv) {
                const float u = uv[0] * t.scale[0], v = uv[1] * t.scale[1];
                return std::array<float, 2> {t.offset[0] + std::cos(t.rotation) * u + std::sin(t.rotation) * v,
                                              t.offset[1] - std::sin(t.rotation) * u + std::cos(t.rotation) * v};
            };
            const auto transformed = apply(albedo, {0.5f, 0.2f}); // TEXCOORD_1, not TEXCOORD_0.
            const auto tiled = apply(pbr, {0.25f, 0.5f});
            if (std::abs(transformed[0] + 0.4f) > 1e-4f || std::abs(transformed[1] + 0.6f) > 1e-4f ||
                std::abs(tiled[0] - 7.5f) > 1e-4f || std::abs(tiled[1] + 15.0f) > 1e-4f) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            auto& rc = engine->GetRenderContext();
            if (material.sheenColorFactor != std::array<float, 3> {0.25f, 0.5f, 0.75f} ||
                std::abs(material.sheenRoughnessFactor - 0.35f) > 1e-5f ||
                material.sheenColorMap == ZHLN::TextureHandle::Invalid ||
                material.sheenRoughnessMap == ZHLN::TextureHandle::Invalid ||
                material.occlusionMap == ZHLN::TextureHandle::Invalid ||
                material.albedoMap != material.sheenColorMap || material.pbrMap != material.sheenRoughnessMap ||
                material.sheenRoughnessMap != material.occlusionMap ||
                std::abs(material.occlusionStrength - 0.4f) > 1e-5f || material.albedoMap == material.pbrMap ||
                rc.GetBindlessIndex(material.sheenColorMap) <= 2 || rc.GetBindlessIndex(material.sheenRoughnessMap) <= 2 ||
                rc.GetBindlessIndex(material.albedoMap) == rc.GetBindlessIndex(material.pbrMap)) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            const ZHLN::Material defaults {};
            if (defaults.sheenColorFactor != std::array<float, 3> {0.0f, 0.0f, 0.0f} ||
                defaults.sheenRoughnessFactor != 0.0f || defaults.textureTransforms != ZHLN::MaterialTextureTransforms {}) {
                return std::unexpected(GLTFImportError::ExtensionMismatch);
            }
            return {};
        }

        /**
         * Emission is a surface term, not a light source.
         *
         * glTF (and Babylon.js, which mirrors it) says an emissive material
         * shades itself; it does not illuminate its neighbours. This engine
         * shades it that way too -- material_model.slang samples the emissive
         * map times emissiveFactor and basic.slang adds it to the lit result,
         * where the bloom threshold pass picks up the overbright. So a spawned
         * neon model must glow with no LightComponent anywhere in the scene.
         *
         * SpawnParams::emissiveVirtualLights opts into an extra approximate
         * bounce light per emissive part. When it is on, that light is a child
         * of the part entity holding a *local* offset, so it inherits the
         * part's world transform. It used to be an unparented entity holding a
         * world position baked at spawn time, which meant every instance left
         * its lights pooled at the spawn point and went dark the moment it
         * moved -- that is what the second half of this test pins down.
         */
        std::expected<void, ZHLN::ErrorCode> emissive_lights_follow_the_prefab_they_belong_to() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless Emissive Spawn");
            if (engine == nullptr) {
                return std::unexpected(GLTFImportError::EngineInitFailed);
            }

            const std::vector<uint8_t> bytes  = MakeEmissiveStrengthFixture();
            const ZHLN::ModelPrefab*   prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), bytes, "emissive_spawn.glb");
            if (prefab == nullptr || prefab->parts.size() != 1) {
                return std::unexpected(GLTFImportError::PrefabLoadFailed);
            }

            auto& registry = engine->GetRegistry();

            const auto lightCount = [&registry] { return registry.GetEntitiesWith<ZHLN::Components::LightComponent>().size(); };

            const size_t lightsBefore = lightCount();

            // 1. The default spawn adds no lights: the glow comes from the
            //    material, exactly as it would in any other glTF viewer.
            std::array<ZHLN::Entity, 8>                  defaultEntities {};
            const ZHLN::PrefabFactory::SpawnParams defaultParams {.position = JPH::RVec3(0.0f, 0.0f, 0.0f)};
            const uint32_t                               defaultSpawned = ZHLN::PrefabFactory::InstantiatePrefab(
                *engine, *prefab, defaultParams, defaultEntities.data(), static_cast<uint32_t>(defaultEntities.size())
            );
            if (defaultSpawned == 0 || lightCount() != lightsBefore) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }

            // 2. Opting in adds exactly one light for the one emissive part.
            std::array<ZHLN::Entity, 8>                  entities {};
            const JPH::Vec3                              spawnPosition(4.0f, 1.0f, -2.0f);
            const ZHLN::PrefabFactory::SpawnParams params {.position = JPH::RVec3(spawnPosition), .emissiveVirtualLights = true};
            const uint32_t                               spawned =
                ZHLN::PrefabFactory::InstantiatePrefab(*engine, *prefab, params, entities.data(), static_cast<uint32_t>(entities.size()));
            if (spawned < 3 || lightCount() != lightsBefore + 1) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }

            // outBuffer order is root, part, glow.
            const ZHLN::Entity rootEntity = entities[0];
            const ZHLN::Entity partEntity = entities[1];
            const ZHLN::Entity glowEntity = entities[2];

            const auto* hierarchy = registry.Get<ZHLN::Components::HierarchyComponent>(glowEntity);
            if (hierarchy == nullptr || hierarchy->parent != partEntity) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }
            if (registry.Get<ZHLN::Components::LightComponent>(glowEntity) == nullptr) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }

            // The stored transform is a local offset. The triangle's bounds sit
            // within a unit box at the origin, so a spawn four metres away must
            // not show up in the light's own TransformComponent.
            const auto* glowLocal = registry.Get<ZHLN::Components::TransformComponent>(glowEntity);
            if (glowLocal == nullptr || glowLocal->position.Length() > 2.0f) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }
            const JPH::Vec3 localOffset = glowLocal->position;

            ZHLN::Test::Headless::TickFrames(*engine, 1);

            const auto* glowWorld = registry.Get<ZHLN::Components::WorldTransformComponent>(glowEntity);
            if (glowWorld == nullptr) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }
            const JPH::Vec3 restingPosition = glowWorld->world.GetTranslation();
            if (!restingPosition.IsClose(spawnPosition + localOffset, 0.001f)) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
            }

            // 3. Move the prefab root; the light has to move with it by the same
            //    delta rather than staying behind at the spawn point.
            const JPH::Vec3 delta(10.0f, 0.0f, 7.0f);
            registry.Patch<ZHLN::Components::TransformComponent>(rootEntity, [&delta](auto& transform) { transform.position += delta; });

            ZHLN::Test::Headless::TickFrames(*engine, 1);

            const auto* movedWorld = registry.Get<ZHLN::Components::WorldTransformComponent>(glowEntity);
            if (movedWorld == nullptr || !movedWorld->world.GetTranslation().IsClose(restingPosition + delta, 0.001f)) {
                return std::unexpected(GLTFImportError::EmissiveLightMismatch);
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
