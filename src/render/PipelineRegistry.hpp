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
        Vk::Context&           ctx,
        Vk::PipelineCache&     pipelineCache,
        Vk::HeapMappingBundle& sceneHeapMappings,
        Vk::GPUDiagnostics&    gpuDiagnostics,
        Vk::DeletionQueue&     deletionQueue,
        VkPipelineLayout       emptyPipelineLayout
    ) noexcept:
        _ctx(ctx), _pipelineCache(pipelineCache), _heapMappings(sceneHeapMappings), _diagnostics(gpuDiagnostics), _deletionQueue(deletionQueue),
        _layout(emptyPipelineLayout) {
    }
    ~PipelineRegistry() {
        RetireAll();
    }

    PipelineRegistry(const PipelineRegistry&)                        = delete;
    auto operator=(const PipelineRegistry&) -> PipelineRegistry&     = delete;
    PipelineRegistry(PipelineRegistry&&) noexcept                    = delete;
    auto operator=(PipelineRegistry&&) noexcept -> PipelineRegistry& = delete;

    // Shader groups, pass attachments, and blend/depth variants are compile-time
    // inputs. Cull mode is recorded dynamically per draw, so sidedness does not
    // multiply the shared pipeline cache.
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
                default:
                    return std::unexpected(MaterialCreationError::PipelineCreationFailed);
            }
        }
    }

    // Invalidates one material handle; its shared pipeline variant remains cached.
    void Destroy(PipelineHandle handle);
    // Retires all shared pipeline variants and invalidates registry handles.
    // At shutdown the owner drains the queue only after waiting for GPU idle.
    void RetireAll() noexcept;

    [[nodiscard]] auto Resolve(PipelineHandle handle) const noexcept -> NativeMaterial* {
        return _materials.Resolve(handle);
    }

  private:
    static constexpr size_t kFamilyCount       = 2;
    static constexpr size_t kGeometryPathCount = 2;
    // Deferred/forward x vertex/mesh x five blend/depth variants; sidedness is dynamic.
    static constexpr size_t kPipelineVariantCount = kFamilyCount * kGeometryPathCount * Vk::k_material_flag_variant_count;
    static_assert(kPipelineVariantCount == 20);

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

        RegisterShaderModules<ShaderModule>();
        const Vk::PipelineCreateBindings bindings {
            .layout          = _layout,
            .descriptorHeap  = true,
            .vertexMapping   = &_heapMappings.info,
            .fragmentMapping = &_heapMappings.info,
        };
        auto created = Vk::GraphicsPipeline<ShaderModule, PassContract, Flags>::Create(_ctx, bindings, _pipelineCache.Get());
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
        material->layout       = _layout;
        material->meshPipeline = meshPipeline;
        return Material {
            .pipeline    = handle,
            .alphaMode   = (Vk::HasFlag(Flags, Vk::MaterialFlags::TranslucentBlend) || Vk::HasFlag(Flags, Vk::MaterialFlags::AdditiveBlend)) ? 2U : 0U,
            .doubleSided = doubleSided,
        };
    }

    template <Vk::GraphicsShaderModule ShaderModule>
    void RegisterShaderModules() const noexcept {
        ShaderModule::ForEachModule([this](auto moduleTag) {
            using Module = typename decltype(moduleTag)::type;
            _diagnostics.RegisterShader(Vk::CreateShaderDesc<Module>(), Module::EntryPoint);
        });
    }

    void LogMeshPipelineFallback(ErrorCode error) const noexcept;

    Vk::Context&           _ctx;
    Vk::PipelineCache&     _pipelineCache;
    Vk::HeapMappingBundle& _heapMappings;
    Vk::GPUDiagnostics&    _diagnostics;
    Vk::DeletionQueue&     _deletionQueue;
    VkPipelineLayout       _layout;

    std::array<Vk::Pipeline, kPipelineVariantCount> _sharedPipelines {};
    std::array<bool, kPipelineVariantCount>         _meshVariantAttempted {};

    GenerationalPool<NativeMaterial, 2048, PipelineHandle> _materials;
};

// Pins the return type where a caller can see it break. Resolve is on the
// per-draw path -- every mesh in the frame goes through it -- and used to
// answer with a std::expected that twenty-odd call sites immediately reduced
// to a pointer with .value_or(nullptr). A nullptr is the answer now, and this
// is what stops it drifting back into a wrapper type nobody reads.
static_assert(std::is_same_v<decltype(std::declval<const PipelineRegistry&>().Resolve(std::declval<PipelineHandle>())), NativeMaterial*>);

} // namespace ZHLN
