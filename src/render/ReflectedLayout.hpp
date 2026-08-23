// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ReflectedLayout.hpp
//
// SPIRV-Reflect binding reflection for the VK_EXT_descriptor_heap model.
// There are no descriptor sets anymore: reflection only produces the
// set/binding structure (types, counts, stages) that the engine bakes into
// VkDescriptorSetAndBindingMappingEXT tables (see HeapBindings.hpp).

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ZHLN::Vk {

class ShaderStages;

struct ReflectedBinding {
    uint32_t                 binding         = 0;
    VkDescriptorType         descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t                 descriptorCount = 1;
    VkShaderStageFlags       stageFlags      = 0;
    VkDescriptorBindingFlags bindingFlags    = 0;
};

struct ReflectedSet {
    std::vector<ReflectedBinding> bindings;
};

/// A single SPIR-V blob + stage to reflect over (union across stages).
struct ReflectedStageInput {
    ZHLN_ShaderDesc       shader;
    VkShaderStageFlagBits stage;
};

/**
 * Reflects the LocalSize execution mode emitted by Slang for a compute entry
 * point. This is the compiled form of `[numthreads(x, y, z)]` and is therefore
 * the dispatch-layout authority for the host.
 */
[[nodiscard]] auto ReflectComputeThreadGroupSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>>;

/**
 * Reflects an optional fixed logical dispatch domain declared by the shader
 * through Zahlen's reserved specialization-constant metadata IDs. Dynamic
 * kernels omit this metadata and receive their domain from the caller.
 */
[[nodiscard]] auto ReflectComputeDispatchSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>>;

/**
 * @brief Standalone SPIR-V parser that extracts binding structure only.
 * @note [UNSAFE] Populated by parsing untrusted SPIR-V bytecode at runtime.
 */
class UnsafeReflectedLayoutBuilder {
  public:
    UnsafeReflectedLayoutBuilder() noexcept = default;

    UnsafeReflectedLayoutBuilder(UnsafeReflectedLayoutBuilder&&)                 = delete;
    UnsafeReflectedLayoutBuilder& operator=(UnsafeReflectedLayoutBuilder&&)      = delete;
    UnsafeReflectedLayoutBuilder(const UnsafeReflectedLayoutBuilder&)            = delete;
    UnsafeReflectedLayoutBuilder& operator=(const UnsafeReflectedLayoutBuilder&) = delete;
    ~UnsafeReflectedLayoutBuilder() noexcept                                     = default;

    /// Registers one shader stage for reflection.
    void AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept;

    /// Reflects all registered stages into `out` (up to 4 sets). Returns false
    /// when nothing usable was found.
    [[nodiscard]] auto BuildUnsafe(std::array<ReflectedSet, 4>& out) noexcept -> bool;

  private:
    struct StageData {
        const uint32_t*    code  = nullptr;
        size_t             size  = 0;
        VkShaderStageFlags stage = 0;
    };
    std::array<StageData, 8> _stages {};
    uint32_t                 _stageCount = 0;
};

} // namespace ZHLN::Vk
