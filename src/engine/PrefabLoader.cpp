// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/PrefabLoader.cpp
//
// The registry behind Zahlen/PrefabLoader.hpp. There is deliberately nothing
// here but a table: no format knowledge, no includes from extras/, no opinion
// about what a model file is. extras/glTF fills it in.

#include <Zahlen/PrefabLoader.hpp>

#include <atomic>

namespace ZHLN::PrefabLoader {

namespace {

// The installed table, in engine-owned storage so a caller may register from a
// temporary. Published through a pointer so readers see either "nothing" or a
// completely written table -- never one half-filled while a system on a worker
// fiber is mid-call.
Backend                     s_Storage {};
std::atomic<const Backend*> s_Backend {nullptr};

} // namespace

void Register(const Backend& backend) noexcept {
    s_Storage = backend;
    s_Backend.store(&s_Storage, std::memory_order_release);
}

auto Get() noexcept -> const Backend* {
    return s_Backend.load(std::memory_order_acquire);
}

bool IsAvailable() noexcept {
    return Get() != nullptr;
}

} // namespace ZHLN::PrefabLoader
