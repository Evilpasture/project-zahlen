// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// include/Zahlen/PrefabLoader.hpp
//
// The seam that keeps the glTF importer optional.
//
// CreativeWorksFactory can build boxes, planes and materials on its own, but
// reading a model *file* means a container parser, an image decoder and a
// schema -- which is exactly the kind of dependency the core/extras boundary
// exists to keep out of the engine. So core declares the three operations it
// needs and calls through them; the implementation lives in extras/glTF and
// installs itself at startup:
//
//     // in the application, before the first prefab is loaded
//     import ZHLN.glTF;
//     ZHLN::glTF::RegisterPrefabLoader();
//
// With nothing registered, every prefab entry point reports the miss and
// returns null rather than crashing: a core-only build simply has no model
// files, the same way it has no JSON. The default `zahlen` executable never
// loads a .glb, so this changes nothing for it; the samples that do load one
// are the ones that register.
//
// Registration is a startup-time, single-threaded act (it belongs next to
// Engine creation). Reads are atomic so a system running on a worker fiber
// cannot observe a half-written table.

#include <Zahlen/Common.h>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN {

class RenderContext;
class CreativeWorksManager;
struct ModelPrefab;

namespace PrefabLoader {

/// What a model-file reader has to provide. Every member is required; a null
/// one is treated as "not available" by the caller rather than called.
struct Backend {
    /// Reads @p path (a virtual asset path) and caches the result on
    /// @p cwMgr. Returns null on failure, which is not an error state --
    /// a missing asset is a thing games do.
    auto (*Load)(RenderContext& ctx, CreativeWorksManager& cwMgr, std::string_view path) -> ModelPrefab* = nullptr;

    /// Same, from bytes already in memory, filed under @p virtualPath.
    auto (*LoadFromMemory)(RenderContext& ctx, CreativeWorksManager& cwMgr, std::span<const uint8_t> bytes, std::string_view virtualPath)
        -> ModelPrefab*                                                                                 = nullptr;

    /// Re-creates the GPU-side resources of an already-built prefab after a
    /// device loss. The CPU description is untouched.
    void (*RebuildGPUResources)(RenderContext& ctx, ModelPrefab* prefab)                                = nullptr;
};

/// Installs @p backend, replacing any previous one. The table is copied into
/// engine-owned storage, so the caller's object does not have to outlive the
/// call. Idempotent: registering the same backend twice is a no-op.
ZHLN_API void Register(const Backend& backend) noexcept;

/// The installed backend, or nullptr when no model-file reader is present.
[[nodiscard]] ZHLN_API auto Get() noexcept -> const Backend*;

/// Convenience for the common question: can this engine load model files?
[[nodiscard]] ZHLN_API bool IsAvailable() noexcept;

} // namespace PrefabLoader
} // namespace ZHLN
