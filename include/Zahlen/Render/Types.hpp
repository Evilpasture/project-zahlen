// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/Types.hpp
//
// The descriptor structs a caller fills in to ask the renderer for something: a
// material recipe, one draw, a CSG draw, a decal. Pure data -- every handle and
// enum in them comes from <Zahlen/Types.hpp>, which is why this header is cheap
// to include on its own.
//
// Not the engine-wide umbrella of the same name: this is the renderer's half.
#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <cstdint>

namespace ZHLN {

// Material recipe for RenderContext::CreateMaterial: pipeline-state flags
// plus the PBR factors and texture bindings of one scene material.
struct MaterialDesc {
    // Pipeline configuration
    bool doubleSided   = false;
    bool alphaBlend    = false;
    bool additiveBlend = false;

    // PBR factors (using std::array eliminates memcpy)
    uint32_t             alphaMode   = 0;
    float                alphaCutoff = 0.5f;
    float                metallic    = 1.0f;
    float                roughness   = 1.0f;
    std::array<float, 4> baseColor   = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissive    = {0.0f, 0.0f, 0.0f, 1.0f};

    // Texture bindings
    TextureHandle albedoMap   = TextureHandle::Invalid;
    TextureHandle normalMap   = TextureHandle::Invalid;
    TextureHandle pbrMap      = TextureHandle::Invalid;
    TextureHandle emissiveMap = TextureHandle::Invalid;
};

struct DrawParams {
    JPH::Mat44           transform        = JPH::Mat44::sIdentity();
    JPH::Mat44           prevTransform    = JPH::Mat44::sIdentity();
    float                cullRadius       = 1.0f;
    std::array<float, 3> localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t             jointOffset      = 0;
    uint32_t             morphOffset      = 0;
    uint32_t             activeMorphCount = 0;
    const float*         morphWeights     = nullptr;
    DrawFlags            flags            = DrawFlags::None;

    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    std::array<float, 4> colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f}; // alpha < 0 means disable override
    std::array<float, 4> emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f}; // alpha < 0 means disable override
};

struct CSGCutterParams {
    Mesh         mesh;
    Material     material;
    JPH::Mat44   transform           = JPH::Mat44::sIdentity();
    JPH::Mat44   prevTransform       = JPH::Mat44::sIdentity();
    float        cullRadius          = 1.0f;
    CSGOperation operation           = CSGOperation::Difference;
    uint32_t     jointOffset         = 0;
    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;
    DrawFlags    flags               = DrawFlags::None;
};

struct CSGDrawParams {
    DrawParams                   eyeParams;
    ZHLN::Array<CSGCutterParams> cutters; // Stably using your custom Array container
};

struct DecalParams {
    JPH::Mat44    transform    = JPH::Mat44::sIdentity();
    JPH::Mat44    invTransform = JPH::Mat44::sIdentity();
    TextureHandle albedoMap    = TextureHandle::Invalid;
    TextureHandle normalMap    = TextureHandle::Invalid;
    float         roughness    = 0.5f;
    float         metallic     = 0.0f;
};

} // namespace ZHLN
