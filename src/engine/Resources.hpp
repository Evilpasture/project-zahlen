// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <span>

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
namespace ZHLN::Resource {

struct ShaderPair {
    std::span<const std::uint8_t> vertex;
    std::span<const std::uint8_t> fragment;
};

[[nodiscard]] constexpr ShaderPair GetBasicProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/basic.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/basic.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetBlitProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/blit.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/blit.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetTaaProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/taa.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/taa.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetUiProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/ui.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/ui.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetAmbientProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/ambient.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/ambient.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetLightingProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/lighting.hlsl.VSMain.LIGHTING_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/lighting.hlsl.PSMain.LIGHTING_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetReflectionProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/reflection.hlsl.VSMain.REFLECTION_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/reflection.hlsl.PSMain.REFLECTION_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetReflectionNortProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/reflection.hlsl.VSMain.REFLECTION_NORT_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/reflection.hlsl.PSMain.REFLECTION_NORT_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetFxaaProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/fxaa.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/fxaa.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetSmaaEdgeProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/smaa_wrap.hlsl.SmaaEdgeVS.SMAA_EDGE_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/smaa_wrap.hlsl.SmaaEdgePS.SMAA_EDGE_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetSmaaWeightProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/smaa_wrap.hlsl.SmaaWeightVS.SMAA_WEIGHT_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/smaa_wrap.hlsl.SmaaWeightPS.SMAA_WEIGHT_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetSmaaBlendProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/smaa_wrap.hlsl.SmaaBlendVS.SMAA_BLEND_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/smaa_wrap.hlsl.SmaaBlendPS.SMAA_BLEND_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetBloomThresholdProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/bloom_threshold.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/bloom_threshold.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetBloomBlurProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/bloom_blur.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/bloom_blur.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetPunctualShadowsProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/punctual_shadows.hlsl.VS.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/punctual_shadows.hlsl.PS.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

[[nodiscard]] constexpr ShaderPair GetLightingNortProgram() noexcept {
    static constexpr const std::uint8_t vtx[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/lighting.hlsl.VSMain.LIGHTING_NORT_VS_SPV.spv"
};
    static constexpr const std::uint8_t frag[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/lighting.hlsl.PSMain.LIGHTING_NORT_PS_SPV.spv"
};
    return {.vertex = vtx, .fragment = frag};
}

// --- STANDALONE RESOURCING ---

[[nodiscard]] constexpr std::span<const std::uint8_t> GetCullingCompSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/culling.hlsl.CSMain.CULLING_COMP_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetShadowFragSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/basic.hlsl.PSShadow.SHADOW_FRAG_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetClusterBoundsSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/cluster_bounds.hlsl.CSMain.CLUSTER_BOUNDS_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetClusterCullingSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/cluster_culling.hlsl.CSMain.CLUSTER_CULLING_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetSkinningCompSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/skinning.hlsl.CSMain.SKINNING_COMP_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetForwardFragSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/basic.hlsl.PSForward.FORWARD_FRAG_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetHangGpuCompSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/hang_gpu.hlsl.CSMain.HANG_GPU_COMP_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetProceduralBakeCompSpv() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "/home/Enwifi/projects/project-zahlen/build/generated_shaders/procedural_bake.hlsl.CSMain.PROCEDURAL_BAKE_CS_SPV.spv"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetLtcMatBin() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "../../resources/shaders/ltc_mat.dds"
};
    return kData;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetLtcAmpBin() noexcept {
    static constexpr const std::uint8_t kData[] = {
    #embed "../../resources/shaders/ltc_amp.dds"
};
    return kData;
}

} // namespace ZHLN::Resource

// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

