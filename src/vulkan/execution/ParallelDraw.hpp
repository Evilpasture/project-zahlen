// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "RecordingBatch.hpp"
#include <Zahlen/Error.hpp>

namespace ZHLN::Vk {

namespace detail {
struct ParallelForCallback {
    void operator()([[maybe_unused]] uint32_t start, [[maybe_unused]] uint32_t end, [[maybe_unused]] uint32_t chunkIdx) const noexcept {
    }
};
}

template <typename S>
concept ParallelScheduler = requires(S&& s, uint32_t count, uint32_t chunkSize) { s.ParallelFor(count, chunkSize, detail::ParallelForCallback {}); };

// Records `drawCount` draws across `num_chunks` secondaries, one chunk per
// worker, and hands back one entry per chunk: the ones whose commands came back
// executable, and the ones that did not, each with its error. A batch with any
// error in it is not executed at all -- a pass whose draws are split across
// secondaries cannot run half of them -- but the caller still learns which chunk
// failed and why, which the previous `std::atomic<bool> recordingFailed` could
// not say.
template <ParallelScheduler SchedulerT, typename CmdProviderFn, typename RecordFn>
inline auto ParallelDrawDispatch(
    VkCommandBuffer             primaryCmd,
    const SecondaryPassContext& passContext,
    uint32_t                    drawCount,
    uint32_t                    chunkSize,
    SchedulerT&&                scheduler,
    CmdProviderFn&&             cmdProvider,
    RecordFn&&                  recordFn
) -> RecordingBatch {
    uint32_t num_chunks = (drawCount + chunkSize - 1) / chunkSize;
    if (num_chunks == 0) {
        return {};
    }

    // One element per chunk, written only by that chunk's worker; the batch is
    // assembled after the parallel section has joined.
    std::vector<VkCommandBuffer> secondaries(num_chunks, VK_NULL_HANDLE);
    std::vector<ErrorCode>       errors(num_chunks);

    std::forward<SchedulerT>(scheduler).ParallelFor(drawCount, chunkSize, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) noexcept {
        VkCommandBuffer sec_cmd = std::forward<CmdProviderFn>(cmdProvider)(chunkIdx);

        // The context links the inheritance chain, pushes the frame rows and
        // sets the viewport -- the same three steps a parallel fork needs, which
        // is why they live there rather than here.
        auto recording = passContext.Begin(sec_cmd, VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        if (!recording) {
            errors[chunkIdx] = recording.error();
            return;
        }

        CommandEncoder encoder(sec_cmd);
        for (uint32_t i = start; i < end; ++i) {
            recordFn(encoder, i);
        }

        auto executable = std::move(*recording).End();
        if (!executable) {
            errors[chunkIdx] = executable.error();
            return;
        }
        secondaries[chunkIdx] = executable->Handle();
    });

    RecordingBatch recorded;
    recorded.Reserve(num_chunks, num_chunks);
    for (uint32_t chunk = 0; chunk < num_chunks; ++chunk) {
        if (secondaries[chunk] != VK_NULL_HANDLE) {
            recorded.AddSuccess(chunk);
        } else {
            recorded.EmplaceError(RecordingFailure {.index = chunk, .error = errors[chunk]});
        }
    }

    // A batch that lost a chunk is not executed: a pass split across secondaries
    // cannot run half of its draws. The batch says which chunk failed and why, so
    // the caller can log it -- that used to be a single anonymous warning here.
    if (!recorded) {
        return recorded;
    }
    Vk::ExecuteCommands(primaryCmd, std::span<const VkCommandBuffer> {secondaries.data(), secondaries.size()});
    return recorded;
}
}
