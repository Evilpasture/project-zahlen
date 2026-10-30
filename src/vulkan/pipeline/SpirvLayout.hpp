// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN::Vk {

// --- Compute workgroup size ------------------------------------------------
//
// SPIR-V bakes a compute entry point's threadgroup size into
// `OpExecutionMode %entry_point LocalSize X Y Z` (opcode 16, mode 17). The
// engine does not decode that, and neither does the generator's output need to
// be decoded: utils/zshader reflects the [numthreads] attribute in-process and
// writes the number into ShaderBindings.hpp as `Module::LocalSize`, which is
// what LocalSizeOf hands to the dispatch math as a compile-time constant.
//
// What used to live beside this -- a constexpr SPIR-V parser that walked every
// module's bytes to re-derive these types, their member offsets and their push
// blocks -- is gone with the checks that read it (CatalogChecks.hpp,
// SpirvBindings.hpp). Slang is the single source of truth: the generator
// reflects, the header states, the engine reads.

struct WorkgroupSize {
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;

    [[nodiscard]] constexpr auto operator==(const WorkgroupSize&) const noexcept -> bool = default;
};

// The module's workgroup size, as the generator stated it. (`Module::Bytes()`
// is declaration-only in the header -- the bytes live in ShaderBytecode.cpp
// alone -- so nothing here could read them even if it wanted to. The
// ShaderProgram constraint is omitted so utils/zshader can include this header
// without pulling the engine's include guard.)
template <typename Module>
inline constexpr WorkgroupSize LocalSizeOf = Module::LocalSize;

} // namespace ZHLN::Vk
