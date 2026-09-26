// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/PipelineDesc.hpp
//
// What it takes to compile a material's pipelines, and the G-buffer layout
// those pipelines write. Lifted out of RenderInternal.hpp for the same reason
// the draw payloads were: PipelineRegistry compiles materials and cannot
// include the renderer's private header without knowing the context exists.
//
// Nothing here is generated. `ZHLN_ShaderDesc` is the RHI's own C struct
// (src/vulkan/core/RenderCore.h) -- bytes, size and entry point travelling
// together. Only the shader *catalog* that fills those descriptors in is
// generated (ShaderBindings.hpp), and that stays on the caller's side of this
// boundary: the registry is handed a description and never looks up a module.

#pragma once
#include "Rendering.hpp"
#include <cstdint>

namespace ZHLN {

// Shader-blob recipe for a material's graphics pipelines. Internal: public callers go
// through RenderContext::CreateMaterial(MaterialDesc); this raw form exists only to
// compile the engine's built-in scene shaders.
struct PipelineDesc {
    // Every stage arrives as a descriptor built from a generated module
    // (<ShaderBindings.hpp>), so bytes and entry point travel together. Which geometry
    // module pairs with which fragment module is the variant's business (see
    // GetSceneShaders in RenderResources.cpp) -- mixing variants mismatches varyings.
    ZHLN_ShaderDesc vertexShader;
    ZHLN_ShaderDesc fragShader;

    // VK_EXT_mesh_shader: optional task/mesh stages. When the device supports mesh
    // shading and `meshShader` is set, the material gets a SECOND pipeline from
    // task+mesh+fragment; the vertex pipeline is always built too, so the renderer can
    // fall back per draw call (skinned meshes, no meshlet streams, no support).
    ZHLN_ShaderDesc taskShader;
    ZHLN_ShaderDesc meshShader;
    bool            doubleSided   = false;
    bool            alphaBlend    = false;
    bool            additiveBlend = false; // Support for emissive particles
    bool            isLineList    = false;
    // Transmission composites a finished color and must occlude the far shell.
    // Ordinary alpha blend keeps this false.
    bool            depthWrite    = false;
};

using ActiveGBuffer = Vk::GBufferLayout<
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>, // Index 0: sceneColor
    Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>,           // Index 1: velocityBuffer
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>,          // Index 2: normalRoughnessBuffer
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>  // Index 3: emissiveBuffer
    >;

} // namespace ZHLN
