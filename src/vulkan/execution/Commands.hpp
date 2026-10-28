// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

struct DrawState {
    VkPipeline pipeline      = VK_NULL_HANDLE;
    uint32_t   vertexCount   = 0;
    uint32_t   indexCount    = 0;
    uint32_t   instanceCount = 1;
    uint32_t   firstVertex   = 0;
    uint32_t   firstIndex    = 0;
    uint32_t   firstInstance = 0;
};

struct MeshTaskState {
    VkPipeline pipeline    = VK_NULL_HANDLE;
    uint32_t   groupCountX = 1;
    uint32_t   groupCountY = 1;
    uint32_t   groupCountZ = 1;
};

template <typename Command>
struct IndirectDrawState {
    VkPipeline   pipeline       = VK_NULL_HANDLE;
    VkBuffer     argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize offset         = 0;
    uint32_t     drawCount      = 0;
    uint32_t     stride         = sizeof(Command);

    static constexpr VkDeviceSize OffsetForIndex(uint32_t index) noexcept {
        return static_cast<VkDeviceSize>(index) * sizeof(Command);
    }
};

template <typename Command>
struct IndirectCountDrawState {
    VkPipeline   pipeline          = VK_NULL_HANDLE;
    VkBuffer     argumentBuffer    = VK_NULL_HANDLE;
    VkDeviceSize offset            = 0;
    VkBuffer     countBuffer       = VK_NULL_HANDLE;
    VkDeviceSize countBufferOffset = 0;
    uint32_t     maxDrawCount      = 0;
    uint32_t     stride            = sizeof(Command);

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
    TimelineCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Timeline semaphore creation failed for a command ring"> {}) = 1,
};

template <QueueType QType, size_t Capacity = 8>
class CommandRing {
  public:
    CommandRing() = default;
    ~CommandRing() noexcept {
        [[maybe_unused]] auto cleanup = Cleanup();
    }
    CommandRing(const CommandRing&)                    = delete;
    auto operator=(const CommandRing&) -> CommandRing& = delete;
    CommandRing(CommandRing&& other) noexcept:
        _device(std::exchange(other._device, VK_NULL_HANDLE)), _timeline(std::move(other._timeline)), _pools(std::move(other._pools)),
        _cmds(std::move(other._cmds)), _completionValues(std::exchange(other._completionValues, {})), _nextValue(other._nextValue.exchange(0)),
        _index(other._index.exchange(0)) {
    }
    auto operator=(CommandRing&& other) noexcept -> CommandRing& {
        if (this != &other) {
            [[maybe_unused]] auto cleanup = Cleanup();
            _device                       = std::exchange(other._device, VK_NULL_HANDLE);
            _timeline                     = std::move(other._timeline);
            _pools                        = std::move(other._pools);
            _cmds                         = std::move(other._cmds);
            _completionValues             = std::exchange(other._completionValues, {});
            _nextValue.store(other._nextValue.exchange(0));
            _index.store(other._index.exchange(0));
        }
        return *this;
    }
    [[nodiscard]] auto Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode> {
        _device       = device;
        auto timeline = Semaphore::CreateTimeline(device);
        if (!timeline) [[unlikely]] {
            return std::unexpected(CommandRingError::TimelineCreationFailed);
        }
        _timeline = std::move(*timeline);
        for (size_t i = 0; i < Capacity; ++i) {
            auto created = CommandPool<QType>::Create(device, queueFamily);
            if (!created) [[unlikely]] {
                if (auto cleanup = Cleanup(); !cleanup) {
                    return std::unexpected(cleanup.error());
                }
                return std::unexpected(created.error());
            }
            _pools[i] = std::move(*created);
            if (auto a = _pools[i].Allocate(1); !a) [[unlikely]] {
                if (auto cleanup = Cleanup(); !cleanup) {
                    return std::unexpected(cleanup.error());
                }
                return std::unexpected(a.error());
            }
            _cmds[i] = _pools[i][0];
        }
        return {};
    }
    [[nodiscard]] auto Cleanup() noexcept -> std::expected<void, ErrorCode> {
        if (_device == VK_NULL_HANDLE) {
            return {};
        }

        uint64_t maxValue = 0;
        for (const uint64_t value: _completionValues) {
            maxValue = std::max(maxValue, value);
        }
        if (const VkResult waited = WaitValue(maxValue); waited != VK_SUCCESS) {
            // Keep every object alive: destroying a pool or its timeline while
            // submitted command buffers may still be executing is invalid.
            // The owner can now propagate device loss instead of silently
            // tearing down live Vulkan objects.
            return std::unexpected(ToError(waited));
        }

        for (size_t i = 0; i < Capacity; ++i) {
            _pools[i] = {};
            _cmds[i]  = {};
        }
        _timeline.Reset();
        _device           = VK_NULL_HANDLE;
        _completionValues = {};
        return {};
    }
    struct Slot {
        CommandBuffer<QType> cmd;
        uint32_t             index;
        uint64_t             completionValue;
    };
    [[nodiscard]] auto WaitValue(uint64_t value) const noexcept -> VkResult {
        if (value == 0)
            return VK_SUCCESS;
        const VkSemaphore         raw = _timeline.Get();
        const VkSemaphoreWaitInfo w {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &raw, .pValues = &value};
        return vkWaitSemaphores(_device, &w, UINT64_MAX);
    }
    [[nodiscard]] auto Acquire() noexcept -> std::expected<Slot, ErrorCode> {
        const uint32_t i = _index.fetch_add(1, std::memory_order_relaxed) % Capacity;
        if (const VkResult r = WaitValue(_completionValues[i]); r != VK_SUCCESS) {
            return std::unexpected(ToError(r));
        }
        if (auto reset = _pools[i].Reset(); !reset) [[unlikely]] {
            return std::unexpected(reset.error());
        }
        return Slot {_cmds[i], i, _completionValues[i]};
    }
    [[nodiscard]] auto Submit(VkQueue queue, Slot slot, ExecutableCommands cmds) noexcept -> std::expected<void, ErrorCode> {
        const uint64_t value  = _nextValue.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto     signal = SemaphoreSubmit::Timeline(_timeline, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        auto           result = QueueSubmit(queue, std::move(cmds), {}, std::span<const SemaphoreSubmit> {&signal, 1});
        if (result)
            _completionValues[slot.index] = value;
        return result;
    }
    [[nodiscard]] auto CompletionValue(uint32_t index) const noexcept -> uint64_t {
        return _completionValues[index];
    }
    [[nodiscard]] auto Submit(StagingRingBuffer& ringBuffer, Slot slot, ExecutableCommands cmds) noexcept
        -> std::expected<uint64_t, ErrorCode> {
        auto submitted = ringBuffer.Submit(std::move(cmds));
        if (!submitted) [[unlikely]] {
            return std::unexpected(submitted.error());
        }
        _completionValues[slot.index] = 0;
        return *submitted;
    }

  private:
    VkDevice                                   _device = VK_NULL_HANDLE;
    Semaphore                                  _timeline;
    std::array<CommandPool<QType>, Capacity>   _pools {};
    std::array<CommandBuffer<QType>, Capacity> _cmds {};
    std::array<uint64_t, Capacity>             _completionValues {};
    std::atomic<uint64_t>                      _nextValue {0};
    std::atomic<uint32_t>                      _index {0};
};

template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
[[nodiscard]] auto ExecuteAsync(const Context& ctx, CommandRing<QType, Capacity>& ring, RecordFn&& record) -> std::expected<uint64_t, ErrorCode> {
    auto acquired = ring.Acquire();
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    auto slot = *acquired;

    auto recording = CommandRecorder::Begin(slot.cmd);
    if (!recording) {
        return std::unexpected(recording.error());
    }

    std::forward<RecordFn>(record)(recording->Handle());

    auto executable = std::move(*recording).End();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    if (auto submitted = ring.Submit(ResolveQueue<QType>(ctx), slot, std::move(*executable)); !submitted) [[unlikely]] {
        return std::unexpected(submitted.error());
    }

    return ring.CompletionValue(slot.index);
}

template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
[[nodiscard]] auto ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring, RecordFn&& record) -> std::expected<void, ErrorCode> {
    auto submitted = ExecuteAsync(ctx, ring, std::forward<RecordFn>(record));

    if (!submitted) [[unlikely]] {
        return std::unexpected(submitted.error());
    }

    if (const VkResult waited = ring.WaitValue(*submitted); waited != VK_SUCCESS) [[unlikely]] {
        if (waited == VK_ERROR_DEVICE_LOST) {
            ZHLN::LogError("[DeviceLost] Vulkan immediate submission lost device");
        }
        return std::unexpected(ToError(waited));
    }

    return {};
}

template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
[[nodiscard]] auto ExecuteImmediate(
    const Context& ctx, CommandRing<QType, Capacity>& ring, StagingRingBuffer& ringBuffer, RecordFn&& record
) -> std::expected<void, ErrorCode> {
    auto acquired = ring.Acquire();
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    auto slot      = *acquired;
    auto recording = CommandRecorder::Begin(slot.cmd);
    if (!recording) {
        return std::unexpected(recording.error());
    }
    std::forward<RecordFn>(record)(recording->Handle());
    auto executable = std::move(*recording).End();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    auto submitted = ring.Submit(ringBuffer, slot, std::move(*executable));
    if (!submitted) [[unlikely]] {
        return std::unexpected(submitted.error());
    }

    const VkSemaphore semaphore = ringBuffer.GetSemaphore();
    const uint64_t     value     = *submitted;
    const VkSemaphoreWaitInfo waitInfo {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &semaphore,
        .pValues        = &value,
    };
    if (const VkResult waited = vkWaitSemaphores(ctx.Device(), &waitInfo, UINT64_MAX); waited != VK_SUCCESS) [[unlikely]] {
        if (waited == VK_ERROR_DEVICE_LOST) {
            ZHLN::LogError("[DeviceLost] Vulkan immediate submission lost device");
        }
        return std::unexpected(ToError(waited));
    }

    return {};
}

class CommandEncoder {
  public:
    VkCommandBuffer cmd          = VK_NULL_HANDLE;
    VkPipeline      lastPipeline = VK_NULL_HANDLE;
    VkCullModeFlags lastCullMode = VK_CULL_MODE_BACK_BIT;
    bool            hasCullMode  = false;

    CommandEncoder() = default;
    explicit CommandEncoder(VkCommandBuffer c) noexcept: cmd(c) {
    }

    void BindPipeline(VkPipeline pipeline) noexcept {
        if (pipeline != VK_NULL_HANDLE && pipeline != lastPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            lastPipeline = pipeline;
        }
    }

    void SetCullMode(VkCullModeFlags mode) noexcept {
        if (!hasCullMode || lastCullMode != mode) {
            vkCmdSetCullMode(cmd, mode);
            lastCullMode = mode;
            hasCullMode  = true;
        }
    }

    void SetViewport(const VkViewport& viewport) noexcept {
        vkCmdSetViewport(cmd, 0, 1, &viewport);
    }

    void SetScissor(const VkRect2D& scissor) noexcept {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawHeap(uint32_t vertexCount, uint32_t instanceCount, const T& pushConstants) noexcept {
        PushDrawData<Modules...>(pushConstants);
        vkCmdDraw(cmd, vertexCount, instanceCount, 0, 0);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void PushDrawData(const T& pushConstants) noexcept {
        static_assert(sizeof...(Modules) > 0, "name the shader module(s) this draw's push struct is written for: DrawInstanced<Shaders::Modules::X>(...)");
        static_assert(
            PushConstantLayoutMatchesAll<T, Modules...>(), "the push struct is not the push-constant block the named shader module(s) declare: same members, "
                                                           "same offsets, same sizes, or it is not the same struct"
        );
        PushData(cmd, 0, pushConstants);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawInstanced(const DrawState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDraw(cmd, state.vertexCount, state.instanceCount, state.firstVertex, state.firstInstance);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndirect(const DrawIndirectState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndirectCount(const DrawIndirectCountState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawIndirectCount(cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndexedIndirect(const DrawIndexedIndirectState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawIndexedIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawIndexedIndirectCount(const DrawIndexedIndirectCountState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawIndexedIndirectCount(cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawMeshTasks(const MeshTaskState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawMeshTasksEXT(cmd, state.groupCountX, state.groupCountY, state.groupCountZ);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawMeshTasksIndirect(const MeshTaskIndirectState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawMeshTasksIndirectEXT(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
    }

    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void DrawMeshTasksIndirectCount(const MeshTaskIndirectCountState& state, const T& pushConstants) noexcept {
        BindDraw<Modules...>(state, pushConstants);
        vkCmdDrawMeshTasksIndirectCountEXT(
            cmd, state.argumentBuffer, state.offset, state.countBuffer, state.countBufferOffset, state.maxDrawCount, state.stride
        );
    }

  private:
    template <ShaderProgram... Modules, GpuTriviallyCopyable T>
    void BindDraw(const auto& state, const T& pushConstants) noexcept {
        BindPipeline(state.pipeline);
        PushDrawData<Modules...>(pushConstants);
    }
};

} // namespace ZHLN::Vk
