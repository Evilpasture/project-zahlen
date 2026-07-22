// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <span>

namespace ZHLN::Resource {

struct ShaderPair {
    std::span<const uint8_t> vertex;
    std::span<const uint8_t> fragment;
};

enum class ShaderID : uint8_t {
    Basic,
    Blit,
    Taa,
    Ui,
    Ambient,
    Lighting,
    Reflection,
    ReflectionNort,
    Fxaa,
    Mlaa,
    SmaaEdge,
    SmaaWeight,
    SmaaBlend,
    BloomThreshold,
    BloomBlur,
    PunctualShadows,
    LightingNort,
    CullingComp,
    ShadowFrag,
    ClusterBounds,
    ClusterCulling,
    SkinningComp,
    ForwardFrag,
    HangGpuComp,
    ProceduralBakeComp,
    LtcMat,
    LtcAmp,
    VolumetricInjection,
    VolumetricScattering,
    VolumetricIntegration,
    ParticleUpdate,
    ParticleRender,
};

// Extern declarations of individual programs to avoid header bloat and allow compile-time routing
extern const ShaderPair basic_shaders;
extern const ShaderPair blit_shaders;
extern const ShaderPair taa_shaders;
extern const ShaderPair ui_shaders;
extern const ShaderPair ambient_shaders;
extern const ShaderPair lighting_shaders;
extern const ShaderPair reflection_shaders;
extern const ShaderPair reflection_nort_shaders;
extern const ShaderPair fxaa_shaders;
extern const ShaderPair mlaa_shaders;
extern const ShaderPair smaa_edge_shaders;
extern const ShaderPair smaa_weight_shaders;
extern const ShaderPair smaa_blend_shaders;
extern const ShaderPair bloom_threshold_shaders;
extern const ShaderPair bloom_blur_shaders;
extern const ShaderPair punctual_shadows_shaders;
extern const ShaderPair lighting_nort_shaders;
extern const ShaderPair volumetric_injection_shaders;
extern const ShaderPair volumetric_scattering_shaders;
extern const ShaderPair volumetric_integration_shaders;
extern const ShaderPair particle_update_shaders;
extern const ShaderPair particle_render_shaders;

// Extern declarations for single spans
extern const std::span<const uint8_t> culling_comp;
extern const std::span<const uint8_t> shadow_frag;
extern const std::span<const uint8_t> cluster_bounds;
extern const std::span<const uint8_t> cluster_culling;
extern const std::span<const uint8_t> skinning_comp;
extern const std::span<const uint8_t> forward_frag;
extern const std::span<const uint8_t> hang_gpu_comp;
extern const std::span<const uint8_t> procedural_bake_comp;
extern const std::span<const uint8_t> ltc_mat;
extern const std::span<const uint8_t> ltc_amp;

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
        {.id = ShaderID::Ambient, .pair = ambient_shaders},
        {.id = ShaderID::Lighting, .pair = lighting_shaders},
        {.id = ShaderID::Reflection, .pair = reflection_shaders},
        {.id = ShaderID::ReflectionNort, .pair = reflection_nort_shaders},
        {.id = ShaderID::Fxaa, .pair = fxaa_shaders},
        {.id = ShaderID::Mlaa, .pair = mlaa_shaders},
        {.id = ShaderID::SmaaEdge, .pair = smaa_edge_shaders},
        {.id = ShaderID::SmaaWeight, .pair = smaa_weight_shaders},
        {.id = ShaderID::SmaaBlend, .pair = smaa_blend_shaders},
        {.id = ShaderID::BloomThreshold, .pair = bloom_threshold_shaders},
        {.id = ShaderID::BloomBlur, .pair = bloom_blur_shaders},
        {.id = ShaderID::PunctualShadows, .pair = punctual_shadows_shaders},
        {.id = ShaderID::LightingNort, .pair = lighting_nort_shaders},
        {.id = ShaderID::CullingComp, .pair = ShaderPair {.vertex = culling_comp, .fragment = {}}},
        {.id = ShaderID::ShadowFrag, .pair = ShaderPair {.vertex = shadow_frag, .fragment = {}}},
        {.id = ShaderID::ClusterBounds, .pair = ShaderPair {.vertex = cluster_bounds, .fragment = {}}},
        {.id = ShaderID::ClusterCulling, .pair = ShaderPair {.vertex = cluster_culling, .fragment = {}}},
        {.id = ShaderID::SkinningComp, .pair = ShaderPair {.vertex = skinning_comp, .fragment = {}}},
        {.id = ShaderID::ForwardFrag, .pair = ShaderPair {.vertex = forward_frag, .fragment = {}}},
        {.id = ShaderID::HangGpuComp, .pair = ShaderPair {.vertex = hang_gpu_comp, .fragment = {}}},
        {.id = ShaderID::ProceduralBakeComp, .pair = ShaderPair {.vertex = procedural_bake_comp, .fragment = {}}},
        {.id = ShaderID::LtcMat, .pair = ShaderPair {.vertex = ltc_mat, .fragment = {}}},
        {.id = ShaderID::LtcAmp, .pair = ShaderPair {.vertex = ltc_amp, .fragment = {}}},
        {.id = ShaderID::VolumetricInjection, .pair = volumetric_injection_shaders},
        {.id = ShaderID::VolumetricScattering, .pair = volumetric_scattering_shaders},
        {.id = ShaderID::VolumetricIntegration, .pair = volumetric_integration_shaders},
        ShaderMapping {.id = ShaderID::ParticleUpdate, .pair = particle_update_shaders},
        ShaderMapping {.id = ShaderID::ParticleRender, .pair = particle_render_shaders},
    };

    for (const auto& mapping: table) {
        if (mapping.id == id) {
            return mapping.pair;
        }
    }

    return {};
}

} // namespace ZHLN::Resource
