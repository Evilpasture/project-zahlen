// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/ReflectedLayout.hpp
//
// SPIRV-Reflect binding reflection for the VK_EXT_descriptor_heap model, and
// the container it fills. There are no descriptor sets anymore: reflection only
// produces the set/binding structure (types, counts, stages) that the engine
// bakes into VkDescriptorSetAndBindingMappingEXT tables (see HeapBindings.hpp).
//
// One header for both halves on purpose. ReflectedLayout is what a pass holds
// and reads (BuildHeapPassBindings walks `sets`); ReflectedLayoutBuilder is the
// parser that fills it. This used to be two headers, and each carried its own
// binding/set pair -- byte-identical structs that differed only in name -- so
// every build ended in a field-by-field copy from one pair to the other. The
// builder now hands back the same ReflectedSet the passes read, and the copy is
// a move.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ZHLN::Vk {

class ShaderStages;

struct ReflectedBinding {
    uint32_t                 binding         = 0;
    VkDescriptorType         descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t                 descriptorCount = 1;
    VkShaderStageFlags       stageFlags      = 0;
    VkDescriptorBindingFlags bindingFlags    = 0;
    // The variable's identifier in the shader, owned here because the
    // reflection module is destroyed once the layout is built. HeapPassBindings
    // matches Vk::Slot names against it, which is what makes a descriptor write
    // independent of argument order and of bindings a configuration drops.
    std::string name;
};

struct ReflectedSet {
    std::vector<ReflectedBinding> bindings;
};

// A single SPIR-V blob + stage to reflect over (union across stages).
struct ReflectedStageInput {
    ZHLN_ShaderDesc       shader;
    VkShaderStageFlagBits stage;
};

/**
 * @brief Binding structure reflected from compiled SPIR-V.
 *
 * In the descriptor-heap model this carries NO descriptor set layouts, pools,
 * or pipeline layouts: the binding structure (set/binding/type/count/stages)
 * is what the engine bakes into its VkDescriptorSetAndBindingMappingEXT
 * tables (see HeapBindings.hpp).
 */
struct ReflectedLayout {
    std::array<ReflectedSet, 4> sets {};

    // True when `binding` is present in the given reflected set.
    [[nodiscard]] auto HasBinding(uint32_t setIndex, uint32_t binding) const noexcept -> bool {
        if (setIndex >= 4) {
            return false;
        }
        for (const auto& b: sets[setIndex].bindings) {
            if (b.binding == binding) {
                return true;
            }
        }
        return false;
    }

    bool Build(VkDevice device, const ShaderStages& shaders) noexcept;

    // Reflects a single stage (commonly a compute shader described by a raw SPV blob).
    bool Build(VkDevice device, const ZHLN_ShaderDesc& shader, VkShaderStageFlagBits stage) noexcept;

    // Reflects the union of an arbitrary set of stages (e.g. the bindless scene registry).
    bool Build(VkDevice device, std::span<const ReflectedStageInput> stages) noexcept;
};

/**
 * Reflects the LocalSize execution mode emitted by Slang for a compute entry
 * point. This is the compiled form of `[numthreads(x, y, z)]` and is therefore
 * the dispatch-layout authority for the host.
 */
[[nodiscard]] auto ReflectComputeThreadGroupSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>>;

/**
 * Reflects an optional fixed logical dispatch domain declared by the shader
 * as `namespace Dispatch { SizeX/Y/Z }` (SPIR-V spec-constant IDs 1000-1002).
 * Dynamic kernels omit this metadata and receive their domain from the caller.
 */
[[nodiscard]] auto ReflectComputeDispatchSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>>;

// Reflects a u32 / f32 specialization constant default by SPIR-V ID.
[[nodiscard]] auto ReflectSpecializationConstantU32(const ZHLN_ShaderDesc& shader, uint32_t constantId) noexcept -> std::optional<uint32_t>;
[[nodiscard]] auto ReflectSpecializationConstantF32(const ZHLN_ShaderDesc& shader, uint32_t constantId) noexcept -> std::optional<float>;

/**
 * @brief Standalone SPIR-V parser that extracts binding structure only.
 *
 * Both operations carry the Unsafe marker on purpose: AddStageUnsafe stores
 * raw pointers into the caller's SPIR-V without copying or validating it
 * (lifetime is the caller's problem), and BuildUnsafe parses untrusted
 * bytecode at runtime, skipping any stage SPIRV-Reflect refuses instead of
 * reporting it.
 */
class ReflectedLayoutBuilder {
  public:
    ReflectedLayoutBuilder() noexcept = default;

    ReflectedLayoutBuilder(ReflectedLayoutBuilder&&)                 = delete;
    ReflectedLayoutBuilder& operator=(ReflectedLayoutBuilder&&)      = delete;
    ReflectedLayoutBuilder(const ReflectedLayoutBuilder&)            = delete;
    ReflectedLayoutBuilder& operator=(const ReflectedLayoutBuilder&) = delete;
    ~ReflectedLayoutBuilder() noexcept                               = default;

    // Registers one shader stage for reflection.
    void AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept;

    // Reflects all registered stages into `out` (up to 4 sets). Returns false
    // when nothing usable was found.
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
