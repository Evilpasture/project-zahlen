// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <span>

namespace ZHLN::Resource {

namespace Paths {
extern const char* const BasicVS;
extern const char* const BasicPS;
extern const char* const BasicTask;        // VK_EXT_mesh_shader amplification stage
extern const char* const BasicMesh;        // VK_EXT_mesh_shader mesh stage
extern const char* const BasicVSShadow;    // -DZHLN_PASS_SHADOW variant
extern const char* const BasicMeshShadow;  // -DZHLN_PASS_SHADOW variant
extern const char* const BasicVSForward;   // -DFORWARD_PASS variant
extern const char* const BasicMeshForward; // -DFORWARD_PASS variant
extern const char* const BlitVS;
extern const char* const BlitPS;
extern const char* const TaaVS;
extern const char* const TaaPS;
extern const char* const UiVS;
extern const char* const UiPS;
extern const char* const LightingVS;
extern const char* const LightingPS;
extern const char* const ReflectionVS;
extern const char* const ReflectionPS;
extern const char* const ReflectionNortVS;
extern const char* const ReflectionNortPS;
extern const char* const FxaaVS;
extern const char* const FxaaPS;
extern const char* const MlaaVS;
extern const char* const MlaaPS;
extern const char* const SmaaEdgeVS;
extern const char* const SmaaEdgePS;
extern const char* const SmaaWeightVS;
extern const char* const SmaaWeightPS;
extern const char* const SmaaBlendVS;
extern const char* const SmaaBlendPS;
extern const char* const BloomThresholdCS;
extern const char* const BloomDownCS;
extern const char* const BloomUpCS;
extern const char* const PunctualShadowsVS;
extern const char* const PunctualShadowsPS;
extern const char* const LightingNortVS;
extern const char* const LightingNortPS;
extern const char* const VolumetricClearCS;
extern const char* const VolumetricFogInjectCS;
extern const char* const VolumetricLightInjectCS;
extern const char* const VolumetricIntegrationCS;
extern const char* const VolumetricTemporalCS;
extern const char* const ParticleUpdateCS;
extern const char* const ParticleRenderVS;
extern const char* const ParticleRenderPS;
extern const char* const DecalVS;
extern const char* const DecalPS;
extern const char* const MeshParticleUpdateCS;
extern const char* const MeshParticleRenderVS;
extern const char* const MeshParticleRenderPS;
extern const char* const MeshParticleShadowVS;
extern const char* const MeshParticleShadowPS;
extern const char* const CullingCS;
extern const char* const HizGenerateCS;
extern const char* const ShadowPS;
extern const char* const ClusterBoundsCS;
extern const char* const ClusterCullingCS;
extern const char* const SkinningCS;
extern const char* const ForwardPS;
extern const char* const HangGpuCS;
extern const char* const ProceduralBakeCS;
extern const char* const BRDFLUTCS;
extern const char* const IBLSpecularCS;
extern const char* const IBLSHCS;
extern const char* const SMAALUTCS;
extern const char* const GPUSceneCS;
extern const char* const GPUABICS;
} // namespace Paths

struct ShaderPair {
    std::span<const uint8_t> vertex;
    std::span<const uint8_t> fragment;
};

enum class ShaderID : uint8_t {
    Basic,
    Blit,
    Taa,
    Ui,
    Lighting,
    Reflection,
    ReflectionNort,
    Fxaa,
    Mlaa,
    SmaaEdge,
    SmaaWeight,
    SmaaBlend,
    BloomThresholdCS,
    BloomDownCS,
    BloomUpCS,
    PunctualShadows,
    LightingNort,
    CullingComp,
    HizGenerateComp,
    ShadowFrag,
    ClusterBounds,
    ClusterCulling,
    SkinningComp,
    ForwardFrag,
    HangGpuComp,
    ProceduralBakeComp,
    LtcMat,
    LtcAmp,
    VolumetricClear,
    VolumetricFogInject,
    VolumetricLightInject,
    VolumetricIntegration,
    VolumetricTemporal,
    VolumetricInjection,  // Legacy alias
    VolumetricScattering, // Legacy alias
    ParticleUpdate,
    ParticleRender,
    Decal,
    MeshParticleUpdate,
    MeshParticleRender,
    MeshParticleShadow,
    BRDFLUTComp,
    IBLSpecularComp,
    IBLSHComp,
    SMAALUTComp,
    GPUSceneComp,
    GPUABIComp,
};

// Extern declarations of individual programs to avoid header bloat and allow compile-time routing
extern const ShaderPair basic_shaders;
extern const ShaderPair blit_shaders;
extern const ShaderPair taa_shaders;
extern const ShaderPair ui_shaders;
extern const ShaderPair lighting_shaders;
extern const ShaderPair reflection_shaders;
extern const ShaderPair reflection_nort_shaders;
extern const ShaderPair fxaa_shaders;
extern const ShaderPair mlaa_shaders;
extern const ShaderPair smaa_edge_shaders;
extern const ShaderPair smaa_weight_shaders;
extern const ShaderPair smaa_blend_shaders;
extern const ShaderPair punctual_shadows_shaders;
extern const ShaderPair lighting_nort_shaders;
extern const ShaderPair volumetric_clear_shaders;
extern const ShaderPair volumetric_fog_inject_shaders;
extern const ShaderPair volumetric_light_inject_shaders;
extern const ShaderPair volumetric_integration_shaders;
extern const ShaderPair volumetric_temporal_shaders;
extern const ShaderPair particle_update_shaders;
extern const ShaderPair particle_render_shaders;
extern const ShaderPair decal_shaders;
extern const ShaderPair mesh_particle_update_shaders;
extern const ShaderPair mesh_particle_render_shaders;
extern const ShaderPair mesh_particle_shadow_shaders;

// Extern declarations for single spans
extern const std::span<const uint8_t> culling_comp;
extern const std::span<const uint8_t> hiz_generate_comp;
extern const std::span<const uint8_t> shadow_frag;
extern const std::span<const uint8_t> cluster_bounds;
extern const std::span<const uint8_t> cluster_culling;
extern const std::span<const uint8_t> skinning_comp;
extern const std::span<const uint8_t> forward_frag;
// Scene geometry stages. Never pair these by hand -- use GetSceneShaders().
extern const std::span<const uint8_t> basic_task;
extern const std::span<const uint8_t> basic_mesh;
extern const std::span<const uint8_t> basic_vs_shadow;
extern const std::span<const uint8_t> basic_mesh_shadow;
extern const std::span<const uint8_t> basic_vs_forward;
extern const std::span<const uint8_t> basic_mesh_forward;
extern const std::span<const uint8_t> hang_gpu_comp;
extern const std::span<const uint8_t> procedural_bake_comp;
extern const std::span<const uint8_t> bloom_threshold_cs;
extern const std::span<const uint8_t> bloom_down_cs;
extern const std::span<const uint8_t> bloom_up_cs;
extern const std::span<const uint8_t> brdf_lut_comp;
extern const std::span<const uint8_t> ibl_specular_comp;
extern const std::span<const uint8_t> ibl_sh_comp;
extern const std::span<const uint8_t> smaa_lut_comp;
extern const std::span<const uint8_t> gpu_scene_comp;
extern const std::span<const uint8_t> gpu_abi_comp;
extern const std::span<const uint8_t> ltc_mat;
extern const std::span<const uint8_t> ltc_amp;

/// Which specialisation of the scene geometry interface a pipeline needs.
/// basic.slang / basic_mesh.slang emit a different varying set per variant, so
/// the geometry and fragment stages must come from the SAME variant.
enum class SceneShaderVariant : uint8_t {
    GBuffer, ///< MainPass1/2 + CSG: full interface (PSMain)
    Shadow,  ///< Depth-only cascades: no motion vectors, no normal frame (PSShadow)
    Forward, ///< Translucent/lines: no motion vectors, no normal frame (PSForward)
};

struct SceneShaderSet {
    std::span<const uint8_t> vertex;
    std::span<const uint8_t> fragment;
    std::span<const uint8_t> task; ///< VK_EXT_mesh_shader (variant-independent)
    std::span<const uint8_t> mesh; ///< VK_EXT_mesh_shader
};

/// The single place that pairs a geometry stage with its fragment stage.
/// Mixing variants produces mismatched varying locations, which the validation
/// layer reports as a SPIR-V interface error at pipeline creation.
[[nodiscard]] SceneShaderSet GetSceneShaders(SceneShaderVariant variant) noexcept;

struct ShaderMapping {
    ShaderID   id;
    ShaderPair pair;
};

[[nodiscard]] constexpr ShaderPair GetShaderProgram(ShaderID id) noexcept {
    static const ShaderMapping table[] = {
        ShaderMapping {.id = ShaderID::Basic, .pair = basic_shaders},
        {.id = ShaderID::Blit, .pair = blit_shaders},
        {.id = ShaderID::Taa, .pair = taa_shaders},
        {.id = ShaderID::Ui, .pair = ui_shaders},
        {.id = ShaderID::Lighting, .pair = lighting_shaders},
        {.id = ShaderID::Reflection, .pair = reflection_shaders},
        {.id = ShaderID::ReflectionNort, .pair = reflection_nort_shaders},
        {.id = ShaderID::Fxaa, .pair = fxaa_shaders},
        {.id = ShaderID::Mlaa, .pair = mlaa_shaders},
        {.id = ShaderID::SmaaEdge, .pair = smaa_edge_shaders},
        {.id = ShaderID::SmaaWeight, .pair = smaa_weight_shaders},
        {.id = ShaderID::SmaaBlend, .pair = smaa_blend_shaders},
        {.id = ShaderID::PunctualShadows, .pair = punctual_shadows_shaders},
        {.id = ShaderID::LightingNort, .pair = lighting_nort_shaders},
        {.id = ShaderID::CullingComp, .pair = ShaderPair {.vertex = culling_comp, .fragment = {}}},
        {.id = ShaderID::HizGenerateComp, .pair = ShaderPair {.vertex = hiz_generate_comp, .fragment = {}}},
        {.id = ShaderID::ShadowFrag, .pair = ShaderPair {.vertex = shadow_frag, .fragment = {}}},
        {.id = ShaderID::ClusterBounds, .pair = ShaderPair {.vertex = cluster_bounds, .fragment = {}}},
        {.id = ShaderID::ClusterCulling, .pair = ShaderPair {.vertex = cluster_culling, .fragment = {}}},
        {.id = ShaderID::SkinningComp, .pair = ShaderPair {.vertex = skinning_comp, .fragment = {}}},
        {.id = ShaderID::ForwardFrag, .pair = ShaderPair {.vertex = forward_frag, .fragment = {}}},
        {.id = ShaderID::HangGpuComp, .pair = ShaderPair {.vertex = hang_gpu_comp, .fragment = {}}},
        {.id = ShaderID::ProceduralBakeComp, .pair = ShaderPair {.vertex = procedural_bake_comp, .fragment = {}}},
        {.id = ShaderID::LtcMat, .pair = ShaderPair {.vertex = ltc_mat, .fragment = {}}},
        {.id = ShaderID::LtcAmp, .pair = ShaderPair {.vertex = ltc_amp, .fragment = {}}},
        {.id = ShaderID::BloomThresholdCS, .pair = ShaderPair {.vertex = bloom_threshold_cs, .fragment = {}}},
        {.id = ShaderID::BloomDownCS, .pair = ShaderPair {.vertex = bloom_down_cs, .fragment = {}}},
        {.id = ShaderID::BloomUpCS, .pair = ShaderPair {.vertex = bloom_up_cs, .fragment = {}}},
        {.id = ShaderID::VolumetricClear, .pair = volumetric_clear_shaders},
        {.id = ShaderID::VolumetricFogInject, .pair = volumetric_fog_inject_shaders},
        {.id = ShaderID::VolumetricLightInject, .pair = volumetric_light_inject_shaders},
        {.id = ShaderID::VolumetricIntegration, .pair = volumetric_integration_shaders},
        {.id = ShaderID::VolumetricTemporal, .pair = volumetric_temporal_shaders},
        ShaderMapping {.id = ShaderID::ParticleUpdate, .pair = particle_update_shaders},
        ShaderMapping {.id = ShaderID::ParticleRender, .pair = particle_render_shaders},
        ShaderMapping {.id = ShaderID::Decal, .pair = decal_shaders},
        ShaderMapping {.id = ShaderID::MeshParticleUpdate, .pair = mesh_particle_update_shaders},
        ShaderMapping {.id = ShaderID::MeshParticleRender, .pair = mesh_particle_render_shaders},
        ShaderMapping {.id = ShaderID::MeshParticleShadow, .pair = mesh_particle_shadow_shaders},
        {.id = ShaderID::BRDFLUTComp, .pair = ShaderPair {.vertex = brdf_lut_comp, .fragment = {}}},
        {.id = ShaderID::IBLSpecularComp, .pair = ShaderPair {.vertex = ibl_specular_comp, .fragment = {}}},
        {.id = ShaderID::IBLSHComp, .pair = ShaderPair {.vertex = ibl_sh_comp, .fragment = {}}},
        {.id = ShaderID::SMAALUTComp, .pair = ShaderPair {.vertex = smaa_lut_comp, .fragment = {}}},
        {.id = ShaderID::GPUSceneComp, .pair = ShaderPair {.vertex = gpu_scene_comp, .fragment = {}}},
        {.id = ShaderID::GPUABIComp, .pair = ShaderPair {.vertex = gpu_abi_comp, .fragment = {}}},
    };

    for (const auto& mapping: table) {
        if (mapping.id == id) {
            return mapping.pair;
        }
    }

    return {};
}

} // namespace ZHLN::Resource
