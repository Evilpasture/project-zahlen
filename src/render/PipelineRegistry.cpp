// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "PipelineRegistry.hpp"

#include <Zahlen/Log.hpp>
#include <utility>

namespace ZHLN {

void PipelineRegistry::Retire(NativeMaterial& material) noexcept {
    if (material.pipeline != VK_NULL_HANDLE) {
        _deletionQueue.EnqueuePipeline(_ctx.Device(), std::exchange(material.pipeline, VK_NULL_HANDLE));
    }
    if (material.meshPipeline != VK_NULL_HANDLE) {
        _deletionQueue.EnqueuePipeline(_ctx.Device(), std::exchange(material.meshPipeline, VK_NULL_HANDLE));
    }
}

void PipelineRegistry::Destroy(PipelineHandle handle) {
    if (NativeMaterial* material = _materials.Resolve(handle)) {
        Retire(*material);
        _materials.Destroy(handle);
    }
}

void PipelineRegistry::RetireAll() noexcept {
    _materials.ForEachLive([this](NativeMaterial& material) { Retire(material); });
    _materials.Clear();
}

auto PipelineRegistry::BuildMeshVariant(const PipelineDesc& desc) const noexcept -> Vk::Pipeline {
    if (!_ctx.MeshShadersSupported() || desc.meshShader.code == nullptr || desc.meshShader.size == 0) {
        return {};
    }

    // PipelineDesc remains alive until the synchronous pipeline build returns.
    auto shaders = Vk::ShaderStagesView::CreateMesh(desc.taskShader, desc.meshShader, desc.fragShader);
    if (!shaders) {
        ZHLN::Log("[PipelineRegistry] Mesh-shader stage creation failed ({}); this material keeps the vertex pipeline.", shaders.error());
        return {};
    }

    _diagnostics.RegisterShader(desc.taskShader, desc.taskShader.entry_point != nullptr ? desc.taskShader.entry_point : "task");
    _diagnostics.RegisterShader(desc.meshShader, desc.meshShader.entry_point != nullptr ? desc.meshShader.entry_point : "mesh");

    auto builder = Vk::PipelineBuilder {}
                       .Shaders(*shaders)
                       .Layout(_layout)
                       .Cache(_pipelineCache.Get())
                       .HeapMappings(&_heapMappings.info, &_heapMappings.info)
                       .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT);

    if (desc.doubleSided) {
        builder.CullNone();
    } else {
        builder.CullBack();
    }

    if (desc.alphaBlend || desc.additiveBlend) {
        builder.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT});
        builder.DepthWrite(desc.depthWrite);
        if (desc.additiveBlend) {
            builder.AdditiveBlend();
        } else {
            builder.AlphaBlend();
        }
    } else {
        builder.ColorFormats(ActiveGBuffer::array);
    }

    auto pipeline = builder.Build(_ctx.Device());
    if (!pipeline) {
        ZHLN::Log("[PipelineRegistry] Mesh pipeline creation failed ({}); this material keeps the vertex pipeline.", pipeline.error());
        return {};
    }
    return std::move(*pipeline);
}

auto PipelineRegistry::CreateMaterial(const PipelineDesc& desc) -> std::expected<Material, ErrorCode> {
    return Vk::ShaderStagesView::Create(desc.vertexShader, desc.fragShader)
        .transform_error([](auto) -> ErrorCode { return MaterialCreationError::ShaderCompilationFailed; })
        .and_then([this, &desc](auto&& shaders) -> std::expected<Material, ErrorCode> {
            _diagnostics.RegisterShader(desc.vertexShader, desc.vertexShader.entry_point != nullptr ? desc.vertexShader.entry_point : "vertex");
            _diagnostics.RegisterShader(desc.fragShader, desc.fragShader.entry_point != nullptr ? desc.fragShader.entry_point : "fragment");

            auto pipeline = Vk::PipelineBuilder {}
                                .Shaders(shaders)
                                .Layout(_layout)
                                .Cache(_pipelineCache.Get())
                                .HeapMappings(&_heapMappings.info, &_heapMappings.info)
                                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT);

            if (desc.doubleSided) {
                pipeline.CullNone();
            } else {
                pipeline.CullBack();
            }

            if (desc.alphaBlend || desc.additiveBlend) {
                pipeline.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT});
                pipeline.DepthWrite(desc.depthWrite);
                if (desc.additiveBlend) {
                    pipeline.AdditiveBlend();
                } else {
                    pipeline.AlphaBlend();
                }
            } else {
                pipeline.ColorFormats(ActiveGBuffer::array);
            }

            if (desc.isLineList) {
                pipeline.Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
            }

            return pipeline.Build(_ctx.Device())
                .transform_error([](auto) -> ErrorCode { return MaterialCreationError::PipelineCreationFailed; })
                .and_then([this, &desc](Vk::Pipeline compiledPipeline) -> std::expected<Material, ErrorCode> {
                    // Builders retain synchronous RAII until a pool slot exists.
                    // An exhausted pool leaves both new pipelines to be destroyed
                    // here, before they can ever be referenced by GPU work.
                    Vk::Pipeline meshPipeline = BuildMeshVariant(desc);
                    const PipelineHandle handle = _materials.Create();
                    if (handle == PipelineHandle::Invalid) {
                        return std::unexpected(MaterialCreationError::MaterialSlotsExhausted);
                    }

                    NativeMaterial* material = _materials.Resolve(handle);
                    if (material == nullptr) [[unlikely]] {
                        // Unreachable for a handle Create() has just handed back,
                        // but Resolve's documented answer is null for every way a
                        // handle can fail to name a live object, and writing
                        // through it unchecked is the null dereference GCC says.
                        _materials.Destroy(handle);
                        return std::unexpected(MaterialCreationError::MaterialSlotsExhausted);
                    }
                    material->pipeline = compiledPipeline.Release();
                    material->layout = _layout;
                    material->meshPipeline = meshPipeline.Release();
                    return Material {
                        .pipeline    = handle,
                        .alphaMode   = (desc.alphaBlend || desc.additiveBlend) ? 2u : 0u,
                        .doubleSided = desc.doubleSided
                    };
                });
        });
}

}
