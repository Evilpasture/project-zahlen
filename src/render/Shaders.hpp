// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/Shaders.hpp
//
// The renderer's shader modules, as types, and the sets one descriptor block
// serves.
//
// A program states two things and inherits the rest: the cooked SPIR-V it was
// compiled to (`#embed`, so a constant expression can walk it) and the source
// path a hot reload would reread. Its stage, its entry point, every binding it
// declares and whether each of those is a sampler are read back out of the
// bytes by ShaderProgram.hpp, so nothing in this file can drift from the shader
// it names -- a rebuild of the shader is a rebuild of the model of it.
//
// A set (`Vk::ShaderSet<...>`) is what a descriptor write names: the modules
// whose bindings that one block serves. Lighting compiles twice (RT and NoRT),
// SMAA three times (EDGE, WEIGHT, BLEND) and the bake block is shared by four
// modules; a write site names the set, the set walks its modules, and a name no
// module declares -- or a binding a module declares and the write forgets -- is
// a compile error that says which.
//
// Include this where a set is named, not from a header every render source
// parses: the catalog is ~550 KiB of bytes, and `Bytes()` is constant-expression
// data only in the translation unit that has the `#embed`. A TU that names a set
// pays for the bytes of the modules that set contains, once, and the linker folds
// the rest -- a program the TU never uses is never materialized.

#pragma once

#include "Rendering.hpp" // the RHI implementation headers: Vk::ShaderSet, Vk::StageOf
#include "pipeline/ShaderProgram.hpp" // Vk::ShaderProgram, Vk::ShaderSet

#include <cstdint>
#include <span>

namespace ZHLN {
namespace Shaders {

// ============================================================================
// The modules
// ============================================================================
//
// These are the modules themselves, not the sets: a set named after a pass
// (Culling) is a different declaration from the module it wraps, and keeping
// the two in separate namespaces is what lets a write site name exactly one of
// them.

namespace Modules {

/// hiz_generate.slang: the depth pyramid's two ends.
struct HizGenerate {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_HIZ_GENERATE_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_HIZ_GENERATE_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// culling.slang: the instance cull, its indirect commands and its HiZ chain.
struct Culling {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_CULLING_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_CULLING_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// cluster_bounds.slang: tightens each cluster's bounds for the next frame.
struct ClusterBounds {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_CLUSTER_BOUNDS_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_CLUSTER_BOUNDS_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// cluster_culling.slang: light-cluster assignment.
struct ClusterCulling {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_CLUSTER_CULLING_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_CLUSTER_CULLING_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// procedural_bake.slang: the authored-texture bake, one variant per look.
struct ProceduralBake {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// brdf_lut.slang: the split-sum BRDF integration LUT.
struct BrdfLut {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_BRDF_LUT_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_BRDF_LUT_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// ibl_specular.slang: the specular prefilter chain, one dispatch per mip.
struct IblSpecular {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_IBL_SPECULAR_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_IBL_SPECULAR_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// smaa_lut.slang: the SMAA area and search textures.
struct SmaaLut {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_SMAA_LUT_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_SMAA_LUT_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// volumetric_clear.slang: the voxel clear that precedes the injection.
struct VolumetricClear {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// volumetric_fog_inject.slang.
struct VolumetricFogInject {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// volumetric_light_inject.slang.
struct VolumetricLightInject {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// volumetric_integration.slang.
struct VolumetricIntegration {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// volumetric_temporal.slang.
struct VolumetricTemporal {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_VOLUMETRIC_TEMPORAL_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_VOLUMETRIC_TEMPORAL_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// bloom_threshold_cs.slang: the bright pass that starts the bloom chain.
struct BloomThreshold {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_BLOOM_THRESHOLD_CS_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_BLOOM_THRESHOLD_CS_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// bloom_down_cs.slang: the dual-Kawase downsample.
struct BloomDown {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_BLOOM_DOWN_CS_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_BLOOM_DOWN_CS_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// bloom_up_cs.slang: the dual-Kawase upsample.
struct BloomUp {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_BLOOM_UP_CS_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_BLOOM_UP_CS_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// hdr_denoise_atrous.slang: the HDR A-Trous wavelet denoiser.
struct HdrDenoiseAtrous {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_HDR_DENOISE_ATROUS_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_HDR_DENOISE_ATROUS_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// rtr_half.slang: the half-resolution ray-traced reflection source.
struct RtrHalf {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_RTR_HALF_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_RTR_HALF_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// ao_gtao.slang: the ground-truth ambient occlusion pass.
struct Gtao {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_AO_GTAO_SLANG_CS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_AO_GTAO_SLANG_CS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// TAA.slang's accumulation pass.
struct Taa {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_TAA_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_TAA_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// FXAA.slang.
struct Fxaa {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_FXAA_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_FXAA_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// MLAA.slang.
struct Mlaa {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_MLAA_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_MLAA_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// SMAA.slang with the EDGE define.
struct SmaaEdge {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_SMAA_EDGE_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_SMAA_EDGE_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// SMAA.slang with the WEIGHT define.
struct SmaaWeight {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_SMAA_WEIGHT_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// SMAA.slang with the BLEND define.
struct SmaaBlend {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_SMAA_BLEND_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_SMAA_BLEND_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// blit.slang: the presentation blit, which also composites the bloom target.
struct Blit {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_BLIT_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_BLIT_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// lighting.slang, ray-tracing configuration: the deferred resolve.
struct Lighting {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_LIGHTING_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_LIGHTING_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// lighting.slang without DISABLE_RTR: the same block, RTR bindings stripped.
struct LightingNoRt {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_LIGHTING_NORT_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_LIGHTING_NORT_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// reflection.slang, ray-tracing configuration.
struct Reflection {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_REFLECTION_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_REFLECTION_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

/// reflection.slang without DISABLE_RTR.
struct ReflectionNoRt {
    /// Where a hot reload rereads it; the bytes below are the same file, cooked.
    static constexpr const char* Path = SHADER_REFLECTION_NORT_SLANG_PS_PATH;
    static constexpr uint8_t     kBytes[] = {
#embed SHADER_REFLECTION_NORT_SLANG_PS_PATH
    };
    static constexpr auto Bytes() noexcept -> std::span<const uint8_t> {
        return kBytes;
    }
};

} // namespace Modules

// ============================================================================
// The sets a descriptor block is written through
// ============================================================================

/// hiz_generate.comp.
using Hiz = Vk::ShaderSet<Modules::HizGenerate>;

/// culling.comp.
using Culling = Vk::ShaderSet<Modules::Culling>;

/// cluster_bounds.comp.
using ClusterBounds = Vk::ShaderSet<Modules::ClusterBounds>;

/// cluster_culling.comp.
using ClusterCulling = Vk::ShaderSet<Modules::ClusterCulling>;

/// the baked-LUT block: one storage image, written by the procedural bake, the BRDF LUT, the specular prefilter and the SMAA LUT.
using Bake = Vk::ShaderSet<Modules::ProceduralBake, Modules::BrdfLut, Modules::IblSpecular, Modules::SmaaLut>;

/// volumetric_clear.comp.
using VolumetricClear = Vk::ShaderSet<Modules::VolumetricClear>;

/// volumetric_fog_inject.comp.
using VolumetricFogInject = Vk::ShaderSet<Modules::VolumetricFogInject>;

/// volumetric_light_inject.comp.
using VolumetricLightInject = Vk::ShaderSet<Modules::VolumetricLightInject>;

/// volumetric_integration.comp.
using VolumetricIntegration = Vk::ShaderSet<Modules::VolumetricIntegration>;

/// volumetric_temporal.comp.
using VolumetricTemporal = Vk::ShaderSet<Modules::VolumetricTemporal>;

/// bloom_threshold_cs.slang.
using BloomThreshold = Vk::ShaderSet<Modules::BloomThreshold>;

/// bloom_down_cs.slang.
using BloomDown = Vk::ShaderSet<Modules::BloomDown>;

/// bloom_up_cs.slang.
using BloomUp = Vk::ShaderSet<Modules::BloomUp>;

/// hdr_denoise_atrous.slang.
using HdrDenoise = Vk::ShaderSet<Modules::HdrDenoiseAtrous>;

/// rtr_half.slang.
using RtrHalf = Vk::ShaderSet<Modules::RtrHalf>;

/// ao_gtao.slang.
using Gtao = Vk::ShaderSet<Modules::Gtao>;

/// TAA.slang's accumulation pass.
using Taa = Vk::ShaderSet<Modules::Taa>;

/// FXAA.slang.
using Fxaa = Vk::ShaderSet<Modules::Fxaa>;

/// MLAA.slang.
using Mlaa = Vk::ShaderSet<Modules::Mlaa>;

/// SMAA.slang with the EDGE define.
using SmaaEdge = Vk::ShaderSet<Modules::SmaaEdge>;

/// SMAA.slang with the WEIGHT define.
using SmaaWeight = Vk::ShaderSet<Modules::SmaaWeight>;

/// SMAA.slang with the BLEND define.
using SmaaBlend = Vk::ShaderSet<Modules::SmaaBlend>;

/// blit.slang.
using Blit = Vk::ShaderSet<Modules::Blit>;

/// lighting.slang's two configurations.
using Lighting = Vk::ShaderSet<Modules::Lighting, Modules::LightingNoRt>;

/// reflection.slang's two configurations.
using Reflection = Vk::ShaderSet<Modules::Reflection, Modules::ReflectionNoRt>;

} // namespace Shaders
} // namespace ZHLN
