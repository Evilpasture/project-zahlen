// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/PipelineRegistry.hpp
//
// The table of compiled material pipelines: compile a PipelineDesc, keep the
// result under a generational handle, and retire it. A Material carries that
// handle, so the handle table is what makes a material meaningful -- which is
// why this and not the render context owns it, and why GeometryManager
// deliberately left its material cache's retirement here.
//
//   * One material can hold TWO pipelines: the vertex pipeline always, plus a
//     task+mesh+fragment twin when the device supports mesh shading and the
//     description supplied mesh stages. The twin failing is not an error -- the
//     vertex pipeline stays the fallback and the draw path picks per call.
//   * Every pipeline is built against the descriptor-heap null layout and the
//     scene registry's heap mappings, so those are injected rather than
//     invented: a pipeline compiled against the wrong layout is a device fault,
//     not a validation warning.
//   * Injection only: the device context, the driver pipeline cache, the heap
//     mapping bundle and GPU diagnostics (which records which shader bytes a
//     pipeline was built from, so a hang can be attributed to a module).
//
// The named per-pass pipelines -- decal, line, CSG, the particle pair, the
// post-process chain -- are not here. They are one-per-pass singletons owned by
// the code that records into them, not entries in a table, and moving them
// would only relocate a field without establishing a registry.

#pragma once
#include "DrawCommands.hpp" // NativeMaterial: what a PipelineHandle resolves to
#include "GenerationalPool.hpp"
#include "PipelineDesc.hpp"
#include "Rendering.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Core/Description.hpp> // ZHLN_ANNOTATION: what a material failure says
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp> // Material: what a compiled pipeline is handed back as
#include <cstdint>
#include <expected>
#include <type_traits>

namespace ZHLN {

// Why a material could not be compiled. Moved here with the code that fails
// this way.
enum class MaterialCreationError : uint8_t {
    ShaderCompilationFailed ZHLN_ANNOTATION(ZHLN::Description<"A material's shader stages could not be compiled">{}) = 1,
    PipelineCreationFailed  ZHLN_ANNOTATION(ZHLN::Description<"A material's graphics pipeline could not be created">{}),
};

class PipelineRegistry {
  public:
    PipelineRegistry(
        Vk::Context&                ctx,
        Vk::PipelineCache&          pipelineCache,
        Vk::HeapMappingBundle&      sceneHeapMappings,
        Vk::GPUDiagnostics&         gpuDiagnostics,
        VkPipelineLayout            emptyPipelineLayout
    ) noexcept
        : _ctx(ctx), _pipelineCache(pipelineCache), _heapMappings(sceneHeapMappings), _diagnostics(gpuDiagnostics), _layout(emptyPipelineLayout) {}
    ~PipelineRegistry() = default;

    PipelineRegistry(const PipelineRegistry&)                = delete;
    auto operator=(const PipelineRegistry&) -> PipelineRegistry& = delete;
    PipelineRegistry(PipelineRegistry&&) noexcept            = delete;
    auto operator=(PipelineRegistry&&) noexcept -> PipelineRegistry& = delete;

    // Compiles the description into a Material: the vertex pipeline always, and
    // the mesh-shader twin when it can be built. The returned Material's
    // `pipeline` is a handle into this registry's table.
    [[nodiscard]] auto CreateMaterial(const PipelineDesc& desc) -> std::expected<Material, ErrorCode>;

    // Retires one compiled pipeline. Safe on an invalid handle.
    void Destroy(PipelineHandle handle) { _materials.Destroy(handle); }

    // Returns the pool's own expected, not a bare pointer: callers test the
    // result and distinguish "invalid handle" from "no such material", and
    // collapsing that here would turn a lookup failure into a null deref at the
    // draw site. Same shape as GeometryManager::Resolve.
    using ResolveError = GenerationalPool<NativeMaterial, 2048, PipelineHandle>::Error;
    [[nodiscard]] auto Resolve(PipelineHandle handle) const noexcept -> std::expected<NativeMaterial*, ResolveError> { return _materials.Resolve(handle); }

  private:
    // The task+mesh+fragment twin. Returns an invalid pipeline, not an error,
    // when mesh shading is unavailable or the description has no mesh stages.
    [[nodiscard]] auto BuildMeshVariant(const PipelineDesc& desc) const noexcept -> Vk::Pipeline;

    Vk::Context&           _ctx;
    Vk::PipelineCache&     _pipelineCache;
    Vk::HeapMappingBundle& _heapMappings;
    Vk::GPUDiagnostics&    _diagnostics;
    VkPipelineLayout       _layout;

    // 2048 live materials. Keyed by pipeline handle rather than by asset id: a
    // material is a compiled state object, and two assets may share one.
    GenerationalPool<NativeMaterial, 2048, PipelineHandle> _materials;
};

// The draw path writes `if (!res) ... res.value() ... res.value_or(nullptr)`
// against this, and every one of those is also valid or silently wrong on a
// bare pointer: `!ptr` compiles, and only `.value()` fails, three lines later in
// a translation unit that cannot be compiled without the generated shader ABI.
// Pinning the return type to the pool's own keeps that mistake a compile error
// in this header, which every consumer does compile.
static_assert(std::is_same_v<decltype(std::declval<const PipelineRegistry&>().Resolve(std::declval<PipelineHandle>())),
                             std::expected<NativeMaterial*, PipelineRegistry::ResolveError>>);

} // namespace ZHLN
