// tools/zcook/GLBModel.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// tools/zcook/GLBModel.hpp
//
// The reflected glTF 2.0 document model the GLB emitter serialises with
// ZHLN::ReflectJSON::SerializeJSON. The struct declarations are the schema:
// field names are the glTF keys, so nothing here hand-builds a brace, a comma
// or an escape -- see the emitter that fills them in GLB.cpp.
//
// glTF is optional-by-omission, so a field that may be absent is a
// std::optional (or an empty container) and the document is serialised with
// ZHLN::ReflectJSON::Options{.omitEmpty = true}: a disengaged optional or an
// empty map/range member is not written at all, because cgltf would read
// "mesh": null as mesh 0 rather than "no mesh".
//
// Field order inside each struct is the key order the emitter wants; JSON
// object order is insignificant to the spec but keeps diffs against the old
// hand-built strings mechanical.
//
// The model is a tools/ private: the runtime importer
// (extras/glTF/GLTFImporter.cpp) decodes with cgltf and never sees these
// types. Tests that synthesize their own documents intentionally keep their
// own fixture structs (tests/render/TestGLTFImport.cpp) so the importer's
// contract is asserted independently of the emitter's implementation.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::GLB {

struct GlbAsset {
    std::string_view version   = "2.0";
    std::string_view generator = "Zahlen GLB Emitter";
};

struct GlbBufferView {
    uint32_t                buffer     = 0;
    uint32_t                byteOffset = 0;
    uint32_t                byteLength = 0;
    std::optional<uint32_t> target; // ARRAY_BUFFER / ELEMENT_ARRAY_BUFFER; absent for images, morphs, IBM, animation
};

struct GlbAccessor {
    uint32_t                bufferView = 0;
    std::optional<uint32_t> byteOffset; // only the shared per-primitive index accessors carry one
    uint32_t                componentType = 0;
    uint32_t                count         = 0;
    std::string_view        type;        // SCALAR / VEC2 / VEC3 / VEC4 / MAT4
    std::vector<float>      min;         // position / morph / animation-input accessors only
    std::vector<float>      max;
    std::optional<bool>     normalized;  // emitted true for UNORM8 color/weight accessors
};

struct GlbTextureRef {
    uint32_t index = 0;
};

struct GlbPbrMetallicRoughness {
    std::array<float, 4>   baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    float                  metallicFactor  = 0.0f;
    float                  roughnessFactor = 1.0f;
    std::optional<GlbTextureRef> baseColorTexture;
};

struct GlbProceduralShader {
    std::string_view                  type; // "NODE_GRAPH" bakes to a texture; anything else keeps parameters
    std::map<std::string, std::vector<float>> parameters;
};

struct GlbEmissiveStrength {
    float emissiveStrength = 1.0f;
};

struct GlbMaterialExtensions {
    std::optional<GlbProceduralShader>  ZHLN_procedural_shader;
    std::optional<GlbEmissiveStrength>  KHR_materials_emissive_strength;
};

struct GlbMaterial {
    std::string_view                name;
    GlbPbrMetallicRoughness         pbrMetallicRoughness;
    std::optional<std::string_view> alphaMode;   // "BLEND" when baseColor alpha < 1
    std::optional<bool>             doubleSided; // emitted true only
    std::optional<GlbTextureRef>    normalTexture;
    std::optional<GlbTextureRef>    metallicRoughnessTexture;
    std::optional<std::array<float, 3>> emissiveFactor;
    std::optional<GlbTextureRef>    emissiveTexture;
    std::optional<GlbMaterialExtensions> extensions;
};

struct GlbPrimitiveAttributes {
    uint32_t                POSITION = 0;
    uint32_t                NORMAL   = 0;
    uint32_t                TANGENT  = 0;
    uint32_t                TEXCOORD_0 = 0;
    uint32_t                COLOR_0    = 0;
    std::optional<uint32_t> JOINTS_0;  // skinned meshes only
    std::optional<uint32_t> WEIGHTS_0; // skinned meshes only
};

struct GlbMorphTarget {
    uint32_t POSITION = 0;
};

struct GlbPrimitive {
    GlbPrimitiveAttributes     attributes;
    std::optional<uint32_t>    indices;
    std::optional<uint32_t>    material;
    std::vector<GlbMorphTarget> targets; // empty = no shape keys
};

struct GlbMesh {
    std::string_view          name;
    std::vector<GlbPrimitive> primitives;
};

struct GlbTexture {
    uint32_t sampler = 0;
    uint32_t source  = 0;
};

struct GlbImage {
    uint32_t         bufferView = 0;
    std::string_view mimeType; // image/png | image/jpeg
};

struct GlbSampler {
    uint32_t magFilter = 9729;
    uint32_t minFilter = 9729;
    uint32_t wrapS     = 10497;
    uint32_t wrapT     = 10497;
};

struct GlbSkin {
    std::string_view        name;
    uint32_t                inverseBindMatrices = 0;
    std::vector<uint32_t>   joints;
};

struct GlbAnimChannelTarget {
    uint32_t         node = 0;
    std::string_view path; // translation | rotation | scale | weights
};

struct GlbAnimChannel {
    uint32_t              sampler = 0;
    GlbAnimChannelTarget  target;
};

struct GlbAnimSampler {
    uint32_t         input = 0;
    std::string_view interpolation = "LINEAR";
    uint32_t         output = 0;
};

struct GlbAnimation {
    std::string_view           name;
    std::vector<GlbAnimChannel> channels;
    std::vector<GlbAnimSampler> samplers;
};

struct GlbNodeExtensions {
    struct KhrLightRef {
        uint32_t light = 0;
    };
    KhrLightRef KHR_lights_punctual;
};

struct GlbNode {
    std::string_view            name;
    std::array<float, 16>       matrix; // local for parented nodes, world for roots
    std::optional<uint32_t>     mesh;
    std::optional<uint32_t>     skin;
    std::vector<uint32_t>       children;
    std::vector<float>          weights; // per morph target, all 0 after cooking
    std::optional<GlbNodeExtensions> extensions;
};

struct GlbScene {
    std::vector<uint32_t> nodes;
};

struct GlbKhrLight {
    std::string_view      name;
    std::string_view      type; // point | spot | directional
    std::array<float, 3>  color {1.0f, 1.0f, 1.0f};
    float                 intensity = 1.0f;
};

struct GlbKhrLightsPunctual {
    std::vector<GlbKhrLight> lights;
};

struct GlbRootExtensions {
    GlbKhrLightsPunctual KHR_lights_punctual;
};

struct GlbBuffer {
    uint32_t byteLength = 0;
};

struct GlbDocument {
    GlbAsset                            asset;
    std::vector<std::string_view>       extensionsUsed;
    std::optional<GlbRootExtensions>    extensions;
    std::vector<GlbBufferView>          bufferViews;
    std::vector<GlbAccessor>            accessors;
    std::vector<GlbMaterial>            materials;
    std::vector<GlbTexture>             textures;
    std::vector<GlbImage>               images;
    std::vector<GlbSampler>             samplers;
    std::vector<GlbMesh>                meshes;
    std::vector<GlbSkin>                skins;
    std::vector<GlbAnimation>           animations;
    std::vector<GlbNode>                nodes;
    std::vector<GlbScene>               scenes;
    uint32_t                            scene = 0;
    std::vector<GlbBuffer>              buffers;
};

} // namespace ZHLN::GLB
