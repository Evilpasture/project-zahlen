// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <map>
#include <tuple>
#include <type_traits>
#include <utility>
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

namespace detail {

template <typename Arg>
void WriteReflectedBinding(
    VkDescriptorSet                               set,
    uint32_t                                      binding,
    VkDescriptorType                              descriptorType,
    const Arg&                                    arg,
    VkDescriptorImageInfo&                        imageInfo,
    VkDescriptorBufferInfo&                       bufferInfo,
    VkWriteDescriptorSetAccelerationStructureKHR& asInfo,
    VkWriteDescriptorSet&                         write
) noexcept {
    using T = std::remove_cvref_t<Arg>;

    if constexpr (std::is_same_v<T, SkipWrite>) {
        write.descriptorCount = 0;
        return;
    }

    write = {
        .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext            = nullptr,
        .dstSet           = set,
        .dstBinding       = binding,
        .dstArrayElement  = 0,
        .descriptorCount  = 1,
        .descriptorType   = descriptorType,
        .pImageInfo       = nullptr,
        .pBufferInfo      = nullptr,
        .pTexelBufferView = nullptr,
    };

    if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        VkImageView   view   = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if constexpr (std::is_same_v<T, ImageWrite>) {
            view   = arg.view;
            layout = arg.layout;
        } else if constexpr (IsTypedImage<T>::value) {
            view   = arg.view;
            layout = T::layout;
        } else if constexpr (requires { arg.view.Get(); }) {
            view   = arg.view.Get();
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else if constexpr (requires {
                                 { arg.Get() } -> std::convertible_to<VkImageView>;
                             }) {
            view   = arg.Get();
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else if constexpr (std::is_same_v<T, VkImageView>) {
            view   = arg;
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkSampler sampler = VK_NULL_HANDLE;
        if (descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            if constexpr (std::is_same_v<T, SamplerWrite>) {
                sampler = arg.sampler;
            } else if constexpr (requires {
                                     { arg.Get() } -> std::convertible_to<VkSampler>;
                                 }) {
                sampler = arg.Get();
            } else if constexpr (std::is_same_v<T, VkSampler>) {
                sampler = arg;
            }
        }

        imageInfo        = {.sampler = sampler, .imageView = view, .imageLayout = layout};
        write.pImageInfo = &imageInfo;

    } else if (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        VkImageView view = VK_NULL_HANDLE;
        if constexpr (std::is_same_v<T, ImageWrite> || IsTypedImage<T>::value) {
            view = arg.view;
        } else if constexpr (requires { arg.view.Get(); }) {
            view = arg.view.Get();
        } else if constexpr (requires {
                                 { arg.Get() } -> std::convertible_to<VkImageView>;
                             }) {
            view = arg.Get();
        } else if constexpr (std::is_same_v<T, VkImageView>) {
            view = arg;
        }

        imageInfo        = {.sampler = VK_NULL_HANDLE, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        write.pImageInfo = &imageInfo;

    } else if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
        VkSampler sampler = VK_NULL_HANDLE;
        if constexpr (std::is_same_v<T, SamplerWrite>) {
            sampler = arg.sampler;
        } else if constexpr (requires {
                                 { arg.Get() } -> std::convertible_to<VkSampler>;
                             }) {
            sampler = arg.Get();
        } else if constexpr (std::is_same_v<T, VkSampler>) {
            sampler = arg;
        }

        imageInfo        = {.sampler = sampler, .imageView = VK_NULL_HANDLE, .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        write.pImageInfo = &imageInfo;

    } else if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range  = VK_WHOLE_SIZE;

        if constexpr (std::is_same_v<T, BufferWrite>) {
            buffer = arg.buffer;
            offset = arg.offset;
            range  = arg.range;
        } else if constexpr (requires { arg.Handle(); }) {
            buffer = arg.Handle();
        } else if constexpr (std::is_same_v<T, VkBuffer>) {
            buffer = arg;
        }

        bufferInfo        = {.buffer = buffer, .offset = offset, .range = range};
        write.pBufferInfo = &bufferInfo;

    } else if (descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        if constexpr (
            std::is_pointer_v<T> &&
            (std::is_same_v<std::remove_pointer_t<T>, const VkAccelerationStructureKHR> || std::is_same_v<std::remove_pointer_t<T>, VkAccelerationStructureKHR>)
        ) {
            if (arg == nullptr || *arg == VK_NULL_HANDLE) {
                write.descriptorCount = 0;
            } else {
                asInfo = {
                    .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                    .pNext                      = nullptr,
                    .accelerationStructureCount = 1,
                    .pAccelerationStructures    = arg
                };
                write.pNext = &asInfo;
            }
        } else {
            write.descriptorCount = 0;
        }
    } else {
        write.descriptorCount = 0;
    }
}

} // namespace detail

/**
 * @brief Holds fully automated, RAII-managed layout handles generated via shader reflection.
 * @note [UNSAFE] This struct is populated by parsing untrusted SPIR-V bytecode at runtime.
 * Incorrect layout assumptions here can lead to undefined behavior, driver hangs, or GPU crashes.
 */
struct UnsafeReflectedLayout {
    PipelineLayout                     pipelineLayout;
    std::array<DescriptorSetLayout, 4> descriptorSetLayouts;
    uint32_t                           setLayoutCount = 0;

    // Tracks the exact count of each descriptor type needed by all sets combined.
    // Sparse: extension descriptor types (e.g. VK_DESCRIPTOR_TYPE_ACCELERATION_
    // STRUCTURE_KHR = 1000150000, used by Slang's RaytracingAccelerationStructure)
    // are far outside the core 0..INPUT_ATTACHMENT enum range, so indexing a
    // fixed array by raw VkDescriptorType overflows it.
    std::map<VkDescriptorType, uint32_t> descriptorTypeCounts {};
    std::array<ReflectedSet, 4>                                   reflectedSets {};

    /**
     * @brief Unsafely fetches a raw layout handle.
     * @warning Caller must guarantee setIndex is within shader layout bounds.
     */
    [[nodiscard]] auto GetSetLayoutUnsafe(uint32_t setIndex = 0) const noexcept -> VkDescriptorSetLayout {
        return setIndex < setLayoutCount ? descriptorSetLayouts[setIndex].Get() : VK_NULL_HANDLE;
    }

    [[nodiscard]] auto GetSetLayout(uint32_t setIndex = 0) const noexcept -> VkDescriptorSetLayout {
        return GetSetLayoutUnsafe(setIndex);
    }

    [[nodiscard]] auto CreateLayout(VkDevice device = VK_NULL_HANDLE) const noexcept -> VkDescriptorSetLayout;

    [[nodiscard]] auto CreatePool(VkDevice device, uint32_t maxSets) const noexcept -> DescriptorPool;

    [[nodiscard]] static auto Allocate(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) noexcept -> VkDescriptorSet;

    template <typename... Args>
    void Write(VkDevice device, VkDescriptorSet set, Args&&... args) const noexcept {
        const auto                                                        arg_tuple = std::forward_as_tuple(std::forward<Args>(args)...);
        constexpr size_t                                                  k_count   = sizeof...(Args);
        std::array<VkDescriptorImageInfo, k_count>                        image_infos {};
        std::array<VkDescriptorBufferInfo, k_count>                       buffer_infos {};
        std::array<VkWriteDescriptorSetAccelerationStructureKHR, k_count> as_infos {};
        std::array<VkWriteDescriptorSet, k_count>                         writes {};

        [&]<size_t... I>(std::index_sequence<I...>) {
            (detail::WriteReflectedBinding(
                 set, I < reflectedSets[0].bindings.size() ? reflectedSets[0].bindings[I].binding : 0,
                 I < reflectedSets[0].bindings.size() ? reflectedSets[0].bindings[I].descriptorType : VK_DESCRIPTOR_TYPE_MAX_ENUM, std::get<I>(arg_tuple),
                 image_infos[I], buffer_infos[I], as_infos[I], writes[I]
             ),
             ...);
        }(std::make_index_sequence<k_count> {});

        std::array<VkWriteDescriptorSet, k_count> valid_writes {};
        uint32_t                                  valid_count = 0;
        for (uint32_t i = 0; i < k_count; ++i) {
            if (writes[i].descriptorCount > 0) {
                valid_writes[valid_count++] = writes[i];
            }
        }
        if (valid_count > 0) {
            vkUpdateDescriptorSets(device, valid_count, valid_writes.data(), 0, nullptr);
        }
    }

    bool Build(VkDevice device, const ShaderStages& shaders) noexcept;
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
