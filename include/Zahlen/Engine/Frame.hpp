// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN {

class WorkerScratchPool;

// One execution's worth of state: the ambient clock every system can read, and
// the scratch pool a system allocates from.
//
// This is the *only* thing that changes between two runs of the same graph. It
// is deliberately not a bag of engine pointers: services live in the graph (see
// SystemGraph<Services>), and what a system gets per invocation is this.
struct Frame {
    uint64_t frame = 0;
    float    alpha = 0.0f;
    float    dt    = 0.0f;

    // Null only in a graph that offers no scratch; a system whose signature asks
    // for SoAScratch<T, N> or LinearArena& is resolved from the engine's pool.
    WorkerScratchPool* scratch = nullptr;
};

} // namespace ZHLN
