// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct ComputePass {
    PipelineLayout        pipelineLayout;
    Pipeline              pipeline;
    std::vector<Pipeline> pipelines;

    [[nodiscard]] std::expected<void, ZHLN::Error> Build(
        VkDevice                   device,
        VkDescriptorSetLayout      descriptorLayout,
        const ZHLN_ShaderDesc&     shader,
        const VkPushConstantRange* pushConstants = nullptr,
        uint32_t                   pushCount     = 0
    ) noexcept;

    [[nodiscard]] std::expected<void, ZHLN::Error> BuildVariants(
        VkDevice                              device,
        VkDescriptorSetLayout                 descriptorLayout,
        const ZHLN_ShaderDesc&                shader,
        std::span<const VkSpecializationInfo> specInfos,
        const VkPushConstantRange*            pushConstants = nullptr,
        uint32_t                              pushCount     = 0
    ) noexcept;

    // --- Stateful Bind and Push Helpers ---

    void Bind(VkCommandBuffer cmd) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    }

    void BindSet(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t firstSet = 0) const noexcept {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), firstSet, 1, &set, 0, nullptr);
    }

    void BindVariant(VkCommandBuffer cmd, uint32_t variantIdx) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[variantIdx].Get());
    }

    template <GpuTriviallyCopyable T>
    void DispatchVariant(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t variantIdx, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        BindVariant(cmd, variantIdx);
        BindSet(cmd, set);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }

    template <GpuTriviallyCopyable T>
    void PushConstants(VkCommandBuffer cmd, const T& pushData) const noexcept {
        Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
    }

    static void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) noexcept {
        ZHLN_CmdDispatch(cmd, x, y, z);
    }

    // --- High-Level Convenience Dispatches ---

    // Dispatch with no Descriptor Set (e.g. BDA only, like Skinning)
    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }

    // Dispatch with Descriptor Set, but no Push Constants (like Clustering)
    void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t x, uint32_t y, uint32_t z) const noexcept {
        Bind(cmd);
        BindSet(cmd, set);
        Dispatch(cmd, x, y, z);
    }

    // Dispatch with both Descriptor Set & Push Constants
    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
        Bind(cmd);
        BindSet(cmd, set);
        PushConstants(cmd, pushData);
        Dispatch(cmd, x, y, z);
    }
};

template <typename LayoutT>
struct DoubleBufferedComputePass {
    DescriptorSetLayout                   descLayout;
    DescriptorPool                        pool;
    ZHLN::DoubleBuffered<VkDescriptorSet> sets;
    PipelineLayout                        pipelineLayout;
    Pipeline                              pipeline;

    [[nodiscard]] bool
        Build(VkDevice device, const ZHLN_ShaderDesc& shader, const VkPushConstantRange* pushConstants = nullptr, uint32_t pushCount = 0) noexcept;

    template <typename... Args>
    void WriteNext(VkDevice device, Args&&... args) const noexcept {
        LayoutT::Write(device, sets.Next(), std::forward<Args>(args)...);
    }

    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) const noexcept;

    template <GpuTriviallyCopyable T>
    void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept;

    void Flip() noexcept {
        sets.Flip();
    }
};

} // namespace ZHLN::Vk

#include "ComputePass.inl"
