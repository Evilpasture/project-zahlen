// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/ShaderBindingChecks.cpp
//
// The one translation unit that reads the compiled shaders, so that the write
// sites do not have to. It embeds every module a descriptor block covers, parses
// each one at compile time (Vk::SpirvBindings, src/vulkan/pipeline/
// SpirvBindings.hpp) and asserts that the block's declaration in
// ShaderBindings.hpp still says exactly what the module says -- every binding
// the module declares is named there with the matching kind, every name there is
// declared by one of the block's modules, and every name recorded as dropped is
// declared by none.
//
// Why this file exists at all: the write gates in SpirvBindings.hpp compare a
// call site against a hand-written declaration, which is only worth anything
// while the declaration is still the shader's own interface. A parameter renamed
// in Slang, or a binding added and never written, would otherwise pass every
// check the compiler can make. Here it fails the build, with the module's own
// bytes as the witness.
//
// On costs: the embeds are only in this file, and each module's parse is its own
// constant expression (the deepest module the engine ships, lighting.ps, needs
// ~328k constexpr steps against the 1,048,576 a compiler allows by default), so
// no other translation unit pays for either. Adding a shader means adding one
// block to ShaderBindings.hpp and its embed below; the coverage assertion at the
// bottom is what refuses to let that be forgotten.

#include "ShaderBindings.hpp"

#include <array>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace ZHLN {

namespace {

/// The compiled bytes of one module, as the SPIR-V words the parser walks.
///
/// `#embed` and not a file read: these are constant expressions, which is what
/// lets a typo in a binding name be a compile error rather than a debug session.
constexpr uint8_t kHizGenerate[] = {
#embed SHADER_HIZ_GENERATE_SLANG_CS_PATH
};

constexpr uint8_t kCulling[] = {
#embed SHADER_CULLING_SLANG_CS_PATH
};

constexpr uint8_t kClusterBounds[] = {
#embed SHADER_CLUSTER_BOUNDS_CS_PATH
};

constexpr uint8_t kClusterCulling[] = {
#embed SHADER_CLUSTER_CULLING_CS_PATH
};

// The bake block is shared by four modules: the procedural bake, the BRDF LUT,
// the IBL prefilter and the SMAA LUT all declare the same single storage image,
// so all four are checked against the one declaration.
constexpr uint8_t kProceduralBake[] = {
#embed SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH
};
constexpr uint8_t kBrdfLut[] = {
#embed SHADER_BRDF_LUT_CS_PATH
};
constexpr uint8_t kIblSpecular[] = {
#embed SHADER_IBL_SPECULAR_CS_PATH
};
constexpr uint8_t kSmaaLut[] = {
#embed SHADER_SMAA_LUT_CS_PATH
};

constexpr uint8_t kVolumetricClear[] = {
#embed SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH
};
constexpr uint8_t kVolumetricFogInject[] = {
#embed SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH
};
constexpr uint8_t kVolumetricLightInject[] = {
#embed SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH
};
constexpr uint8_t kVolumetricIntegration[] = {
#embed SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH
};
constexpr uint8_t kVolumetricTemporal[] = {
#embed SHADER_VOLUMETRIC_TEMPORAL_CS_PATH
};

constexpr uint8_t kBloomThreshold[] = {
#embed SHADER_BLOOM_THRESHOLD_CS_SLANG_CS_PATH
};
constexpr uint8_t kBloomDown[] = {
#embed SHADER_BLOOM_DOWN_CS_SLANG_CS_PATH
};
constexpr uint8_t kBloomUp[] = {
#embed SHADER_BLOOM_UP_CS_SLANG_CS_PATH
};
constexpr uint8_t kHdrDenoise[] = {
#embed SHADER_HDR_DENOISE_ATROUS_SLANG_CS_PATH
};
constexpr uint8_t kRtrHalf[] = {
#embed SHADER_RTR_HALF_SLANG_CS_PATH
};
constexpr uint8_t kGtao[] = {
#embed SHADER_AO_GTAO_SLANG_CS_PATH
};

constexpr uint8_t kTaa[] = {
#embed SHADER_TAA_SLANG_PS_PATH
};
constexpr uint8_t kFxaa[] = {
#embed SHADER_FXAA_SLANG_PS_PATH
};
constexpr uint8_t kMlaa[] = {
#embed SHADER_MLAA_SLANG_PS_PATH
};
// SMAA compiles three times from one source (EDGE, WEIGHT, BLEND), so three
// modules stand for three passes with three different interfaces.
constexpr uint8_t kSmaaEdge[] = {
#embed SHADER_SMAA_EDGE_PS_PATH
};
constexpr uint8_t kSmaaWeight[] = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
};
constexpr uint8_t kSmaaBlend[] = {
#embed SHADER_SMAA_BLEND_PS_PATH
};
constexpr uint8_t kBlit[] = {
#embed SHADER_BLIT_SLANG_PS_PATH
};

// The two lighting and two reflection configurations share one mapping table
// each, so both modules of a pair are checked against the one declaration.
constexpr uint8_t kLighting[] = {
#embed SHADER_LIGHTING_SLANG_PS_PATH
};
constexpr uint8_t kLightingNoRt[] = {
#embed SHADER_LIGHTING_NORT_SLANG_PS_PATH
};
constexpr uint8_t kReflection[] = {
#embed SHADER_REFLECTION_SLANG_PS_PATH
};
constexpr uint8_t kReflectionNoRt[] = {
#embed SHADER_REFLECTION_NORT_SLANG_PS_PATH
};

// One parse per module, never a shared initializer: a constant expression is
// where a compiler's step budget is spent, and the budget is per expression.

constexpr Vk::SpirvBindings kHizGenerateModule       = Vk::SpirvBindings::Parse(kHizGenerate, 0);
constexpr Vk::SpirvBindings kCullingModule           = Vk::SpirvBindings::Parse(kCulling, 0);
constexpr Vk::SpirvBindings kClusterBoundsModule     = Vk::SpirvBindings::Parse(kClusterBounds, 0);
constexpr Vk::SpirvBindings kClusterCullingModule    = Vk::SpirvBindings::Parse(kClusterCulling, 0);
constexpr Vk::SpirvBindings kProceduralBakeModule    = Vk::SpirvBindings::Parse(kProceduralBake, 0);
constexpr Vk::SpirvBindings kBrdfLutModule           = Vk::SpirvBindings::Parse(kBrdfLut, 0);
constexpr Vk::SpirvBindings kIblSpecularModule       = Vk::SpirvBindings::Parse(kIblSpecular, 0);
constexpr Vk::SpirvBindings kSmaaLutModule           = Vk::SpirvBindings::Parse(kSmaaLut, 0);
constexpr Vk::SpirvBindings kVolumetricClearModule   = Vk::SpirvBindings::Parse(kVolumetricClear, 0);
constexpr Vk::SpirvBindings kVolumetricFogModule     = Vk::SpirvBindings::Parse(kVolumetricFogInject, 0);
constexpr Vk::SpirvBindings kVolumetricLightModule   = Vk::SpirvBindings::Parse(kVolumetricLightInject, 0);
constexpr Vk::SpirvBindings kVolumetricIntegrateModule = Vk::SpirvBindings::Parse(kVolumetricIntegration, 0);
constexpr Vk::SpirvBindings kVolumetricTemporalModule  = Vk::SpirvBindings::Parse(kVolumetricTemporal, 0);
constexpr Vk::SpirvBindings kBloomThresholdModule    = Vk::SpirvBindings::Parse(kBloomThreshold, 0);
constexpr Vk::SpirvBindings kBloomDownModule         = Vk::SpirvBindings::Parse(kBloomDown, 0);
constexpr Vk::SpirvBindings kBloomUpModule           = Vk::SpirvBindings::Parse(kBloomUp, 0);
constexpr Vk::SpirvBindings kHdrDenoiseModule        = Vk::SpirvBindings::Parse(kHdrDenoise, 0);
constexpr Vk::SpirvBindings kRtrHalfModule           = Vk::SpirvBindings::Parse(kRtrHalf, 0);
constexpr Vk::SpirvBindings kGtaoModule              = Vk::SpirvBindings::Parse(kGtao, 0);
constexpr Vk::SpirvBindings kTaaModule               = Vk::SpirvBindings::Parse(kTaa, 0);
constexpr Vk::SpirvBindings kFxaaModule              = Vk::SpirvBindings::Parse(kFxaa, 0);
constexpr Vk::SpirvBindings kMlaaModule              = Vk::SpirvBindings::Parse(kMlaa, 0);
constexpr Vk::SpirvBindings kSmaaEdgeModule          = Vk::SpirvBindings::Parse(kSmaaEdge, 0);
constexpr Vk::SpirvBindings kSmaaWeightModule        = Vk::SpirvBindings::Parse(kSmaaWeight, 0);
constexpr Vk::SpirvBindings kSmaaBlendModule         = Vk::SpirvBindings::Parse(kSmaaBlend, 0);
constexpr Vk::SpirvBindings kBlitModule              = Vk::SpirvBindings::Parse(kBlit, 0);
constexpr Vk::SpirvBindings kLightingModule          = Vk::SpirvBindings::Parse(kLighting, 0);
constexpr Vk::SpirvBindings kLightingNoRtModule      = Vk::SpirvBindings::Parse(kLightingNoRt, 0);
constexpr Vk::SpirvBindings kReflectionModule        = Vk::SpirvBindings::Parse(kReflection, 0);
constexpr Vk::SpirvBindings kReflectionNoRtModule    = Vk::SpirvBindings::Parse(kReflectionNoRt, 0);

/// The blocks checked below. `Bindings::All` is asserted to be a subset: a block
/// declared but not verified here would be a hand-written interface with nothing
/// behind it, which is the exact thing these checks exist to remove.
using CheckedBlocks = std::tuple<
    Bindings::Hiz, Bindings::Culling, Bindings::ClusterBounds, Bindings::ClusterCulling, Bindings::Bake, Bindings::VolumetricClear,
    Bindings::VolumetricFogInject, Bindings::VolumetricLightInject, Bindings::VolumetricIntegration, Bindings::VolumetricTemporal,
    Bindings::BloomThreshold, Bindings::BloomDown, Bindings::BloomUp, Bindings::HdrDenoise, Bindings::RtrHalf, Bindings::Gtao, Bindings::Taa,
    Bindings::Fxaa, Bindings::Mlaa, Bindings::SmaaEdge, Bindings::SmaaWeight, Bindings::SmaaBlend, Bindings::Blit, Bindings::Lighting,
    Bindings::Reflection>;

template <typename Block, typename... Checked>
[[nodiscard]] consteval auto IsChecked() noexcept -> bool {
    return (std::is_same_v<Block, Checked> || ...);
}

[[nodiscard]] consteval auto EveryBlockIsChecked() noexcept -> bool {
    return []<typename... Checked>(std::tuple<Checked...>* = nullptr) consteval -> bool {
        return []<typename... Blocks>(std::tuple<Blocks...>* = nullptr) consteval -> bool {
            return (IsChecked<Blocks, Checked...>() && ...);
        }(static_cast<Bindings::All*>(nullptr));
    }(static_cast<CheckedBlocks*>(nullptr));
}

} // namespace

// ============================================================================
// Every module against the block that writes its descriptors
// ============================================================================
// Each assertion reads as the question it answers: are these modules and this
// declaration saying the same thing? Slang drops what a configuration does not
// reference, so a pair like the lighting modules is not two identical sets but
// two module states of one table -- the check is per module for that reason.

static_assert(Vk::ModulesMatchDeclarations<Bindings::Hiz>(std::array {kHizGenerateModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Culling>(std::array {kCullingModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::ClusterBounds>(std::array {kClusterBoundsModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::ClusterCulling>(std::array {kClusterCullingModule}));
static_assert(
    Vk::ModulesMatchDeclarations<Bindings::Bake>(std::array {kProceduralBakeModule, kBrdfLutModule, kIblSpecularModule, kSmaaLutModule})
);
static_assert(Vk::ModulesMatchDeclarations<Bindings::VolumetricClear>(std::array {kVolumetricClearModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::VolumetricFogInject>(std::array {kVolumetricFogModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::VolumetricLightInject>(std::array {kVolumetricLightModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::VolumetricIntegration>(std::array {kVolumetricIntegrateModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::VolumetricTemporal>(std::array {kVolumetricTemporalModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::BloomThreshold>(std::array {kBloomThresholdModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::BloomDown>(std::array {kBloomDownModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::BloomUp>(std::array {kBloomUpModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::HdrDenoise>(std::array {kHdrDenoiseModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::RtrHalf>(std::array {kRtrHalfModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Gtao>(std::array {kGtaoModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Taa>(std::array {kTaaModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Fxaa>(std::array {kFxaaModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Mlaa>(std::array {kMlaaModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::SmaaEdge>(std::array {kSmaaEdgeModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::SmaaWeight>(std::array {kSmaaWeightModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::SmaaBlend>(std::array {kSmaaBlendModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Blit>(std::array {kBlitModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Lighting>(std::array {kLightingModule, kLightingNoRtModule}));
static_assert(Vk::ModulesMatchDeclarations<Bindings::Reflection>(std::array {kReflectionModule, kReflectionNoRtModule}));

static_assert(EveryBlockIsChecked(), "a descriptor block in ShaderBindings.hpp has no module check here");

} // namespace ZHLN
