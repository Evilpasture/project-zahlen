// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN {

class WorkerScratchPool;

// The ambient values a system can ask for are engine vocabulary, not ECS
// vocabulary: Frame carries them, and the system-graph resolvers hand them
// back. Unlike raw float/uint64_t they cannot be confused with one another by
// the parameter resolver.
struct FrameDt {
    float value;
    constexpr operator float() const noexcept {
        return value;
    }
};
struct FrameAlpha {
    float value;
    constexpr operator float() const noexcept {
        return value;
    }
};
struct FrameIndex {
    uint64_t value;
    constexpr operator uint64_t() const noexcept {
        return value;
    }
};

// The engine's fixed simulation rate and per-frame step budget. Engine::Tick
// drains Clock::ConsumeFixedSteps(kFixedStepDt, kMaxFixedSteps) once per frame;
// the physics step runs exactly that many steps and the leftover fraction is
// this frame's interpolation alpha.
inline constexpr float    kFixedStepDt   = 1.0f / 60.0f;
inline constexpr uint32_t kMaxFixedSteps = 4;

// One execution's worth of state: the ambient clock every system can read, and
// the scratch pool a system allocates from.
//
// This is the *only* thing that changes between two runs of the same graph. It
// is deliberately not a bag of engine pointers: services live in the graph (see
// SystemGraph<Services>), and what a system gets per invocation is this.
struct Frame {
    FrameIndex frame = {};
    FrameAlpha alpha = {};
    FrameDt    dt    = {};

    // Null only in a graph that offers no scratch; a system whose signature asks
    // for SoAScratch<T, N> or LinearArena& is resolved from the engine's pool.
    WorkerScratchPool* scratch = nullptr;
};

} // namespace ZHLN
