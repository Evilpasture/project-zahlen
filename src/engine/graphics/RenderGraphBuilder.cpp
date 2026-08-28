// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../Scheduler.hpp"
#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN {

namespace {

struct PassFactory {
    RenderContext::Impl&                        self;
    uint32_t                                    fIdx;
    const RenderContext::Impl::PPPushConstants& pc;
    uint32_t                                    lightVariant;
    uint32_t                                    reflVariant;

    [[nodiscard]] auto GetTLAS() const noexcept {
        if constexpr (isMac) {
            return Vk::SkipWrite {};
        } else {
            return self.rtCtx.Valid() ? &self.frames.tlas.Current() : VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] auto RcpExtent(VkExtent2D e) const noexcept {
        return std::pair {1.0f / static_cast<float>(e.width), 1.0f / static_cast<float>(e.height)};
    }

    [[nodiscard]] auto BuildSceneResources() const noexcept {
        return SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> {
            .sceneColor = Vk::Assume<Vk::ColorWrite<Res_SceneColor>>(self.graphResources.sceneColor),
            .velocity   = Vk::Assume<Vk::ColorWrite<Res_Velocity>>(self.graphResources.velocityBuffer),
            .normRough  = Vk::Assume<Vk::ColorWrite<Res_NormRough>>(self.graphResources.normalRoughnessBuffer),
            .depth      = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(self.presentation.depthTarget)
        };
    }

    [[nodiscard]] auto MakeMainPass1() const noexcept {
        return Vk::Passieren<
            "MainPass1", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::DepthStencilWrite<Res_Depth>>(
            [this](VkCommandBuffer c) noexcept {
                FrameRecorder mainRec(c, self);
                Passes::MainPass1 {}.Execute(mainRec, BuildSceneResources());
            }
        );
    }

    [[nodiscard]] auto MakeHiZGeneratePass() const noexcept {
        return Vk::MakePass<"HiZGenerate", Vk::ShaderRead<Res_Depth>, Vk::ComputeWrite<Res_HiZ>>([this](VkCommandBuffer c) noexcept {
            uint32_t width  = self.graphResources.hizMap.extent.width;
            uint32_t height = self.graphResources.hizMap.extent.height;
            uint32_t mips   = self.graphResources.hizMap.mipLevels;

            for (uint32_t mip = 0; mip < mips; ++mip) {
                if (mip > 0) {
                    Vk::MemoryBarrier(
                        c, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                            .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
                    );
                }

                uint32_t srcW = std::max(1u, width >> (mip == 0 ? 0 : mip - 1));
                uint32_t srcH = std::max(1u, height >> (mip == 0 ? 0 : mip - 1));
                uint32_t dstW = std::max(1u, width >> mip);
                uint32_t dstH = std::max(1u, height >> mip);

                struct PC {
                    float    rcpW, rcpH;
                    uint32_t resW, resH;
                    uint32_t isFirstPass;
                } hizPC = {1.0f / static_cast<float>(srcW), 1.0f / static_cast<float>(srcH), srcW, srcH, mip == 0 ? 1u : 0u};

                // VK_EXT_descriptor_heap: the pushed index selects the mip slot span.
                self.hizGeneratePass.DispatchHeapIndexedThreads(self.ctx, c, mip, dstW, dstH, 1, hizPC);
            }
        });
    }

    [[nodiscard]] auto MakeClusterCullingPass() const noexcept {
        return Vk::MakePass<"ClusterCulling">([this](VkCommandBuffer c) noexcept {
            const auto& counterBuffer = self.frames.globalCounterBuffers[fIdx];

            Vk::FillBuffer(c, counterBuffer, 0, 0u);

            Vk::BufferBarrier(
                c, counterBuffer, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Compute,
                Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite
            );

            // Both the logical grid and [numthreads] are reflected from Slang;
            // the host supplies no shader-specific dimensions.
            self.clusterCullingPass.DispatchHeapIndexed(self.ctx, c, fIdx);

            Vk::MemoryBarrier(
                c, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                    .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
            );
        });
    }

    [[nodiscard]] auto MakeMainPass2() const noexcept {
        return Vk::Passieren<
            "MainPass2", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::DepthStencilWrite<Res_Depth>,
            Vk::ComputeRead<Res_HiZ>>([this](VkCommandBuffer c) noexcept {
            FrameRecorder mainRec(c, self);
            Passes::MainPass2 {}.Execute(mainRec, BuildSceneResources());
        });
    }

    [[nodiscard]] auto MakeShadowPass() const noexcept {
        return Vk::Passieren<
            "MainShadow", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::DepthStencilWrite<Res_Depth>,
            Vk::DepthWrite<Res_ShadowMap>, Vk::DepthWrite<Res_ShadowAtlas>>([this](VkCommandBuffer c) noexcept {
            const auto drawCount      = static_cast<uint32_t>(self.queues.drawQueue.size());
            const bool gpuCullingUsed = self.cullingPass.pipeline.Valid() && self.frames.indirectCommandsBuffers->Valid() &&
                                        (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling() && !self.MeshShadingActive();

            if (!gpuCullingUsed) {
                FrameRecorder shadowRec(c, self);
                Passes::ShadowPass {}.Execute(shadowRec);
                FrameRecorder mainRec(c, self);
                Passes::MainPass1 {}.Execute(mainRec, BuildSceneResources());
                return;
            }
            auto& rec = self.parallelRecorder.Current();
            rec.Reset();

            self.BindHeapsAndPushFrame(c);
            const auto samplerBind  = self.heapManager.GetSamplerHeapBindInfo();
            const auto resourceBind = self.heapManager.GetResourceHeapBindInfo();
            const auto frameAddrs   = self.FrameHeapAddresses();
            rec.SetHeapState(
                &samplerBind, &resourceBind, &self.ctx, self.heapPushDataLayout.frameAddressOffsets,
                std::span<const VkDeviceAddress> {frameAddrs.data(), frameAddrs.size()}
            );

            TaskSystemScheduler scheduler;
            rec.Record(
                scheduler,
                [&](Vk::RecordingSlot slot) noexcept {
                    FrameRecorder shadowRec(slot.cmd, self, true);
                    Passes::ShadowPass {}.Execute(shadowRec);
                },
                [&](Vk::RecordingSlot slot) noexcept {
                    FrameRecorder mainRec(slot.cmd, self, true);
                    Passes::MainPass1 {}.Execute(mainRec, BuildSceneResources());
                }
            );
            Vk::ExecuteCommands(c, rec.GetCommandBuffers());
        });
    }

    [[nodiscard]] auto MakeParticleUpdatePass() const noexcept {
        return Vk::MakePass<"ParticleUpdate">([this](VkCommandBuffer c) noexcept {
            if (!self.particleUpdatePass.pipeline.Valid() || self.queues.particleEmittersQueue.empty()) {
                return;
            }

            self.BindHeapsAndPushFrame(c);

            for (const auto& emitter: self.queues.particleEmittersQueue) {
                auto* buffer = self.meshPool.Resolve(emitter.gpuBuffer).value_or(nullptr);
                if (!buffer) {
                    continue;
                }

                RenderContext::Impl::ComputePushConstants particlePC = {
                    .particleBufferAddr = self.ctx.BufferAddress(buffer->buffer.Handle()),
                    .particleCount      = emitter.maxParticles,
                    .deltaTime          = self.currentDt,
                    .p                  = emitter.params
                };

                self.particleUpdatePass.DispatchHeapThreads(self.ctx, c, emitter.maxParticles, 1, 1, particlePC);
            }
        });
    }

    [[nodiscard]] auto MakeMeshParticleUpdatePass() const noexcept {
        return Vk::MakePass<"MeshParticleUpdate">([this](VkCommandBuffer c) noexcept {
            if (!self.meshParticleUpdatePass.pipeline.Valid() || self.queues.meshParticleQueue.empty()) {
                return;
            }

            self.BindHeapsAndPushFrame(c);

            for (const auto& emitter: self.queues.meshParticleQueue) {
                auto* buffer = self.meshPool.Resolve(emitter.gpuBuffer).value_or(nullptr);
                if (!buffer) {
                    continue;
                }

                RenderContext::Impl::MeshParticleComputePush pushPC = {
                    .particleBufferAddr = self.ctx.BufferAddress(buffer->buffer.Handle()),
                    .particleCount      = emitter.maxParticles,
                    .deltaTime          = self.currentDt,
                    .p                  = emitter.params
                };

                self.meshParticleUpdatePass.DispatchHeapThreads(self.ctx, c, emitter.maxParticles, 1, 1, pushPC);
            }
        });
    }

    [[nodiscard]] auto MakeVolumetricClearPass() const noexcept {
        return Vk::MakePass<"VolumetricClear", Vk::ComputeWrite<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>>([this](VkCommandBuffer c) noexcept {
            self.volumetricClearPass.WriteHeap(
                self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(self.graphResources.voxelMedia),
                Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(self.graphResources.voxelLight)
            );
            self.volumetricClearPass.DispatchHeap(self.ctx, c, fIdx);
        });
    }

    [[nodiscard]] auto MakeVolumetricFogInjectPass() const noexcept {
        return Vk::MakePass<"VolumetricFogInject", Vk::ComputeWrite<Res_VoxelMedia>>([this](VkCommandBuffer c) noexcept {
            // Noise texture/sampler are static and were written into both
            // descriptor-heap frames during initialization.
            self.volumetricFogInjectPass.WriteHeap(
                self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(self.graphResources.voxelMedia), Vk::SkipWrite {},
                Vk::SkipWrite {}, self.frames.frameUniformBuffers[fIdx], self.frames.fogVolumesBuffer[fIdx]
            );

            VolumetricFogPushConstants fogPC = {};
            self.volumetricFogInjectPass.DispatchHeap(self.ctx, c, fIdx, fogPC);
        });
    }

    [[nodiscard]] auto MakeVolumetricLightInjectPass() const noexcept {
        return Vk::MakePass<"VolumetricLightInject", Vk::ComputeReadGeneral<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>, Vk::ComputeRead<Res_ShadowMap>>(
            [this](VkCommandBuffer c) noexcept {
                self.volumetricLightInjectPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(self.graphResources.voxelMedia),
                    Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(self.graphResources.voxelLight), self.frames.frameUniformBuffers[fIdx],
                    self.frames.lightStorageBuffers[fIdx], self.frames.clusterGridBuffers[fIdx], self.frames.lightIndexListBuffers[fIdx],
                    Vk::Assume<Vk::ComputeRead<Res_ShadowMap>>(self.graphResources.shadowMap), self.shadowSampler
                );
                VolumetricLightInjectPushConstants lightInjectPC = {};
                self.volumetricLightInjectPass.DispatchHeap(self.ctx, c, fIdx, lightInjectPC);
            }
        );
    }

    [[nodiscard]] auto MakeVolumetricIntegrationPass() const noexcept {
        return Vk::MakePass<"VolumetricIntegrate", Vk::ComputeReadGeneral<Res_VoxelLight>, Vk::ComputeWrite<Res_VoxelInt>>([this](VkCommandBuffer c) noexcept {
            self.volumetricIntegrationPass.WriteHeap(
                self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(self.graphResources.voxelLight),
                Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(self.graphResources.voxelIntegrated)
            );
            self.volumetricIntegrationPass.DispatchHeap(self.ctx, c, fIdx);
        });
    }

    [[nodiscard]] auto MakeVolumetricTemporalPass() const noexcept {
        return Vk::MakePass<
            "VolumetricTemporal", Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>>(
            [this](VkCommandBuffer c) noexcept {
                self.volumetricTemporalPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(self.graphResources.voxelIntegrated),
                    Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(self.graphResources.voxelHistory),
                    Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(self.graphResources.voxelResolved), self.frames.frameUniformBuffers[fIdx],
                    self.defaultSampler
                );
                VolumetricTemporalPushConstants temporalPC = {};

                self.volumetricTemporalPass.DispatchHeap(self.ctx, c, fIdx, temporalPC);
            }
        );
    }

    [[nodiscard]] auto MakeLightingPass() const noexcept {
        return Vk::MakePass<
            "Lighting", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ColorWrite<Res_Lighting>>([this](auto& ctx) noexcept {
            const auto ltcMatHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.ltcMatImage.Handle(),
                .view     = self.ltcMatView.Get(),
                .extent   = {.width = 64, .height = 64, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
                .viewInfo = &self.ltcMatViewInfo
            };
            const auto ltcAmpHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.ltcAmpImage.Handle(),
                .view     = self.ltcAmpView.Get(),
                .extent   = {.width = 64, .height = 64, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
                .viewInfo = &self.ltcAmpViewInfo
            };
            const auto atlasCubeHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.graphResources.shadowAtlas.image.Handle(),
                .view     = self.shadowAtlasCubeView.Get(),
                .extent   = {.width = 1024, .height = 1024, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_DEPTH_BIT,
                .format   = VK_FORMAT_D32_SFLOAT,
                .viewInfo = &self.shadowAtlasCubeViewInfo
            };
            const auto atlas2DHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.graphResources.shadowAtlas.image.Handle(),
                .view     = self.shadowAtlas2DView.Get(),
                .extent   = {.width = 1024, .height = 1024, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_DEPTH_BIT,
                .format   = VK_FORMAT_D32_SFLOAT,
                .viewInfo = &self.shadowAtlas2DViewInfo
            };
            // Blue noise tile, matching the tail declaration in lighting.slang
            // (after pointSampler, before the reserved trailing TLAS slot).
            const auto blueNoiseHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.textureImages[self.blueNoiseTexIdx].Handle(),
                .view     = self.textureViews[self.blueNoiseTexIdx].Get(),
                .extent   = {.width = self.blueNoiseWidth, .height = self.blueNoiseHeight, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.blueNoiseViewInfo
            };
            self.lightingPass.WriteHeap(
                self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler,
                Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget),
                Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer), self.frames.lightStorageBuffers[fIdx],
                self.frames.frameUniformBuffers[fIdx], Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(self.graphResources.shadowMap), self.shadowSampler, ltcMatHeap,
                ltcAmpHeap, self.clampSampler, self.frames.clusterGridBuffers[fIdx], self.frames.lightIndexListBuffers[fIdx],
                self.pointSampler, atlasCubeHeap, atlas2DHeap, blueNoiseHeap, self.blueNoiseSampler,
                Vk::AsAddressWrite {
                    .address = (self.rtCtx.Valid() && self.frames.tlas.Current() != VK_NULL_HANDLE) ?
                                   self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) :
                                   0
                }
            );
            self.lightingPass.ExecuteVariantHeap(self.ctx, ctx.Cmd(), lightVariant, pc, fIdx);
        });
    }

    [[nodiscard]] auto MakeReflectionPass() const noexcept {
        return Vk::MakePass<
            "Reflection", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_Lighting>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ColorWrite<Res_HdrSceneColor>>(
            [this](auto& ctx) noexcept {
                self.reflectionPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler,
                    Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget),
                    Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer), self.pointSampler,
                    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                        .handle   = self.iblPayload.prefilteredImage.Handle(),
                        .view     = self.iblPayload.prefilteredView.Get(),
                        .extent   = {.width = 128, .height = 128, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R8G8B8A8_UNORM,
                        .viewInfo = &self.iblPayload.prefilteredViewInfo
                    },
                    self.frames.frameUniformBuffers[fIdx],
                    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                        .handle   = self.iblPayload.brdfLutImage.Handle(),
                        .view     = self.iblPayload.brdfLutView.Get(),
                        .extent   = {.width = 512, .height = 512, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R8G8B8A8_UNORM,
                        .viewInfo = &self.iblPayload.brdfLutViewInfo
                    },
                    self.clampSampler, Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget),
                    Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(self.graphResources.voxelResolved), self.frames.instanceDataBuffers[fIdx],
                    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                        .handle   = self.textureImages[self.blueNoiseTexIdx].Handle(),
                        .view     = self.textureViews[self.blueNoiseTexIdx].Get(),
                        .extent   = {.width = self.blueNoiseWidth, .height = self.blueNoiseHeight, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R8G8B8A8_UNORM,
                        .viewInfo = &self.blueNoiseViewInfo
                    },
                    self.blueNoiseSampler,
                    Vk::AsAddressWrite {
                        .address = (self.rtCtx.Valid() && self.frames.tlas.Current() != VK_NULL_HANDLE) ?
                                       self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) :
                                       0
                    }
                );

                self.reflectionPass.ExecuteVariantHeap(self.ctx, ctx.Cmd(), reflVariant, pc, fIdx);
            }
        );
    }

    [[nodiscard]] auto MakeTranslucentPrePass() const noexcept {
        return Vk::Passieren<"TransPrePass", Vk::ColorWrite<Res_TransNorm>, Vk::DepthStencilWrite<Res_TransDepth>>([this](VkCommandBuffer c) noexcept {
            FrameRecorder rec(c, self);
            Passes::TranslucentPrePass {}.Execute(
                rec, Vk::Assume<Vk::ColorWrite<Res_TransNorm>>(self.graphResources.transNormalBuffer),
                Vk::Assume<Vk::DepthStencilWrite<Res_TransDepth>>(self.graphResources.transDepthBuffer)
            );
        });
    }

    [[nodiscard]] auto MakeTranslucentReflectionPass() const noexcept {
        return Vk::MakePass<
            "TransReflection", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_TransNorm>, Vk::ShaderRead<Res_TransDepth>, Vk::ShaderRead<Res_Lighting>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ColorWrite<Res_TransLighting>>(
            [this](auto& ctx) noexcept {
                self.translucentReflectionPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler,
                    Vk::Assume<Vk::ShaderRead<Res_TransDepth>>(self.graphResources.transDepthBuffer),
                    Vk::Assume<Vk::ShaderRead<Res_TransNorm>>(self.graphResources.transNormalBuffer), self.pointSampler,
                    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                        .handle   = self.iblPayload.prefilteredImage.Handle(),
                        .view     = self.iblPayload.prefilteredView.Get(),
                        .extent   = {.width = 128, .height = 128, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R8G8B8A8_UNORM,
                        .viewInfo = &self.iblPayload.prefilteredViewInfo
                    },
                    self.frames.frameUniformBuffers[fIdx],
                    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                        .handle   = self.iblPayload.brdfLutImage.Handle(),
                        .view     = self.iblPayload.brdfLutView.Get(),
                        .extent   = {.width = 512, .height = 512, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R8G8B8A8_UNORM,
                        .viewInfo = &self.iblPayload.brdfLutViewInfo
                    },
                    self.clampSampler, Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget),
                    Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(self.graphResources.voxelResolved), self.frames.instanceDataBuffers[fIdx],
                    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                        .handle   = self.textureImages[self.blueNoiseTexIdx].Handle(),
                        .view     = self.textureViews[self.blueNoiseTexIdx].Get(),
                        .extent   = {.width = self.blueNoiseWidth, .height = self.blueNoiseHeight, .depth = 1},
                        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                        .format   = VK_FORMAT_R8G8B8A8_UNORM,
                        .viewInfo = &self.blueNoiseViewInfo
                    },
                    self.blueNoiseSampler,
                    Vk::AsAddressWrite {
                        .address = (self.rtCtx.Valid() && self.frames.tlas.Current() != VK_NULL_HANDLE) ?
                                       self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) :
                                       0
                    }
                );
                self.translucentReflectionPass.ExecuteVariantHeap(self.ctx, ctx.Cmd(), reflVariant, pc, fIdx);
            }
        );
    }

    [[nodiscard]] auto MakeForwardPass() const noexcept {
        auto& targetImage = self.graphResources.hdrSceneColor;
        return Vk::Passieren<"Forward", Vk::ColorWrite<Res_HdrSceneColor>, Vk::DepthStencilWrite<Res_Depth>, Vk::ShaderRead<Res_TransLighting>>(
            [this, &targetImage](VkCommandBuffer c) noexcept {
                FrameRecorder fwdRecorder(c, self);
                Passes::ForwardPass {}.Execute(
                    fwdRecorder, Vk::Assume<Vk::ColorWrite<Res_HdrSceneColor>>(targetImage),
                    Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(self.presentation.depthTarget)
                );
            }
        );
    }

    [[nodiscard]] auto MakeBloomPass() const noexcept {
        return Vk::MakePass<
            "BloomKawase", Vk::ComputeReadGeneral<Res_HdrSceneColor>, Vk::ComputeWrite<Res_BloomThresh>, Vk::ComputeWrite<Res_BloomDown1>,
            Vk::ComputeWrite<Res_BloomDown2>, Vk::ComputeWrite<Res_BloomDown3>, Vk::ComputeWrite<Res_BloomUp2>, Vk::ComputeWrite<Res_BloomUp1>,
            Vk::ComputeWrite<Res_BloomFinal>>([this](VkCommandBuffer c) noexcept {
            self.BindHeapsAndPushFrame(c);

            auto& heap = self.heapManager;

            // Everything stays in GENERAL layout for the whole chain: each
            // level is written by an imageStore and re-read as a sampled image
            // by the next dispatch, so only in-pass compute->compute barriers
            // separate the dispatches -- no render pass boundaries, no layout
            // ping-pong.
            const auto srcHdr = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.hdrSceneColor);
            const auto thresh = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomThresholdTarget);
            const auto down1  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomDown1);
            const auto down2  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomDown2);
            const auto down3  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomDown3);
            const auto up2    = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomUp2);
            const auto up1    = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomUp1);
            const auto bloomFinal = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomFinalTarget);

            const auto KawaseBarrier = [&c]() noexcept {
                Vk::MemoryBarrier(
                    c, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                        .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
                );
            };

            const auto Dispatch = [&](Vk::ComputePass& pass, const Vk::HeapPassBindings& bindings, const auto& dst, const auto& src, int mode) noexcept {
                heap.WriteBindings(self.ctx, bindings, fIdx, src, self.defaultSampler, dst);
                pass.DispatchHeapIndexedThreads(
                    self.ctx, c, fIdx, dst.extent.width, dst.extent.height, 1,
                    RenderContext::Impl::KawasePushConstants {
                        .mode      = mode,
                        .rcpWidth  = 1.0f / static_cast<float>(src.extent.width),
                        .rcpHeight = 1.0f / static_cast<float>(src.extent.height),
                        .padding   = 0.0f
                    }
                );
                KawaseBarrier();
            };

            // 0. Bright pass: HDR scene color -> half-res threshold target.
            heap.WriteBindings(self.ctx, self.bloomThresholdHeapBindings, fIdx, srcHdr, self.defaultSampler, thresh);
            self.bloomThresholdCS.DispatchHeapIndexedThreads(
                self.ctx, c, fIdx, thresh.extent.width, thresh.extent.height, 1,
                RenderContext::Impl::KawasePushConstants {
                    .mode = 0,
                    .rcpWidth  = 1.0f / static_cast<float>(self.graphResources.hdrSceneColor.extent.width),
                    .rcpHeight = 1.0f / static_cast<float>(self.graphResources.hdrSceneColor.extent.height),
                    .padding   = 0.0f
                }
            );
            KawaseBarrier();

            // 1-3. Downsample chain: thresh -> down1 -> down2 -> down3.
            Dispatch(self.bloomDownCS, self.bloomDownHeapBindings, down1, thresh, 0);
            Dispatch(self.bloomDownCS, self.bloomDownHeapBindings, down2, down1, 0);
            Dispatch(self.bloomDownCS, self.bloomDownHeapBindings, down3, down2, 0);

            // 4-6. Upsample chain with additive recombination of the same-
            //      resolution downsample stages.
            heap.WriteBindings(self.ctx, self.bloomUpHeapBindings, fIdx, down3, self.defaultSampler, down2, up2);
            self.bloomUpCS.DispatchHeapIndexedThreads(
                self.ctx, c, fIdx, up2.extent.width, up2.extent.height, 1,
                RenderContext::Impl::KawasePushConstants {
                    .mode      = 1,
                    .rcpWidth  = 1.0f / static_cast<float>(down3.extent.width),
                    .rcpHeight = 1.0f / static_cast<float>(down3.extent.height),
                    .padding   = 0.0f
                }
            );
            KawaseBarrier();

            heap.WriteBindings(self.ctx, self.bloomUpHeapBindings, fIdx, up2, self.defaultSampler, down1, up1);
            self.bloomUpCS.DispatchHeapIndexedThreads(
                self.ctx, c, fIdx, up1.extent.width, up1.extent.height, 1,
                RenderContext::Impl::KawasePushConstants {
                    .mode      = 1,
                    .rcpWidth  = 1.0f / static_cast<float>(up2.extent.width),
                    .rcpHeight = 1.0f / static_cast<float>(up2.extent.height),
                    .padding   = 0.0f
                }
            );
            KawaseBarrier();

            heap.WriteBindings(self.ctx, self.bloomUpHeapBindings, fIdx, up1, self.defaultSampler, thresh, bloomFinal);
            self.bloomUpCS.DispatchHeapIndexedThreads(
                self.ctx, c, fIdx, bloomFinal.extent.width, bloomFinal.extent.height, 1,
                RenderContext::Impl::KawasePushConstants {
                    .mode      = 1,
                    .rcpWidth  = 1.0f / static_cast<float>(up1.extent.width),
                    .rcpHeight = 1.0f / static_cast<float>(up1.extent.height),
                    .padding   = 0.0f
                }
            );
        });
    }

    // A-Trous wavelet denoise of the composited HDR color. Runs after the
    // reflection/forward passes have deposited their 1 SPP ray-traced grain
    // into hdrSceneColor and before bloom reads it. Ping-pongs through the
    // DenoiseA/B scratch targets and writes the final iteration back into
    // hdrSceneColor, so every downstream consumer (bloom, AA, blit) sees the
    // denoised result without changes.
    [[nodiscard]] auto MakeHdrDenoisePass() const noexcept {
        // HdrSceneColor is declared as a read (same as BloomKawase): the graph
        // transitions it COLOR_ATTACHMENT -> GENERAL on entry, and the final
        // write-back is ordered against bloom by the explicit compute barrier
        // every dispatch ends with.
        return Vk::MakePass<
            "HdrDenoise", Vk::ComputeReadGeneral<Res_HdrSceneColor>, Vk::ComputeWrite<Res_DenoiseA>, Vk::ComputeWrite<Res_DenoiseB>,
            Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_NormRough>>([this](VkCommandBuffer c) noexcept {
            // TEMPORARY bisect switch: the RT suites (TestLightingRayTraced,
            // TestRTRPBRReflection, isolated_08) render black frames while the
            // wavelet executes, but the mechanism is not yet pinned. Keep the
            // whole pass inert until that run is green, then flip this back to
            // true and iterate on the pass itself.
            constexpr bool kDenoiseDispatchEnabled = false;
            const uint32_t passes = self.settings.rayTracing.denoiserPasses;
            const bool active     = kDenoiseDispatchEnabled && self.rtCtx.Valid() && passes > 0 &&
                                (self.settings.rayTracing.enableShadows || self.settings.rayTracing.enableReflections);
            if (!active) {
                return;
            }
            self.BindHeapsAndPushFrame(c);

            auto& heap = self.heapManager;

            const auto hdr   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.hdrSceneColor);
            const auto dstA  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.denoiseA);
            const auto dstB  = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.denoiseB);
            const auto depth = Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget);
            const auto norm  = Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer);

            const auto AtrousBarrier = [&c]() noexcept {
                Vk::MemoryBarrier(
                    c, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                        .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
                );
            };

            const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t step) noexcept {
                heap.WriteBindings(self.ctx, self.hdrDenoiseHeapBindings, fIdx, src, depth, norm, dst, self.frames.frameUniformBuffers[fIdx]);
                self.hdrDenoiseCS.DispatchHeapIndexedThreads(
                    self.ctx, c, fIdx, dst.extent.width, dst.extent.height, 1,
                    RenderContext::Impl::HdrAtrousPushConstants {
                        .stepSize  = step,
                        .phiDepth  = 0.02f,
                        .phiNormal = 16.0f,
                        .pad       = 0u
                    }
                );
                AtrousBarrier();
            };

            // Wavelet ladder: doubling tap spacing reaches a wide footprint
            // with narrow kernels. The last dispatch always lands back on
            // hdrSceneColor.
            switch (passes) {
            case 1:
                Dispatch(hdr, dstA, 1);
                Dispatch(dstA, hdr, 2);
                break;
            case 2:
                Dispatch(hdr, dstA, 1);
                Dispatch(dstA, dstB, 2);
                Dispatch(dstB, hdr, 2);
                break;
            default:
                Dispatch(hdr, dstA, 1);
                Dispatch(dstA, dstB, 2);
                Dispatch(dstB, hdr, 4);
                break;
            }
        });
    }

    [[nodiscard]] auto MakeDecalPass() const noexcept {
        return Vk::MakePass<"DecalPass", Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_NormRough>>([this](auto& ctx) noexcept {
            auto c = ctx.Cmd();
            if (!self.decalPipeline.Valid() || self.queues.decalQueue.empty()) {
                return;
            }

            self.BindHeapsAndPushFrame(c);

            FrameRecorder recorder(c, self);
            recorder.encoder.BindPipeline(self.decalPipeline.Get(), self.decalPipelineLayout);

            for (const auto& decalCmd: self.queues.decalQueue) {
                RenderContext::Impl::DecalPushConstants decalPC {
                    .world       = decalCmd.transform,
                    .invWorld    = decalCmd.invTransform,
                    .albedoIndex = decalCmd.albedoIndex,
                    .normalIndex = decalCmd.normalIndex,
                    .roughness   = decalCmd.roughness,
                    .metallic    = decalCmd.metallic
                };

                recorder.encoder.BindPipeline(self.decalPipeline.Get(), self.decalPipelineLayout);
                recorder.encoder.DrawHeap(36, 1, decalPC);
            }
        });
    }

    [[nodiscard]] auto MakeTAAPass() const noexcept {
        return Vk::MakePass<
            "TAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_Velocity>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_AccumNext>,
            Vk::ShaderRead<Res_AccumCurr>>([this](auto& ctx) noexcept {
            auto  c          = ctx.Cmd();
            auto& inputColor = self.graphResources.hdrSceneColor;

            if (self.taaPass.pipeline.Valid()) {
                struct TAAPushConstants {
                    float feedback;
                };

                self.taaPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor),
                    Vk::Assume<Vk::ShaderRead<Res_AccumCurr>>(self.frames.accumBuffers.Current()),
                    Vk::Assume<Vk::ShaderRead<Res_Velocity>>(self.graphResources.velocityBuffer), self.defaultSampler, self.frames.frameUniformBuffers[fIdx]
                );

                self.taaPass.ExecuteHeap(self.ctx, c, TAAPushConstants {.feedback = self.settings.antiAliasing.taaFeedback}, fIdx);
            }
        });
    }

    [[nodiscard]] auto MakeFXAAPass() const noexcept {
        return Vk::MakePass<"FXAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_AccumNext>>([this](auto& ctx) noexcept {
            auto  c          = ctx.Cmd();
            auto& inputColor = self.graphResources.hdrSceneColor;

            if (self.fxaaPass.pipeline.Valid()) {
                auto [rcpW, rcpH] = RcpExtent(inputColor.extent);
                struct FXAAPushConstants {
                    float rcpFrameX;
                    float rcpFrameY;
                    float subpix;
                    float edgeThreshold;
                    float edgeThresholdMin;
                    float _pad;
                };

                self.fxaaPass.WriteHeap(self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler);

                self.fxaaPass.ExecuteHeap(
                    self.ctx, c,
                    FXAAPushConstants {rcpW, rcpH, self.settings.antiAliasing.fxaaSubpix, self.settings.antiAliasing.fxaaEdgeThreshold,
                                     self.settings.antiAliasing.fxaaEdgeThresholdMin, 0.0f}, fIdx
                );
            }
        });
    }

    [[nodiscard]] auto MakeMLAAPass() const noexcept {
        return Vk::MakePass<"MLAA", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_AccumNext>>([this](auto& ctx) noexcept {
            auto  c          = ctx.Cmd();
            auto& inputColor = self.graphResources.hdrSceneColor;

            if (self.mlaaPass.pipeline.Valid()) {
                auto [rcpW, rcpH] = RcpExtent(inputColor.extent);

                struct MLAAPushConstants {
                    float    rcpFrameX;
                    float    rcpFrameY;
                    float    threshold;
                    uint32_t maxSearchSteps;
                };

                self.mlaaPass.WriteHeap(self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler);

                self.mlaaPass.ExecuteHeap(self.ctx, c, MLAAPushConstants {rcpW, rcpH, self.settings.antiAliasing.mlaaThreshold, self.settings.antiAliasing.mlaaMaxSearchSteps}, fIdx);
            }
        });
    }

    [[nodiscard]] auto MakeSMAAEdgePass() const noexcept {
        return Vk::MakePass<"SmaaEdge", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_SmaaEdge>>([this](auto& ctx) noexcept {
            auto  c          = ctx.Cmd();
            auto& inputColor = self.graphResources.hdrSceneColor;
            if (self.smaaEdgePass.pipeline.Valid()) {
                auto [rcpW, rcpH] = RcpExtent(inputColor.extent);
                struct SMAAMetrics {
                    float rcpWidth, rcpHeight, width, height;
                } metrics = {rcpW, rcpH, static_cast<float>(inputColor.extent.width), static_cast<float>(inputColor.extent.height)};

                self.smaaEdgePass.WriteHeap(self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler);
                self.smaaEdgePass.ExecuteHeap(self.ctx, c, metrics, fIdx);
            }
        });
    }

    [[nodiscard]] auto MakeSMAAWeightPass() const noexcept {
        return Vk::MakePass<"SmaaWeight", Vk::ShaderRead<Res_SmaaEdge>, Vk::ColorWrite<Res_SmaaWeight>>([this](auto& ctx) noexcept {
            auto c = ctx.Cmd();
            if (self.smaaWeightPass.pipeline.Valid()) {
                auto [rcpW, rcpH] = RcpExtent(self.graphResources.smaaWeightTarget.extent);
                struct SMAAMetrics {
                    float rcpWidth, rcpHeight, width, height;
                } metrics = {
                    rcpW, rcpH, static_cast<float>(self.graphResources.smaaWeightTarget.extent.width),
                    static_cast<float>(self.graphResources.smaaWeightTarget.extent.height)
                };

                const auto& [areaView, searchView] = std::tie(self.textureViews[self.smaaAreaTexIdx], self.textureViews[self.smaaSearchTexIdx]);
                const auto areaInfo =
                    Vk::MakeViewCreateInfo2D(self.textureImages[self.smaaAreaTexIdx].Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                const auto searchInfo =
                    Vk::MakeViewCreateInfo2D(self.textureImages[self.smaaSearchTexIdx].Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
                const auto areaHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                    .handle   = self.textureImages[self.smaaAreaTexIdx].Handle(),
                    .view     = areaView.Get(),
                    .extent   = {.width = 160, .height = 560, .depth = 1},
                    .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                    .format   = VK_FORMAT_R8G8B8A8_UNORM,
                    .viewInfo = &areaInfo
                };
                const auto searchHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                    .handle   = self.textureImages[self.smaaSearchTexIdx].Handle(),
                    .view     = searchView.Get(),
                    .extent   = {.width = 64, .height = 16, .depth = 1},
                    .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                    .format   = VK_FORMAT_R8G8B8A8_UNORM,
                    .viewInfo = &searchInfo
                };
                self.smaaWeightPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(self.graphResources.smaaEdgeTarget), areaHeap, searchHeap,
                    self.defaultSampler
                );
                self.smaaWeightPass.ExecuteHeap(self.ctx, c, metrics, fIdx);
            }
        });
    }

    [[nodiscard]] auto MakeSMAABlendPass() const noexcept {
        return Vk::MakePass<"SmaaBlend", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ShaderRead<Res_SmaaWeight>, Vk::ColorWrite<Res_AccumNext>>(
            [this](auto& ctx) noexcept {
                auto  c          = ctx.Cmd();
                auto& inputColor = self.graphResources.hdrSceneColor;
                if (self.smaaBlendPass.pipeline.Valid()) {
                    auto [rcpW, rcpH] = RcpExtent(inputColor.extent);
                    struct SMAAMetrics {
                        float rcpWidth, rcpHeight, width, height;
                    } metrics = {rcpW, rcpH, static_cast<float>(inputColor.extent.width), static_cast<float>(inputColor.extent.height)};

                    self.smaaBlendPass.WriteHeap(
                        self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor),
                        Vk::Assume<Vk::ShaderRead<Res_SmaaWeight>>(self.graphResources.smaaWeightTarget), self.defaultSampler
                    );
                    self.smaaBlendPass.ExecuteHeap(self.ctx, c, metrics, fIdx);
                }
            }
        );
    }

    template <AAMode Mode, typename GetSwapchainImageT>
    auto MakeBlitPass(GetSwapchainImageT&& getSwapchainImage) const noexcept {
        using enum AAMode;
        using BlitInputRes         = std::conditional_t<Mode != None, Res_AccumNext, Res_HdrSceneColor>;
        const auto& blitInputImage = [&]() -> auto& {
            if constexpr (Mode != None) {
                return self.frames.accumBuffers.Next();
            } else {
                return self.graphResources.hdrSceneColor;
            }
        }();

        return Vk::Passieren<"Blit", Vk::ShaderRead<BlitInputRes>, Vk::ShaderRead<Res_BloomFinal>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_Swapchain>>(
            [this, &blitInputImage, getSwapchainImage = std::forward<GetSwapchainImageT>(getSwapchainImage)](VkCommandBuffer c) noexcept {
                FrameRecorder blitRecorder(c, self);

                self.blitPass.WriteHeap(
                    self.ctx, self.heapManager, fIdx, Vk::Assume<Vk::ShaderRead<BlitInputRes>>(blitInputImage), self.defaultSampler,
                    Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(self.graphResources.bloomFinalTarget),
                    Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget), self.frames.frameUniformBuffers[fIdx]
                );

                Passes::BlitPass {}.Execute(
                    blitRecorder, Vk::Assume<Vk::ShaderRead<BlitInputRes>>(blitInputImage), getSwapchainImage(), self.currentUniforms.fullBright != 0 ? 1 : 0
                );
            }
        );
    }

    [[nodiscard]] auto MakeViewmodelPass() const noexcept {
        return Vk::Passieren<
            "Viewmodel", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::DepthStencilWrite<Res_Depth>>(
            [this](VkCommandBuffer c) noexcept {
                FrameRecorder vmRec(c, self);
                Passes::ViewmodelPass {}.Execute(vmRec, BuildSceneResources());
            }
        );
    }
};

auto BuildComputeGraph(const PassFactory& factory) {
    return Vk::CompileTimeFrameGraph(
        factory.MakeClusterCullingPass(), factory.MakeVolumetricClearPass(), factory.MakeVolumetricFogInjectPass(), factory.MakeVolumetricLightInjectPass(),
        factory.MakeVolumetricIntegrationPass(), factory.MakeVolumetricTemporalPass(), factory.MakeParticleUpdatePass(), factory.MakeMeshParticleUpdatePass()
    );
}

template <AAMode Mode, typename GetSwapchainImageT>
auto BuildFrameGraph(const PassFactory& factory, GetSwapchainImageT&& getSwapchainImage) {
    using enum AAMode;

    auto corePasses = std::tuple {factory.MakeShadowPass(),     factory.MakeHiZGeneratePass(),           factory.MakeMainPass2(),   factory.MakeDecalPass(),
                                  factory.MakeViewmodelPass(),  factory.MakeTranslucentPrePass(),        factory.MakeLightingPass(),
                                  factory.MakeReflectionPass(), factory.MakeTranslucentReflectionPass(), factory.MakeForwardPass(),
                                  factory.MakeHdrDenoisePass()};

    auto bloomPasses = std::tuple {factory.MakeBloomPass()};

    auto tailPasses = [&] {
        if constexpr (Mode == TAA) {
            return std::tuple {factory.MakeTAAPass(), factory.MakeBlitPass<Mode>(std::forward<GetSwapchainImageT>(getSwapchainImage))};
        } else if constexpr (Mode == FXAA) {
            return std::tuple {factory.MakeFXAAPass(), factory.MakeBlitPass<Mode>(std::forward<GetSwapchainImageT>(getSwapchainImage))};
        } else if constexpr (Mode == MLAA) {
            return std::tuple {factory.MakeMLAAPass(), factory.MakeBlitPass<Mode>(std::forward<GetSwapchainImageT>(getSwapchainImage))};
        } else if constexpr (Mode == SMAA) {
            return std::tuple {
                factory.MakeSMAAEdgePass(), factory.MakeSMAAWeightPass(), factory.MakeSMAABlendPass(),
                factory.MakeBlitPass<Mode>(std::forward<GetSwapchainImageT>(getSwapchainImage))
            };
        } else {
            return std::tuple {factory.MakeBlitPass<Mode>(std::forward<GetSwapchainImageT>(getSwapchainImage))};
        }
    }();

    return std::apply(
        [](auto&&... passes) { return Vk::CompileTimeFrameGraph(std::move(passes)...); },
        std::tuple_cat(std::move(corePasses), std::move(bloomPasses), std::move(tailPasses))
    );
}

template <AAMode Mode, typename GetSwapchainImageT>
void ExecuteFrameGraph(RenderContext::Impl& self, VkCommandBuffer cmd, const PassFactory& factory, GetSwapchainImageT&& getSwapchainImage) {
    auto graph = BuildFrameGraph<Mode>(factory, std::forward<GetSwapchainImageT>(getSwapchainImage));

    typename decltype(graph)::Binder binder;
    using Resources = typename decltype(graph)::Resources;
    using GraphRes  = RenderContext::Impl::GraphResources;
    using Meta      = GraphRes::ReflectMetadata;

    Reflect::ForEachReflectedField<Meta>(self.graphResources, [&]<typename Tag>(auto& image) {
        if constexpr (Vk::IsInList<Resources, Tag>::value) {
            auto ref = Vk::MakeRef<Tag>(image);
            binder.template Bind<Tag>(ref.handle, ref.view, ref.extent);
        }
    });

    if constexpr (Vk::IsInList<Resources, Res_Depth>::value) {
        auto ref = Vk::MakeRef<Res_Depth>(self.presentation.depthTarget);
        binder.template Bind<Res_Depth>(ref.handle, ref.view, ref.extent);
    }
    if constexpr (Vk::IsInList<Resources, Res_ShadowMap>::value) {
        auto ref = Vk::MakeRef<Res_ShadowMap>(self.graphResources.shadowMap);
        binder.template Bind<Res_ShadowMap>(ref.handle, ref.view, ref.extent);
    }
    if constexpr (Vk::IsInList<Resources, Res_AccumCurr>::value) {
        auto ref = Vk::MakeRef<Res_AccumCurr>(self.frames.accumBuffers.Current());
        binder.template Bind<Res_AccumCurr>(ref.handle, ref.view, ref.extent);
    }
    if constexpr (Vk::IsInList<Resources, Res_AccumNext>::value) {
        auto ref = Vk::MakeRef<Res_AccumNext>(self.frames.accumBuffers.Next());
        binder.template Bind<Res_AccumNext>(ref.handle, ref.view, ref.extent);
    }
    if constexpr (Vk::IsInList<Resources, Res_Swapchain>::value) {
        if (self.presentation.swapchain.Valid()) {
            const auto& sc = self.presentation.swapchain.Get();
            auto        ref =
                Vk::MakeRef<Res_Swapchain>(sc.images[self.current_image_index], sc.views[self.current_image_index], self.graphResources.sceneColor.extent);
            binder.template Bind<Res_Swapchain>(ref.handle, ref.view, ref.extent);
        } else {
            auto ref = Vk::MakeRef<Res_Swapchain>(
                self.presentation.headlessColorTarget.image.Handle(), self.presentation.headlessColorTarget.view.Get(),
                self.presentation.headlessColorTarget.extent
            );
            binder.template Bind<Res_Swapchain>(ref.handle, ref.view, ref.extent);
        }
    }

    auto* diagnostics = self.gpuDiagnostics.IsActive() ? &self.gpuDiagnostics : nullptr;
    graph.Execute(cmd, binder, self.frame_index, &self.gpuProfiler, diagnostics);
}

template <typename Self, typename GetSwapchainImageT>
void DispatchAAMode(Self& self, VkCommandBuffer cmd, AAMode mode, const PassFactory& factory, GetSwapchainImageT&& getSwapchainImage) {
    Reflect::DispatchEnum(mode, [&]<AAMode Val>() { ExecuteFrameGraph<Val>(self, cmd, factory, std::forward<GetSwapchainImageT>(getSwapchainImage)); });
}

} // namespace

std::string_view GetRenderGraphDump(AAMode currentMode) noexcept {
    using enum AAMode;
    using Vk::Debug::GraphVisualizer;

    static constexpr auto vis_taa = GraphVisualizer<decltype(BuildFrameGraph<TAA>(std::declval<PassFactory>(), []() {
        return Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {};
    }))>::Visualize();

    static constexpr auto vis_smaa = GraphVisualizer<decltype(BuildFrameGraph<SMAA>(std::declval<PassFactory>(), []() {
        return Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {};
    }))>::Visualize();

    static constexpr auto vis_mlaa =
        GraphVisualizer<decltype(BuildFrameGraph<MLAA>(std::declval<PassFactory>(), []() -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
            return {};
        }))>::Visualize();

    static constexpr auto vis_fxaa = GraphVisualizer<decltype(BuildFrameGraph<FXAA>(std::declval<PassFactory>(), []() {
        return Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {};
    }))>::Visualize();

    static constexpr auto vis_none = GraphVisualizer<decltype(BuildFrameGraph<None>(std::declval<PassFactory>(), []() {
        return Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {};
    }))>::Visualize();

    switch (currentMode) {
        case TAA:
            return vis_taa.string_view();
        case SMAA:
            return vis_smaa.string_view();
        case FXAA:
            return vis_fxaa.string_view();
        case MLAA:
            return vis_mlaa.string_view();
        case None:
            return vis_none.string_view();
    }
    return "Not implemented.";
}

void RenderContext::Impl::RecordComputeFrame(Vk::CommandBuffer<Vk::QueueType::Compute> compCmd) {
    Vk::CommandBufferGuard guard(current_compute_cmd);
    uint32_t               fIdx = frame_index;

    BindHeapsAndPushFrame(compCmd);

    if (clusterBoundsDirty && clusterBoundsPass.pipeline.Valid() && clusterBoundsPass.fixedDispatchSize[0] != 0) {
        clusterBoundsPass.DispatchHeapIndexed(ctx, compCmd, fIdx);
        Vk::MemoryBarrier(
            compCmd, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
                      .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
        );
        clusterBoundsDirty = false;
    }

    PassFactory factory {.self = *this, .fIdx = fIdx, .pc = {}, .lightVariant = (settings.rayTracing.enableReflections && rtCtx.Valid()) ? 1u : 0u, .reflVariant = 0};

    auto                                 compGraph = BuildComputeGraph(factory);
    typename decltype(compGraph)::Binder compBinder;
    using CompResources = typename decltype(compGraph)::Resources;
    using Meta          = GraphResources::ReflectMetadata;

    Reflect::ForEachReflectedField<Meta>(graphResources, [&]<typename Tag>(auto& image) {
        if constexpr (Vk::IsInList<CompResources, Tag>::value) {
            auto ref = Vk::MakeRef<Tag>(image);
            compBinder.template Bind<Tag>(ref.handle, ref.view, ref.extent);
        }
    });

    if constexpr (Vk::IsInList<CompResources, Res_ShadowMap>::value) {
        auto ref = Vk::MakeRef<Res_ShadowMap>(shadowMapPrev);
        compBinder.template Bind<Res_ShadowMap>(ref.handle, ref.view, ref.extent);
    }

    auto* diagnostics = gpuDiagnostics.IsActive() ? &gpuDiagnostics : nullptr;
    compGraph.Execute(compCmd, compBinder, frame_index, &gpuProfiler, diagnostics);
}

void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
    uint32_t imageIdx = current_image_index;
    uint32_t fIdx     = frame_index;

    using namespace ZHLN::Vk;
    using enum AAMode;

    auto getSwapchainImage = [&]() -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
        if (presentation.swapchain.Valid()) {
            return {
                .handle = presentation.swapchain.Get().images[imageIdx],
                .view   = presentation.swapchain.Get().views[imageIdx],
                .extent = {.width = graphResources.sceneColor.extent.width, .height = graphResources.sceneColor.extent.height, .depth = 1},
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                .format = presentation.swapchain.Get().format
            };
        }
        return {
            .handle = presentation.headlessColorTarget.image.Handle(),
            .view   = presentation.headlessColorTarget.view.Get(),
            .extent = {.width = presentation.headlessColorTarget.extent.width, .height = presentation.headlessColorTarget.extent.height, .depth = 1},
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            .format = VK_FORMAT_R8G8B8A8_UNORM
        };
    };

    const bool rtrActive = settings.rayTracing.enableReflections && rtCtx.Valid();
    uint32_t lightVariant = rtrActive ? 1 : 0;
    uint32_t reflVariant  = (settings.post.enableSSR ? 1 : 0) | (rtrActive ? 2 : 0);

    PassFactory factory {
        .self = *this,
        .fIdx = fIdx,
        .pc =
            {.invViewProj = current_view_proj.Inversed(),
             .viewProj    = current_view_proj,
             .camPos      = {currentUniforms.camPos[0], currentUniforms.camPos[1], currentUniforms.camPos[2], currentUniforms.camPos[3]},
             .giMode      = settings.post.mode,
             .aoRadius    = settings.post.aoRadius,
             .aoBias      = settings.post.aoBias,
             .aoPower     = settings.post.aoPower,
             .giIntensity = settings.post.giIntensity,
             .giSamples   = settings.post.giSamples,
             .enableSSR   = settings.post.enableSSR,
             .enableRTR   = (frames.tlas.Current() != VK_NULL_HANDLE && settings.rayTracing.enableReflections) ? settings.post.enableRTR : 0,
             ._pad        = {}},
        .lightVariant = lightVariant,
        .reflVariant  = reflVariant
    };

    DispatchAAMode(*this, cmd, settings.antiAliasing.mode, factory, getSwapchainImage);
}

} // namespace ZHLN
