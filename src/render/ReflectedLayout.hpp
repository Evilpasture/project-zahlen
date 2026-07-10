// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {
/**
 * @brief Holds fully automated, RAII-managed layout handles generated via shader reflection.
 * @note [UNSAFE] This struct is populated by parsing untrusted SPIR-V bytecode at runtime.
 * Incorrect layout assumptions here can lead to undefined behavior, driver hangs, or GPU crashes.
 */
struct UnsafeReflectedLayout {
    PipelineLayout                     pipelineLayout;
    std::array<DescriptorSetLayout, 4> descriptorSetLayouts;
    uint32_t                           setLayoutCount = 0;

    // Tracks the exact count of each descriptor type needed by all sets combined
    std::array<uint32_t, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1> descriptorTypeCounts {};

    /**
     * @brief Unsafely fetches a raw layout handle.
     * @warning Caller must guarantee setIndex is within shader layout bounds.
     */
    [[nodiscard]] auto GetSetLayoutUnsafe(uint32_t setIndex = 0) const noexcept -> VkDescriptorSetLayout {
        return setIndex < setLayoutCount ? descriptorSetLayouts[setIndex].Get() : VK_NULL_HANDLE;
    }
};

/**
 * @brief Reflection builder that queries SPIR-V bytecode at runtime using SPIRV-Reflect.
 * @note [UNSAFE] Bypasses C++ compile-time type-safety guarantees. Relies entirely on
 * runtime binary parsing. Use only when static layouts cannot be predefined.
 */
class UnsafeReflectedLayoutBuilder {
  public:
    UnsafeReflectedLayoutBuilder() noexcept = default;

    // Non-movable, non-copyable
    UnsafeReflectedLayoutBuilder(UnsafeReflectedLayoutBuilder&&)                         = delete;
    UnsafeReflectedLayoutBuilder& operator=(UnsafeReflectedLayoutBuilder&&)              = delete;
    UnsafeReflectedLayoutBuilder(const UnsafeReflectedLayoutBuilder&)                    = delete;
    auto operator=(const UnsafeReflectedLayoutBuilder&) -> UnsafeReflectedLayoutBuilder& = delete;
    ~UnsafeReflectedLayoutBuilder() noexcept                                             = default;

    /**
     * @brief Adds a shader bytecode stage to the pipeline parsing queue.
     * @warning It is undefined behavior if desc contains malformed SPIR-V or invalid bytecode size.
     */
    void AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept;

    /**
     * @brief Unsafely parses all registered stages and builds the Vulkan layouts.
     * @throws Does not throw, but failure to match pipeline state object requirements
     * later down the line will cause hard validation layer errors.
     */
    [[nodiscard]] auto BuildUnsafe(VkDevice device) noexcept -> UnsafeReflectedLayout;

  private:
    struct StageData {
        const uint32_t*    code  = nullptr;
        size_t             size  = 0;
        VkShaderStageFlags stage = 0;
    };
    std::array<StageData, 5> _stages {};
    uint32_t                 _stageCount = 0;
};
} // namespace ZHLN::Vk
