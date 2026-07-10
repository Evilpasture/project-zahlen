// src/zcook/IR.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Compiler {

struct IRBuffer {
    uint32_t byteOffset = 0;
    uint32_t byteLength = 0;
};

struct IRPrimitive {
    std::string materialId;
    uint32_t    vertexOffset;
    uint32_t    vertexCount;
};

struct IRMorphTarget {
    std::string name;
    std::string binFile;
    uint32_t    byteOffset = 0;
    uint32_t    byteLength = 0;
};

struct IRMesh {
    std::string                id, layout, binFile;
    IRBuffer                   vertexBuffer;
    std::vector<IRPrimitive>   primitives;
    std::vector<IRMorphTarget> morphTargets;
};

struct IRNode {
    std::string id, meshId, lightId, skinId, parentId;
    float       localMatrix[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    float       worldMatrix[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    bool        visible         = true;
};

struct IRLight {
    std::string id, type;
    float       color[3]  = {1.f, 1.f, 1.f};
    float       intensity = 1.0f;
};

struct IRProceduralParam {
    std::string        name;
    std::vector<float> values;
};

struct IRProcedural {
    bool                           active = false;
    std::string                    type;
    std::vector<IRProceduralParam> parameters;
};

struct IRMaterial {
    std::string  id, albedoMap, normalMap, metallicRoughnessMap, emissiveMap;
    float        baseColor[4] = {1.f, 1.f, 1.f, 1.f};
    float        metallic = 0.f, roughness = 0.5f;
    float        emissiveFactor[3] = {0.f, 0.f, 0.f};
    float        emissiveStrength  = 1.0f;
    bool         doubleSided       = false;
    IRProcedural procedural;
};

struct IRAnimationSampler {
    std::string interpolation;
    uint32_t    inputOffset  = 0;
    uint32_t    inputLength  = 0;
    uint32_t    outputOffset = 0;
    uint32_t    outputLength = 0;
    std::string binFile;
};

struct IRAnimationChannel {
    std::string targetNodeId;
    std::string targetPath;
    uint32_t    samplerId = 0;
};

struct IRAnimation {
    std::string                     id;
    std::string                     name;
    float                           duration = 0.0f;
    bool                            loop     = false;
    std::vector<IRAnimationChannel> channels;
    std::vector<IRAnimationSampler> samplers;
};

struct IRSkin {
    std::string              id;
    std::string              name;
    std::vector<std::string> joints;
    std::vector<std::string> parents;
    std::vector<float>       inverseBindMatrices;
    std::vector<float>       restPose;
};

struct IRManifest {
    std::string              levelName;
    std::vector<IRMesh>      meshes;
    std::vector<IRNode>      nodes;
    std::vector<IRLight>     lights;
    std::vector<IRMaterial>  materials;
    std::vector<IRAnimation> animations;
    std::vector<IRSkin>      skins;
};

} // namespace Compiler
