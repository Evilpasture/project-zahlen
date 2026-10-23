// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/BatchResult.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN::Vk {

// Recording a batch of secondary command buffers is N independent Begin/End
// pairs: one per parallel slot in ParallelCommandRecorder, one per draw chunk in
// ParallelDrawDispatch. They do not share a fate -- a slot can fail because its
// command pool is exhausted while the others record fine -- but the shape both
// callers need is the same, and it is not "the first error". A fork whose
// secondaries half-recorded is skipped as a whole, which is exactly the moment
// someone wants to know *which* slot failed and why, not just that something did.

/// One item of a recording batch that produced no executable commands.
struct RecordingFailure {
    //: The item's ordinal in the batch: a parallel slot, or a draw chunk.
    uint32_t  index = 0;
    ErrorCode error {};
};

/// The recordings a batch produced: the ordinals that came back executable, and
/// the ones that did not, each with the error that stopped it. Empty is a
/// complete success, like every other BatchResult.
using RecordingBatch = BatchResult<uint32_t, RecordingFailure>;

} // namespace ZHLN::Vk
