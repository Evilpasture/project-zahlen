// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
namespace ZHLN::Vk {


struct DrawState {
    VkPipeline       pipeline      = VK_NULL_HANDLE;
    VkPipelineLayout layout        = VK_NULL_HANDLE;
    VkDescriptorSet  set           = VK_NULL_HANDLE;
    bool             heap          = false;
    uint32_t         vertexCount   = 0;
    uint32_t         indexCount    = 0;
    uint32_t         instanceCount = 1;
    uint32_t         firstVertex   = 0;
    uint32_t         firstIndex    = 0;
    uint32_t         firstInstance = 0;
};

struct MeshTaskState {
    VkPipeline       pipeline    = VK_NULL_HANDLE;
    VkPipelineLayout layout      = VK_NULL_HANDLE;
    VkDescriptorSet  set         = VK_NULL_HANDLE;
    bool             heap        = false;
    uint32_t         groupCountX = 1;
    uint32_t         groupCountY = 1;
    uint32_t         groupCountZ = 1;
};

template <typename Command>
struct IndirectDrawState {
    VkPipeline       pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout layout         = VK_NULL_HANDLE;
    VkDescriptorSet  set            = VK_NULL_HANDLE;
    bool             heap           = false;
    VkBuffer         argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize     offset         = 0;
    uint32_t         drawCount      = 0;
    uint32_t         stride         = sizeof(Command);

    static constexpr VkDeviceSize OffsetForIndex(uint32_t index) noexcept {
        return static_cast<VkDeviceSize>(index) * sizeof(Command);
    }
};

template <typename Command>
struct IndirectCountDrawState {
    VkPipeline       pipeline          = VK_NULL_HANDLE;
    VkPipelineLayout layout            = VK_NULL_HANDLE;
    VkDescriptorSet  set               = VK_NULL_HANDLE;
    bool             heap              = false;
    VkBuffer         argumentBuffer    = VK_NULL_HANDLE;
    VkDeviceSize     offset            = 0;
    VkBuffer         countBuffer       = VK_NULL_HANDLE;
    VkDeviceSize     countBufferOffset = 0;
    uint32_t         maxDrawCount      = 0;
    uint32_t         stride            = sizeof(Command);

    static constexpr VkDeviceSize OffsetForIndex(uint32_t index) noexcept {
        return static_cast<VkDeviceSize>(index) * sizeof(Command);
    }

    static constexpr VkDeviceSize CountOffsetForIndex(uint32_t index) noexcept {
        return static_cast<VkDeviceSize>(index) * sizeof(uint32_t);
    }
};

using DrawIndirectState             = IndirectDrawState<VkDrawIndirectCommand>;
using DrawIndexedIndirectState      = IndirectDrawState<VkDrawIndexedIndirectCommand>;
using MeshTaskIndirectState         = IndirectDrawState<VkDrawMeshTasksIndirectCommandEXT>;
using DrawIndirectCountState        = IndirectCountDrawState<VkDrawIndirectCommand>;
using DrawIndexedIndirectCountState = IndirectCountDrawState<VkDrawIndexedIndirectCommand>;
using MeshTaskIndirectCountState    = IndirectCountDrawState<VkDrawMeshTasksIndirectCommandEXT>;


enum class CommandRingError : uint8_t {
    FenceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Synchronization fence creation failed for a command ring slot">{}) = 1,
};

template <QueueType QType, size_t Capacity = 8>
class CommandRing {
  public:
    CommandRing() = default;
    ~CommandRing() {
        Cleanup();
    }

    CommandRing(const CommandRing&)            = delete;
    CommandRing& operator=(const CommandRing&) = delete;

    CommandRing(CommandRing&& other) noexcept:
        _device(std::exchange(other._device, VK_NULL_HANDLE)), _pools(std::move(other._pools)), _cmds(std::move(other._cmds)),
        _fences(std::exchange(other._fences, {})), _pending(std::exchange(other._pending, {})),
        _index(other._index.exchange(0, std::memory_order::relaxed)) {
    }

    auto operator=(CommandRing&& other) noexcept -> CommandRing& {
        if (this != &other) {
            Cleanup();
            _device = std::exchange(other._device, VK_NULL_HANDLE);
            _pools  = std::move(other._pools);
            _cmds   = std::move(other._cmds);
            _fences = std::exchange(other._fences, {});
            _pending = std::exchange(other._pending, {});
            _index.store(other._index.exchange(0, std::memory_order::relaxed), std::memory_order::relaxed);
        }
        return *this;
    }

    [[nodiscard]] auto Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode> {
        _device = device;
        for (size_t i = 0; i < Capacity; ++i) {
            _pools[i] = CommandPool<QType>(_device, queueFamily);
            auto alloc = _pools[i].Allocate(1);
            if (!alloc) [[unlikely]] {
                Cleanup();
                return std::unexpected(alloc.error());
            }
            _cmds[i] = _pools[i][0];

            VkFenceCreateInfo fence_info = {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT
            };
            if (vkCreateFence(_device, &fence_info, nullptr, &_fences[i]) != VK_SUCCESS) [[unlikely]] {
                _fences[i] = VK_NULL_HANDLE;
                Cleanup();
                return std::unexpected(CommandRingError::FenceCreationFailed);
            }
        }
        return {};
    }

    void Cleanup() noexcept {
        if (_device != VK_NULL_HANDLE) {
            for (size_t i = 0; i < Capacity; ++i) {
                if (_fences[i] != VK_NULL_HANDLE) {
                    if (_pending[i]) { vkWaitForFences(_device, 1, &_fences[i], VK_TRUE, UINT64_MAX); }
                    vkDestroyFence(_device, _fences[i], nullptr);
                    _fences[i] = VK_NULL_HANDLE;
                    _pending[i] = false;
                }
                _pools[i] = {};
                _cmds[i]  = {};
            }
            _device = VK_NULL_HANDLE;
        }
    }

    struct Slot {
        CommandBuffer<QType> cmd;
        VkFence              fence;
        uint32_t             index;
    };

    [[nodiscard]] auto Acquire() noexcept -> std::expected<Slot, ErrorCode> {
        const uint32_t slotIndex = _index.fetch_add(1, std::memory_order_relaxed) % Capacity;
        if (_pending[slotIndex]) {
            if (const VkResult waited = vkWaitForFences(_device, 1, &_fences[slotIndex], VK_TRUE, UINT64_MAX); waited != VK_SUCCESS) {
                return std::unexpected(ToError(waited));
            }
            _pending[slotIndex] = false;
        }
        _pools[slotIndex].Reset();
        return Slot {_cmds[slotIndex], _fences[slotIndex], slotIndex};
    }

    [[nodiscard]] auto Submit(VkQueue queue, Slot slot, ExecutableCommands cmds) noexcept -> std::expected<void, ErrorCode> {
        if (const VkResult reset = vkResetFences(_device, 1, &slot.fence); reset != VK_SUCCESS) {
            return std::unexpected(ToError(reset));
        }
        auto submitted = QueueSubmit(queue, std::move(cmds), {}, {}, slot.fence);
        if (submitted) { _pending[slot.index] = true; }
        return submitted;
    }

    [[nodiscard]] auto Submit(StagingRingBuffer& ringBuffer, Slot slot, ExecutableCommands cmds) noexcept -> uint64_t {
        if (vkResetFences(_device, 1, &slot.fence) != VK_SUCCESS) { return 0; }
        const uint64_t value = ringBuffer.Submit(std::move(cmds), slot.fence);
        if (value != 0) { _pending[slot.index] = true; }
        return value;
    }

  private:
    VkDevice                                   _device = VK_NULL_HANDLE;
    std::array<CommandPool<QType>, Capacity>   _pools {};
    std::array<CommandBuffer<QType>, Capacity> _cmds {};
    std::array<VkFence, Capacity>              _fences {};
    std::array<bool, Capacity>                 _pending {};
    std::atomic<uint32_t>                      _index {0};
};

template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
void ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring, RecordFn&& record, bool blockCPU = true) {
    auto acquired = ring.Acquire();
    if (!acquired) { return; }
    auto slot = *acquired;
    auto recording = CommandRecorder::Begin(slot.cmd);
    if (!recording) { return; }
    std::forward<RecordFn>(record)(recording->Handle());
    auto executable = std::move(*recording).End();
    if (!executable) { return; }

    if (auto res = ring.Submit(ResolveQueue<QType>(ctx), slot, std::move(*executable)); !res) [[unlikely]] {
        return;
    }

    if (blockCPU) {
        vkWaitForFences(ctx.Device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    }
}

template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
void ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring, StagingRingBuffer& ringBuffer, RecordFn&& record) {
    auto acquired = ring.Acquire();
    if (!acquired) { return; }
    auto slot = *acquired;
    auto recording = CommandRecorder::Begin(slot.cmd);
    if (!recording) { return; }
    std::forward<RecordFn>(record)(recording->Handle());
    auto executable = std::move(*recording).End();
    if (!executable) { return; }

    const uint64_t submit_val = ring.Submit(ringBuffer, slot, std::move(*executable));
    if (submit_val == 0) [[unlikely]] {
        return;
    }

    VkSemaphore         semaphore = ringBuffer.GetSemaphore();
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .pNext = nullptr, .flags = 0, .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &submit_val
    };
    vkWaitSemaphores(ctx.Device(), &wait_info, UINT64_MAX);
}


inline constexpr VkShaderStageFlags kMeshTaskPushStages = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;

class CommandEncoder {
  public:
    VkCommandBuffer  cmd               = VK_NULL_HANDLE;
    VkPipeline       lastPipeline      = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout        = VK_NULL_HANDLE;
    VkDescriptorSet  lastDescriptorSet = VK_NULL_HANDLE;
    VkCullModeFlags  lastCullMode      = VK_CULL_MODE_BACK_BIT;
    bool             hasCullMode       = false;

    CommandEncoder() = default;
    explicit CommandEncoder(VkCommandBuffer c) noexcept: cmd(c) {
    }

    void BindPipeline(VkPipeline pipeline, VkPipelineLayout layout) noexcept {
        if (pipeline != VK_NULL_HANDLE && pipeline != lastPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            lastPipeline      = pipeline;
            lastLayout        = layout;
            lastDescriptorSet = VK_NULL_HANDLE;
        }
    }

    void BindDescriptorSet(VkDescriptorSet set) noexcept {
        if (set != VK_NULL_HANDLE && set != lastDescriptorSet) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lastLayout, 0, 1, &set, 0, nullptr);
            lastDescriptorSet = set;
        }
    }

    void BindDescriptorSets(uint32_t firstSet, std::span<const VkDescriptorSet> sets) noexcept {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lastLayout, firstSet, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        if (!sets.empty()) {
            lastDescriptorSet = sets[0];
        }
    }

    // Vulkan 1.3 core dynamic cull state; redundant updates are skipped per encoder.
    void SetCullMode(VkCullModeFlags mode) noexcept {
        if (!hasCullMode || lastCullMode != mode) {
            vkCmdSetCullMode(cmd, mode);
            lastCullMode = mode;
            hasCullMode = true;
        }
    }

    void SetViewport(const VkViewport& viewport) noexcept {
        vkCmdSetViewport(cmd, 0, 1, &viewport);
    }

    void SetScissor(const VkRect2D& scissor) noexcept {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void Draw(
        uint32_t           vertexCount,
        uint32_t           instanceCount,
        const T&           pushConstants,
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this draw's push struct is written for: DrawInstanced<Shaders::Modules::X>(...)");
        static_assert(
            PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        Push(cmd, lastLayout, stages, pushConstants);
        vkCmdDraw(cmd, vertexCount, instanceCount, 0, 0);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawHeap(uint32_t vertexCount, uint32_t instanceCount, const T& pushConstants) noexcept {
        PushDrawData<Modules...>(pushConstants);
        vkCmdDraw(cmd, vertexCount, instanceCount, 0, 0);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void PushDrawData(const T& pushConstants, VkShaderStageFlags  = 0) noexcept {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this draw's push struct is written for: DrawInstanced<Shaders::Modules::X>(...)");
        static_assert(
            PushConstantLayoutMatchesAll<T, Modules...>(),
            "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
        );
        PushData(cmd, 0, pushConstants);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawInstanced(
        const DrawState&   state,
        const T&           pushConstants,
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDraw(cmd, state.vertexCount, state.instanceCount, state.firstVertex, state.firstInstance);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndirect(
        const DrawIndirectState& state,
        const T&                 pushConstants,
        VkShaderStageFlags       stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndirectCount(
        const DrawIndirectCountState& state,
        const T&                      pushConstants,
        VkShaderStageFlags            stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawIndirectCount(cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndexedIndirect(
        const DrawIndexedIndirectState& state,
        const T&                        pushConstants,
        VkShaderStageFlags              stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawIndexedIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndexedIndirectCount(
        const DrawIndexedIndirectCountState& state,
        const T&                             pushConstants,
        VkShaderStageFlags                   stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawIndexedIndirectCount(cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride);
    }


    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawMeshTasks(const MeshTaskState& state, const T& pushConstants, VkShaderStageFlags stages = kMeshTaskPushStages) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawMeshTasksEXT(cmd, state.groupCountX, state.groupCountY, state.groupCountZ);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawMeshTasksIndirect(const MeshTaskIndirectState& state, const T& pushConstants, VkShaderStageFlags stages = kMeshTaskPushStages) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawMeshTasksIndirectEXT(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawMeshTasksIndirectCount(const MeshTaskIndirectCountState& state, const T& pushConstants, VkShaderStageFlags stages = kMeshTaskPushStages) noexcept {
        BindDraw<Modules...>(state, pushConstants, stages);
        vkCmdDrawMeshTasksIndirectCountEXT(
            cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride
        );
    }

  private:
    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void BindDraw(const auto& state, const T& pushConstants, VkShaderStageFlags stages) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData<Modules...>(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
    }
};

}
