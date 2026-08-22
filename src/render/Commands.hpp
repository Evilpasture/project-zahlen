// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Commands.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

/**
 * @brief Configuration for a generic draw batch.
 */
struct DrawBatchConfig {
    VkPipeline         pipeline   = VK_NULL_HANDLE;
    VkPipelineLayout   layout     = VK_NULL_HANDLE;
    VkBuffer           vbo        = VK_NULL_HANDLE;
    VkBuffer           ibo        = VK_NULL_HANDLE;
    VkDescriptorSet    set        = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages = 0;
};

/**
 * @brief A high-performance template that binds common Vulkan state once
 * and executes a stream of draw calls via a user-provided loop.
 *
 * @tparam PushT The Type of the Push Constant struct (use std::monostate if none).
 * @tparam LoopFn A lambda that receives a 'draw(PushT, count, first)' caller.
 */
template <typename PushT = std::monostate, typename LoopFn>
inline void DrawBatch(const VkCommandBuffer cmd, const DrawBatchConfig& cfg, LoopFn&& loop) {
    // 1. Static Bindings (Fixed for the whole batch)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cfg.pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &cfg.vbo, &offset);
    vkCmdBindIndexBuffer(cmd, cfg.ibo, 0, VK_INDEX_TYPE_UINT32);

    if (cfg.set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cfg.layout, 0, 1, &cfg.set, 0, nullptr);
    }

    // 2. Dynamic Recording
    // We provide a 'binder' lambda back to the user to record individual instances
    auto record = [&](const PushT& pc, uint32_t indexCount, uint32_t firstIndex) -> auto {
        if constexpr (!std::is_same_v<PushT, std::monostate>) {
            ZHLN::Vk::Push(cmd, cfg.layout, cfg.pushStages, pc);
        }
        vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, 0, 0);
    };

    // Forward the loop to ensure the caller's value category is preserved
    std::forward<LoopFn>(loop)(record);
}

/**
 * @brief High-performance, strongly-typed bindless batch drawer.
 * Binds the pipeline and global bindless set once, exposing an optimized draw callback.
 */
template <size_t ColorCount, bool HasDepth, typename LoopFn>
inline void DrawBindlessBatch(
    const VkCommandBuffer                      cmd,
    const TypedPipeline<ColorCount, HasDepth>& pipeline,
    VkPipelineLayout                           layout,
    VkDescriptorSet                            bindlessSet,
    VkShaderStageFlags                         pushStages,
    LoopFn&&                                   loop
) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

    if (bindlessSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bindlessSet, 0, nullptr);
    }

    auto draw = [&](uint32_t vertexCount, uint32_t instanceIdx, const auto& pc) {
        Push(cmd, layout, pushStages, pc);
        vkCmdDraw(cmd, vertexCount, 1, 0, instanceIdx);
    };

    std::forward<LoopFn>(loop)(draw);
}

// ============================================================================
// Immediate Commands
// ============================================================================

template <QueueType QType, size_t Capacity = 8>
class CommandRing {
  public:
    CommandRing() = default;
    ~CommandRing() {
        Cleanup();
    }

    // Enforce move-only RAII semantics
    CommandRing(const CommandRing&)                = delete;
    CommandRing& operator=(const CommandRing&)     = delete;
    CommandRing(CommandRing&&) noexcept            = default;
    CommandRing& operator=(CommandRing&&) noexcept = default;

    void Init(VkDevice device, uint32_t queueFamily) noexcept {
        _device = device;
        for (size_t i = 0; i < Capacity; ++i) {
            _pools[i] = CommandPool<QType>(_device, queueFamily);
            if (!_pools[i].Valid()) {
                continue;
            }

            auto alloc_res = _pools[i].Allocate(1);
            if (!alloc_res) {
                continue;
            }
            _cmds[i] = _pools[i][0];

            VkFenceCreateInfo fence_info = {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                // Start signaled so the first Acquire() call passes through without stalling
                .flags = VK_FENCE_CREATE_SIGNALED_BIT
            };
            vkCreateFence(_device, &fence_info, nullptr, &_fences[i]);
        }
    }

    void Cleanup() noexcept {
        if (_device != VK_NULL_HANDLE) {
            for (size_t i = 0; i < Capacity; ++i) {
                if (_fences[i] != VK_NULL_HANDLE) {
                    vkWaitForFences(_device, 1, &_fences[i], VK_TRUE, UINT64_MAX);
                    vkDestroyFence(_device, _fences[i], nullptr);
                    _fences[i] = VK_NULL_HANDLE;
                }
                _pools[i] = {}; // Safely calls destructor (triggers C-core CommandPool destruction)
                _cmds[i]  = {};
            }
            _device = VK_NULL_HANDLE;
        }
    }

    struct Slot {
        CommandBuffer<QType> cmd;
        VkFence              fence;
    };

    [[nodiscard]] auto Acquire() noexcept -> Slot {
        uint32_t slot_idx = _index.fetch_add(1, std::memory_order::relaxed) % Capacity;

        // If the GPU is still processing this slot's last submission, block here
        vkWaitForFences(_device, 1, &_fences[slot_idx], VK_TRUE, UINT64_MAX);
        vkResetFences(_device, 1, &_fences[slot_idx]);

        // Recycle the command pool instantly without any driver reallocation
        _pools[slot_idx].Reset();

        return {_cmds[slot_idx], _fences[slot_idx]};
    }

  private:
    VkDevice                                   _device = VK_NULL_HANDLE;
    std::array<CommandPool<QType>, Capacity>   _pools {};
    std::array<CommandBuffer<QType>, Capacity> _cmds {};
    std::array<VkFence, Capacity>              _fences {};
    std::atomic<uint32_t>                      _index {0};
};

// Simple RAII wrapper.
class CommandBufferGuard {
  public:
    CommandBufferGuard(VkCommandBuffer cmdBuffer): cmd(cmdBuffer) {
        ZHLN_BeginCommandBuffer(cmd);
    }

    ~CommandBufferGuard() {
        ZHLN_EndCommandBuffer(cmd);
    }

    [[nodiscard]] VkCommandBuffer get() const {
        return cmd;
    }

    CommandBufferGuard(const CommandBufferGuard&)            = delete;
    CommandBufferGuard(CommandBufferGuard&&)                 = delete;
    CommandBufferGuard& operator=(const CommandBufferGuard&) = delete;
    CommandBufferGuard& operator=(CommandBufferGuard&&)      = delete;

  private:
    VkCommandBuffer cmd {};
};

/**
 * @brief Recycles a command buffer from the ring, records operations,
 *        and submits it. Stalls the CPU only if blockCPU is true.
 */
template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
void ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring, RecordFn&& record, bool blockCPU = true) {
    // 1. Recycle command buffer and fence from the ring (O(1) / Allocation-Free)
    auto [cmd, fence] = ring.Acquire();
    {
        CommandBufferGuard guard(cmd);
        std::forward<RecordFn>(record)(cmd);
    }

    VkQueue queue = ResolveQueue<QType>(ctx);

    // Bail early on submission failure to prevent vkWaitForFences from hanging
    if (auto res =
            QueueSubmit(queue, cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fence);
        !res) [[unlikely]] {
        return;
    }

    // 2. Synchronization Strategy
    if (blockCPU) {
        // Hard stall: waits immediately (e.g. for synchronous debug hooks)
        vkWaitForFences(ctx.Device(), 1, &fence, VK_TRUE, UINT64_MAX);
    }
}

/**
 * @brief Recycles a command buffer from the ring, records operations,
 *        submits via StagingRingBuffer (properly stamping its timeline value),
 *        and blocks the CPU until the staging transfer completes.
 */
template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
void ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring, StagingRingBuffer& ringBuffer, RecordFn&& record) {
    // 1. Recycle command buffer and fence from the ring
    auto [cmd, fence] = ring.Acquire();
    {
        CommandBufferGuard guard(cmd);
        std::forward<RecordFn>(record)(cmd);
    }

    // 2. Submit command buffer and fence in a single submission
    uint64_t submit_val = ringBuffer.Submit(cmd, fence);

    if (submit_val == 0) [[unlikely]] {
        return;
    }

    // 3. Synchronously wait on the timeline semaphore to retire staging memory
    VkSemaphore         semaphore = ringBuffer.GetSemaphore();
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .pNext = nullptr, .flags = 0, .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &submit_val
    };
    vkWaitSemaphores(ctx.Device(), &wait_info, UINT64_MAX);
}

// ============================================================================
// Command Encoder (Stateful Bind Filtering with Unified Push Constants)
// ============================================================================

/// Push-constant stages for the mesh path (the fragment stage keeps reading the
/// same block, and the task stage needs the instance id to cull against).
inline constexpr VkShaderStageFlags kMeshTaskPushStages = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct MeshTaskState {
    VkPipeline       pipeline    = VK_NULL_HANDLE;
    VkPipelineLayout layout      = VK_NULL_HANDLE;
    bool             heap        = false;
    VkDescriptorSet  set         = VK_NULL_HANDLE;
    uint32_t         groupCountX = 1;
    uint32_t         groupCountY = 1;
    uint32_t         groupCountZ = 1;
};

struct MeshTaskIndirectState {
    VkPipeline       pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout layout         = VK_NULL_HANDLE;
    bool             heap           = false;
    VkDescriptorSet  set            = VK_NULL_HANDLE;
    VkBuffer         argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize     offset         = 0;
    uint32_t         drawCount      = 1;
    uint32_t         stride         = sizeof(VkDrawMeshTasksIndirectCommandEXT);
};

struct MeshTaskIndirectCountState {
    VkPipeline       pipeline          = VK_NULL_HANDLE;
    VkPipelineLayout layout            = VK_NULL_HANDLE;
    bool             heap              = false;
    VkDescriptorSet  set               = VK_NULL_HANDLE;
    VkBuffer         argumentBuffer    = VK_NULL_HANDLE;
    VkDeviceSize     offset            = 0;
    VkBuffer         countBuffer       = VK_NULL_HANDLE;
    VkDeviceSize     countBufferOffset = 0;
    uint32_t         maxDrawCount      = 1;
    uint32_t         stride            = sizeof(VkDrawMeshTasksIndirectCommandEXT);
};

class CommandEncoder {
  public:
    VkCommandBuffer  cmd               = VK_NULL_HANDLE;
    VkPipeline       lastPipeline      = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout        = VK_NULL_HANDLE;
    VkDescriptorSet  lastDescriptorSet = VK_NULL_HANDLE;

    // Context is required when recording heap-mode draws (vkCmdPushDataEXT).
    const Context* ctx = nullptr;

    CommandEncoder() = default;
    explicit CommandEncoder(VkCommandBuffer c, const Context* context = nullptr) noexcept: cmd(c), ctx(context) {
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

    template <GpuTriviallyCopyable T>
    void Draw(
        uint32_t           vertexCount,
        uint32_t           instanceCount,
        const T&           pushConstants,
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        Push(cmd, lastLayout, stages, pushConstants);
        vkCmdDraw(cmd, vertexCount, instanceCount, 0, 0);
    }

    // VK_EXT_descriptor_heap draw: heaps are bound on the command buffer, the
    // pipeline was bound with BindPipeline, and per-draw data travels through
    // vkCmdPushDataEXT at offset 0.
    template <GpuTriviallyCopyable T>
    void DrawHeap(uint32_t vertexCount, uint32_t instanceCount, const T& pushConstants) noexcept {
        PushDrawData(pushConstants);
        vkCmdDraw(cmd, vertexCount, instanceCount, 0, 0);
    }

    // Heap mode (VK_EXT_descriptor_heap): the resource/sampler heaps are bound
    // on the command buffer itself, so no descriptor set is bound here, and
    // per-draw data travels through vkCmdPushDataEXT at offset 0 (legacy
    // PushConstant blocks in the SPIR-V read the push-data blob directly).
    template <GpuTriviallyCopyable T>
    void PushDrawData(const T& pushConstants, VkShaderStageFlags /*stages*/ = 0) noexcept {
        PushData(*ctx, cmd, 0, pushConstants);
    }

    template <GpuTriviallyCopyable T>
    void DrawInstanced(
        const DrawState&   state,
        const T&           pushConstants,
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        vkCmdDraw(cmd, state.vertexCount, state.instanceCount, state.firstVertex, state.firstInstance);
    }

    template <GpuTriviallyCopyable T>
    void DrawIndirect(
        const DrawIndirectState& state,
        const T&                 pushConstants,
        VkShaderStageFlags       stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        vkCmdDrawIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <GpuTriviallyCopyable T>
    void DrawIndirectCount(
        const DrawIndirectCountState& state,
        const T&                      pushConstants,
        VkShaderStageFlags            stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        vkCmdDrawIndirectCount(cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride);
    }

    template <GpuTriviallyCopyable T>
    void DrawIndexedIndirect(
        const DrawIndexedIndirectState& state,
        const T&                        pushConstants,
        VkShaderStageFlags              stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        vkCmdDrawIndexedIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    // ========================================================================
    // VK_EXT_mesh_shader
    // ========================================================================

    /// Dispatches task (or, without amplification, mesh) workgroups. The bound
    /// pipeline must be a mesh pipeline; per-draw data travels through push
    /// data at offset 0 exactly like the vertex path, so the task and mesh
    /// stages read the same ObjectConstants block the vertex shader used to.
    template <GpuTriviallyCopyable T>
    void DrawMeshTasks(const MeshTaskState& state, const T& pushConstants, VkShaderStageFlags stages = kMeshTaskPushStages) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        if (ctx != nullptr) {
            ctx->CmdDrawMeshTasks(cmd, state.groupCountX, state.groupCountY, state.groupCountZ);
        }
    }

    /// Indirect variant. `argumentBuffer` must hold VkDrawMeshTasksIndirectCommandEXT
    /// records (groupCountX/Y/Z) — note there is no firstInstance field, so the
    /// instance index has to be supplied through push data.
    template <GpuTriviallyCopyable T>
    void DrawMeshTasksIndirect(const MeshTaskIndirectState& state, const T& pushConstants, VkShaderStageFlags stages = kMeshTaskPushStages) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        if (ctx != nullptr) {
            ctx->CmdDrawMeshTasksIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
        }
    }

    template <GpuTriviallyCopyable T>
    void DrawMeshTasksIndirectCount(const MeshTaskIndirectCountState& state, const T& pushConstants, VkShaderStageFlags stages = kMeshTaskPushStages) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        if (ctx != nullptr) {
            ctx->CmdDrawMeshTasksIndirectCount(
                cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride
            );
        }
    }

    template <GpuTriviallyCopyable T>
    void DrawIndexedIndirectCount(
        const DrawIndexedIndirectCountState& state,
        const T&                             pushConstants,
        VkShaderStageFlags                   stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept {
        BindPipeline(state.pipeline, state.layout);
        if (state.heap) {
            PushDrawData(pushConstants);
        } else {
            BindDescriptorSet(state.set);
            Push(cmd, state.layout, stages, pushConstants);
        }
        vkCmdDrawIndexedIndirectCount(cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride);
    }
};

} // namespace ZHLN::Vk
