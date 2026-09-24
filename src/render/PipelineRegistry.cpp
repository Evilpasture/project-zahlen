// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/PipelineRegistry.cpp

#include "PipelineRegistry.hpp"

#include <Zahlen/Log.hpp>
#include <utility>

namespace ZHLN {

auto PipelineRegistry::BuildMeshVariant(const PipelineDesc& desc) const noexcept -> Vk::Pipeline {
    if (!_ctx.MeshShadersSupported() || desc.meshShader.code == nullptr || desc.meshShader.size == 0) {
        return {};
    }

    auto shaders = Vk::ShaderStages::CreateMesh(_ctx.Device(), desc.taskShader, desc.meshShader, desc.fragShader);
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
        builder.DepthWrite(false);
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
    return Vk::ShaderStages::Create(_ctx.Device(), desc.vertexShader, desc.fragShader)
        .transform_error([](auto) -> ErrorCode { return MaterialCreationError::ShaderCompilationFailed; })
        .and_then([this, &desc](auto&& shaders) -> std::expected<Material, ErrorCode> {
            // The stage descriptor came from a generated module, so the entry
            // point is the module's own -- nothing here invents one.
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
                pipeline.DepthWrite(false);
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
                .transform([this, &desc](auto&& compiledPipeline) -> auto {
                    Vk::Pipeline meshPipeline = BuildMeshVariant(desc);

                    return Material {
                        .pipeline  = _materials.Create(std::forward<decltype(compiledPipeline)>(compiledPipeline), _layout, std::move(meshPipeline)),
                        .alphaMode = (desc.alphaBlend || desc.additiveBlend) ? 2u : 0u
                    };
                });
        });
}

} // namespace ZHLN
