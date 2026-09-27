// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "DrawCommands.hpp"
#include "GenerationalPool.hpp"
#include "PipelineDesc.hpp"
#include "Rendering.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace ZHLN {

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

    [[nodiscard]] auto CreateMaterial(const PipelineDesc& desc) -> std::expected<Material, ErrorCode>;

    void Destroy(PipelineHandle handle) { _materials.Destroy(handle); }

    using ResolveError = GenerationalPool<NativeMaterial, 2048, PipelineHandle>::Error;
    [[nodiscard]] auto Resolve(PipelineHandle handle) const noexcept -> std::expected<NativeMaterial*, ResolveError> { return _materials.Resolve(handle); }

  private:
    [[nodiscard]] auto BuildMeshVariant(const PipelineDesc& desc) const noexcept -> Vk::Pipeline;

    Vk::Context&           _ctx;
    Vk::PipelineCache&     _pipelineCache;
    Vk::HeapMappingBundle& _heapMappings;
    Vk::GPUDiagnostics&    _diagnostics;
    VkPipelineLayout       _layout;

    GenerationalPool<NativeMaterial, 2048, PipelineHandle> _materials;
};

static_assert(std::is_same_v<decltype(std::declval<const PipelineRegistry&>().Resolve(std::declval<PipelineHandle>())),
                             std::expected<NativeMaterial*, PipelineRegistry::ResolveError>>);

}
