// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "DrawCommands.hpp"
#include "GenerationalPool.hpp"
#include "Rendering.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace ZHLN {

enum class MaterialCreationError : uint8_t {
    ShaderCompilationFailed ZHLN_ANNOTATION(ZHLN::Description<"A material's shader stages could not be compiled"> {}) = 1,
    PipelineCreationFailed  ZHLN_ANNOTATION(ZHLN::Description<"A material's graphics pipeline could not be created"> {}),
    MaterialSlotsExhausted  ZHLN_ANNOTATION(ZHLN::Description<"No free material pipeline slots remain"> {}),
};

enum class MaterialPipelineFamily : uint8_t { Deferred, Forward };

class PipelineRegistry {
  public:
    PipelineRegistry(
        Vk::Context&             ctx,
        Vk::HeapManager&         heapManager,
        Vk::PipelineBinaryStore& pipelineBinaries,
        Vk::HeapMappingTable&    sceneHeapMappings,
        Vk::DeletionQueue&       deletionQueue
    ) noexcept:
        _ctx(ctx),
        _heapManager(heapManager),
        _pipelineBinaries(pipelineBinaries),
        _heapMappings(sceneHeapMappings),
        _deletionQueue(deletionQueue) {
    }
    ~PipelineRegistry() {
        RetireAll();
    }

    PipelineRegistry(const PipelineRegistry&)                        = delete;
    auto operator=(const PipelineRegistry&) -> PipelineRegistry&     = delete;
    PipelineRegistry(PipelineRegistry&&) noexcept                    = delete;
    auto operator=(PipelineRegistry&&) noexcept -> PipelineRegistry& = delete;

    template <
        MaterialPipelineFamily   Family,
        Vk::GraphicsShaderModule VertexShaderModule,
        Vk::GraphicsShaderModule MeshShaderModule,
        Vk::GraphicsPassContract PassContract>
    [[nodiscard]] auto CreateMaterial(Vk::MaterialFlags flags) -> std::expected<Material, ErrorCode> {
        static_assert(!VertexShaderModule::is_mesh_pipeline, "A material's primary pipeline must use vertex stages.");
        static_assert(MeshShaderModule::is_mesh_pipeline, "A material's optional mesh variant must use task/mesh stages.");
        static_assert(Family == MaterialPipelineFamily::Deferred || Family == MaterialPipelineFamily::Forward);

        const bool doubleSided = Vk::HasFlag(flags, Vk::MaterialFlags::DoubleSided);
        if constexpr (Family == MaterialPipelineFamily::Deferred) {
            if (Vk::MaterialFlagVariantIndex(flags) != 0) {
                return std::unexpected(MaterialCreationError::PipelineCreationFailed);
            }
            return CreateMaterialVariant<Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::None>(doubleSided);
        } else {
            switch (Vk::MaterialFlagVariantIndex(flags)) {
                case 0:
                    return CreateMaterialVariant<Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::None>(doubleSided);
                case 1:
                    return CreateMaterialVariant<Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::TranslucentBlend>(doubleSided);
                case 2:
                    return CreateMaterialVariant<
                        Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::TranslucentBlend | Vk::MaterialFlags::DepthWrite>(
                        doubleSided
                    );
                case 3:
                    return CreateMaterialVariant<Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::AdditiveBlend>(doubleSided);
                case 4:
                    return CreateMaterialVariant<
                        Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::AdditiveBlend | Vk::MaterialFlags::DepthWrite>(
                        doubleSided
                    );
                case 5:
                    return CreateMaterialVariant<Family, VertexShaderModule, MeshShaderModule, PassContract, Vk::MaterialFlags::DepthWrite>(doubleSided);
                default:
                    return std::unexpected(MaterialCreationError::PipelineCreationFailed);
            }
        }
    }

    void Destroy(PipelineHandle handle);

    void RetireAll() noexcept;

    [[nodiscard]] auto Resolve(PipelineHandle handle) const noexcept -> NativeMaterial* {
        return _materials.Resolve(handle);
    }

  private:
    static constexpr size_t kFamilyCount       = 2;
    static constexpr size_t kGeometryPathCount = 2;

    static constexpr size_t kPipelineVariantCount = kFamilyCount * kGeometryPathCount * Vk::k_material_flag_variant_count;
    static_assert(kPipelineVariantCount == 24);

    [[nodiscard]] static constexpr auto VariantIndex(MaterialPipelineFamily family, bool mesh, Vk::MaterialFlags flags) noexcept -> size_t {
        const size_t geometry = mesh ? 1U : 0U;
        return ((static_cast<size_t>(family) * kGeometryPathCount + geometry) * Vk::k_material_flag_variant_count) + Vk::MaterialFlagVariantIndex(flags);
    }

    template <MaterialPipelineFamily Family, Vk::GraphicsShaderModule ShaderModule, Vk::GraphicsPassContract PassContract, Vk::MaterialFlags Flags>
    [[nodiscard]] auto GetOrCreatePipeline() -> std::expected<VkPipeline, ErrorCode> {
        constexpr size_t stateIndex = Vk::MaterialFlagVariantIndex(Flags);
        static_assert(stateIndex < Vk::k_material_flag_variant_count);
        constexpr bool mesh  = ShaderModule::is_mesh_pipeline;
        const size_t   index = VariantIndex(Family, mesh, Flags);

        if constexpr (mesh) {
            if (!_ctx.MeshShadersSupported()) {
                return VK_NULL_HANDLE;
            }
        }

        Vk::Pipeline& cached = _sharedPipelines[index];
        if (cached.Valid()) {
            return cached.Get();
        }
        if constexpr (mesh) {
            if (_meshVariantAttempted[index]) {
                return VK_NULL_HANDLE;
            }
            _meshVariantAttempted[index] = true;
        }

        // The shared scene block is materialized per build, not stored: the
        // registry keeps the 32-byte rows, the arena keeps the driver structs
        // alive for exactly as long as the pipeline creation reads them.
        Vk::HeapMappingArena arena {_heapManager};
        const auto*          mapping = arena.Map(_heapMappings);
        const Vk::PipelineCreateBindings bindings {
            .vertexMapping   = mapping,
            .fragmentMapping = mapping,
        };
        auto created = Vk::GraphicsPipeline<ShaderModule, PassContract, Flags>::Create(_ctx.Device(), bindings, &_pipelineBinaries);
        if (!created) {
            return std::unexpected(created.error());
        }
        cached = std::move(*created);
        return cached.Get();
    }

    template <
        MaterialPipelineFamily   Family,
        Vk::GraphicsShaderModule VertexShaderModule,
        Vk::GraphicsShaderModule MeshShaderModule,
        Vk::GraphicsPassContract PassContract,
        Vk::MaterialFlags        Flags>
    [[nodiscard]] auto CreateMaterialVariant(bool doubleSided) -> std::expected<Material, ErrorCode> {
        auto vertexPipeline = GetOrCreatePipeline<Family, VertexShaderModule, PassContract, Flags>();
        if (!vertexPipeline) {
            return std::unexpected(MaterialCreationError::PipelineCreationFailed);
        }

        VkPipeline meshPipeline = VK_NULL_HANDLE;
        if (_ctx.MeshShadersSupported()) {
            auto meshResult = GetOrCreatePipeline<Family, MeshShaderModule, PassContract, Flags>();
            if (meshResult) {
                meshPipeline = *meshResult;
            } else {
                LogMeshPipelineFallback(meshResult.error());
            }
        }

        const PipelineHandle handle = _materials.Create();
        if (handle == PipelineHandle::Invalid) {
            return std::unexpected(MaterialCreationError::MaterialSlotsExhausted);
        }

        NativeMaterial* material = _materials.Resolve(handle);
        if (material == nullptr) [[unlikely]] {
            _materials.Destroy(handle);
            return std::unexpected(MaterialCreationError::MaterialSlotsExhausted);
        }
        material->pipeline     = *vertexPipeline;
        material->meshPipeline = meshPipeline;
        return Material {
            .pipeline    = handle,
            .alphaMode   = (Vk::HasFlag(Flags, Vk::MaterialFlags::TranslucentBlend) || Vk::HasFlag(Flags, Vk::MaterialFlags::AdditiveBlend)) ? 2U : 0U,
            .doubleSided = doubleSided,
        };
    }

    void LogMeshPipelineFallback(ErrorCode error) const noexcept;

    Vk::Context&                                    _ctx;
    Vk::HeapManager&                                _heapManager;
    Vk::PipelineBinaryStore&                        _pipelineBinaries;
    Vk::HeapMappingTable&                           _heapMappings;
    Vk::DeletionQueue&                              _deletionQueue;
    std::array<Vk::Pipeline, kPipelineVariantCount> _sharedPipelines {};
    std::array<bool, kPipelineVariantCount>         _meshVariantAttempted {};

    GenerationalPool<NativeMaterial, 2048, PipelineHandle> _materials;
};

static_assert(std::is_same_v<decltype(std::declval<const PipelineRegistry&>().Resolve(std::declval<PipelineHandle>())), NativeMaterial*>);

} // namespace ZHLN
