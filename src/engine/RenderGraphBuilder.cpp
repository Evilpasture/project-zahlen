// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "engine/Scheduler.hpp"
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
    VkCommandBuffer                             cmd;
    uint32_t                                    fIdx;
    VkDevice                                    device;
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
        return std::pair {1.0f / (float) e.width, 1.0f / (float) e.height};
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
        return Vk::MakePass<"HiZGenerate", Vk::ShaderRead<Res_Depth>, Vk::ComputeWrite<Res_HiZ>>([this](VkCommandBuffer cmd) noexcept {
            Profiler::ScopedGpuProfile timer(cmd, fIdx, self.gpuProfiler, Stage::HiZPass);

            uint32_t width  = self.graphResources.hizMap.extent.width;
            uint32_t height = self.graphResources.hizMap.extent.height;
            uint32_t mips   = self.graphResources.hizMap.mipLevels;

            for (uint32_t mip = 0; mip < mips; ++mip) {
                if (mip > 0) {
                    Vk::MemoryBarrier(
                        cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
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
                } pc = {1.0f / (float) srcW, 1.0f / (float) srcH, srcW, srcH, mip == 0 ? 1u : 0u};

                self.hizGeneratePass.Dispatch(cmd, self.hizSets[mip], (dstW + 15) / 16, (dstH + 15) / 16, 1, pc);
            }
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
            auto& rec = self.parallelRecorder.Current();
            rec.Reset();
            TaskSystemScheduler scheduler;
            rec.Record(
                scheduler,
                [&](Vk::RecordingSlot slot) noexcept {
                    FrameRecorder shadowRec(slot.cmd, self);
                    Passes::ShadowPass {}.Execute(shadowRec);
                },
                [&](Vk::RecordingSlot slot) noexcept {
                    FrameRecorder mainRec(slot.cmd, self);
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

            auto* bindlessSet = self.frames.bindlessSets[self.frame_index];

            for (const auto& emitter: self.queues.particleEmittersQueue) {
                auto* buffer = self.meshPool.Resolve(emitter.gpuBuffer).value_or(nullptr);
                if (!buffer) {
                    continue;
                }

                RenderContext::Impl::ComputePushConstants pc = {
                    .particleBufferAddr = self.ctx.BufferAddress(buffer->buffer.Handle()),
                    .particleCount      = emitter.maxParticles,
                    .deltaTime          = self.currentDt,
                    .p                  = emitter.params
                };

                self.particleUpdatePass.Dispatch(c, bindlessSet, (emitter.maxParticles + 63) / 64, 1, 1, pc);
            }
        });
    }

    [[nodiscard]] auto MakeMeshParticleUpdatePass() const noexcept {
        return Vk::MakePass<"MeshParticleUpdate">([this](VkCommandBuffer c) noexcept {
            if (!self.meshParticleUpdatePass.pipeline.Valid() || self.queues.meshParticleQueue.empty()) {
                return;
            }

            auto* bindlessSet = self.frames.bindlessSets[self.frame_index];

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

                self.meshParticleUpdatePass.Dispatch(c, bindlessSet, (emitter.maxParticles + 63) / 64, 1, 1, pushPC);
            }
        });
    }

    [[nodiscard]] auto MakeVolumetricClearPass() const noexcept {
        return Vk::MakePass<"VolumetricClear", Vk::ComputeWrite<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>>([this](VkCommandBuffer c) noexcept {
            Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::VolumetricClearPass);
            self.volumetricClearPass.WriteNext(
                device, Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(self.graphResources.voxelMedia),
                Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(self.graphResources.voxelLight)
            );
            self.volumetricClearPass.Dispatch(c, 160 / 8, (90 + 7) / 8, 64);
        });
    }

    [[nodiscard]] auto MakeVolumetricFogInjectPass() const noexcept {
        return Vk::MakePass<"VolumetricFogInject", Vk::ComputeWrite<Res_VoxelMedia>>([this](VkCommandBuffer c) noexcept {
            Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::VolumetricFogInjectPass);
            self.volumetricFogInjectPass.WriteNext(
                device, Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(self.graphResources.voxelMedia), self.frames.frameUniformBuffers[fIdx].Handle(),
                self.frames.fogVolumesBuffer[fIdx].Handle()
            );

            VolumetricFogInjectPushConstants pc = {};
            self.volumetricFogInjectPass.Dispatch(c, 160 / 8, (90 + 7) / 8, 64, pc);
        });
    }

    [[nodiscard]] auto MakeVolumetricLightInjectPass() const noexcept {
        return Vk::MakePass<"VolumetricLightInject", Vk::ComputeReadGeneral<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>, Vk::ComputeRead<Res_ShadowMap>>(
            [this](VkCommandBuffer c) noexcept {
                Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::VolumetricLightInjectPass);
                self.volumetricLightInjectPass.WriteNext(
                    device, Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(self.graphResources.voxelMedia),
                    Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(self.graphResources.voxelLight), self.frames.frameUniformBuffers[fIdx].Handle(),
                    self.frames.lightStorageBuffers[fIdx].Handle(), self.frames.clusterGridBuffers[fIdx].Handle(),
                    self.frames.lightIndexListBuffers[fIdx].Handle(), self.graphResources.shadowMap.view.Get(), self.shadowSampler.Get()
                );
                VolumetricLightInjectPushConstants pc = {};
                self.volumetricLightInjectPass.Dispatch(c, 160 / 8, (90 + 7) / 8, 64, pc);
            }
        );
    }

    [[nodiscard]] auto MakeVolumetricIntegrationPass() const noexcept {
        return Vk::MakePass<"VolumetricIntegrate", Vk::ComputeReadGeneral<Res_VoxelLight>, Vk::ComputeWrite<Res_VoxelInt>>([this](VkCommandBuffer c) noexcept {
            Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::VolumetricIntegratePass);
            self.volumetricIntegrationPass.WriteNext(
                device, Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(self.graphResources.voxelLight),
                Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(self.graphResources.voxelIntegrated)
            );
            self.volumetricIntegrationPass.Dispatch(c, 160 / 16, (90 + 8) / 9, 1);
        });
    }

    [[nodiscard]] auto MakeVolumetricTemporalPass() const noexcept {
        return Vk::MakePass<
            "VolumetricTemporal", Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>>(
            [this](VkCommandBuffer c) noexcept {
                Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::VolumetricTemporalPass);
                self.volumetricTemporalPass.WriteNext(
                    device, Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(self.graphResources.voxelIntegrated),
                    Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(self.graphResources.voxelHistory),
                    Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(self.graphResources.voxelResolved), self.frames.frameUniformBuffers[fIdx].Handle(),
                    self.defaultSampler.Get()
                );
                VolumetricTemporalPushConstants pc = {};

                self.volumetricTemporalPass.Dispatch(c, 160 / 8, (90 + 7) / 8, 64, pc);
            }
        );
    }

    [[nodiscard]] auto MakeAmbientPass() const noexcept {
        return Vk::MakePass<"Ambient", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_Ambient>>(
            [this](auto& ctx) noexcept {
                self.ambientPass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler.Get(),
                    Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget),
                    Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer), self.pointSampler.Get(),
                    self.iblPayload.prefilteredView.Get(), self.iblPayload.brdfLutView.Get(), self.clampSampler.Get(), self.frames.frameUniformBuffers->Handle()
                );
                self.ambientPass.Execute(ctx.Cmd(), pc);
            }
        );
    }

    [[nodiscard]] auto MakeLightingPass() const noexcept {
        return Vk::MakePass<
            "Lighting", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_Ambient>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ColorWrite<Res_Lighting>>([this](auto& ctx) noexcept {
            self.lightingPass.WriteNext(
                device, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler.Get(),
                Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget),
                Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer), self.frames.lightStorageBuffers->Handle(),
                self.frames.frameUniformBuffers->Handle(), self.graphResources.shadowMap.view.Get(), self.shadowSampler.Get(), self.ltcMatView.Get(),
                self.ltcAmpView.Get(), self.clampSampler.Get(), self.frames.clusterGridBuffers->Handle(), self.frames.lightIndexListBuffers->Handle(),
                Vk::Assume<Vk::ShaderRead<Res_Ambient>>(self.graphResources.ambientTarget), self.pointSampler.Get(), GetTLAS(), self.shadowAtlasCubeView.Get(),
                self.shadowAtlas2DView.Get()
            );
            self.lightingPass.ExecuteVariant(ctx.Cmd(), lightVariant, pc);
        });
    }

    [[nodiscard]] auto MakeReflectionPass() const noexcept {
        return Vk::MakePass<
            "Reflection", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_Lighting>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ColorWrite<Res_HdrSceneColor>>(
            [this](auto& ctx) noexcept {
                Profiler::ScopedGpuProfile timer(ctx.Cmd(), fIdx, self.gpuProfiler, Stage::PostProcessPass);
                self.reflectionPass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler.Get(),
                    Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget),
                    Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer), self.pointSampler.Get(),
                    self.iblPayload.prefilteredView.Get(), GetTLAS(), self.frames.frameUniformBuffers[fIdx].Handle(), self.iblPayload.brdfLutView.Get(),
                    self.clampSampler.Get(), Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget),
                    Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(self.graphResources.voxelResolved)
                );
                self.reflectionPass.ExecuteVariant(ctx.Cmd(), reflVariant, pc);
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
                Profiler::ScopedGpuProfile timer(ctx.Cmd(), fIdx, self.gpuProfiler, Stage::TransReflection);

                self.translucentReflectionPass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor), self.defaultSampler.Get(),
                    Vk::Assume<Vk::ShaderRead<Res_TransDepth>>(self.graphResources.transDepthBuffer),
                    Vk::Assume<Vk::ShaderRead<Res_TransNorm>>(self.graphResources.transNormalBuffer), self.pointSampler.Get(),
                    self.iblPayload.prefilteredView.Get(), GetTLAS(), self.frames.frameUniformBuffers[fIdx].Handle(), self.iblPayload.brdfLutView.Get(),
                    self.clampSampler.Get(), Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget),
                    Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(self.graphResources.voxelResolved)
                );
                self.translucentReflectionPass.ExecuteVariant(ctx.Cmd(), reflVariant, pc);
            }
        );
    }

    [[nodiscard]] auto MakeForwardPass() const noexcept {
        auto& targetImage = self.graphResources.hdrSceneColor;
        return Vk::Passieren<"Forward", Vk::ColorWrite<Res_HdrSceneColor>, Vk::DepthStencilWrite<Res_Depth>>([this, &targetImage](VkCommandBuffer c) noexcept {
            Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::ForwardPass);
            FrameRecorder              fwdRecorder(c, self);
            Passes::ForwardPass {}.Execute(
                fwdRecorder, Vk::Assume<Vk::ColorWrite<Res_HdrSceneColor>>(targetImage),
                Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(self.presentation.depthTarget)
            );
        });
    }

    [[nodiscard]] auto MakeBloomThresholdPass() const noexcept {
        const auto& inputColor = self.graphResources.hdrSceneColor;
        return Vk::MakePass<"BloomThreshold", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_BloomThresh>>([this, &inputColor](auto& ctx) noexcept {
            Profiler::ScopedGpuProfile timer(ctx.Cmd(), fIdx, self.gpuProfiler, Stage::BloomThreshPass);
            self.bloomThresholdPass.WriteNext(device, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler.Get());
            self.bloomThresholdPass.Execute(ctx.Cmd());
        });
    }

    template <size_t Index>
    [[nodiscard]] auto MakeBloomDownPass() const noexcept {
        if constexpr (Index == 0) {
            return Vk::MakePass<"BloomDown0", Vk::ShaderRead<Res_BloomThresh>, Vk::ColorWrite<Res_BloomDown1>>([this](auto& ctx) noexcept {
                RunKawasePass(device, ctx.Cmd(), self.bloomDownPass[0], self.graphResources.bloomThresholdTarget, self.defaultSampler);
            });
        } else if constexpr (Index == 1) {
            return Vk::MakePass<"BloomDown1", Vk::ShaderRead<Res_BloomDown1>, Vk::ColorWrite<Res_BloomDown2>>([this](auto& ctx) noexcept {
                RunKawasePass(device, ctx.Cmd(), self.bloomDownPass[1], self.graphResources.bloomDown1, self.defaultSampler);
            });
        } else {
            return Vk::MakePass<"BloomDown2", Vk::ShaderRead<Res_BloomDown2>, Vk::ColorWrite<Res_BloomDown3>>([this](auto& ctx) noexcept {
                RunKawasePass(device, ctx.Cmd(), self.bloomDownPass[2], self.graphResources.bloomDown2, self.defaultSampler);
            });
        }
    }

    template <size_t Index>
    [[nodiscard]] auto MakeBloomUpPass() const noexcept {
        if constexpr (Index == 2) {
            return Vk::MakePass<"BloomUp2", Vk::ShaderRead<Res_BloomDown3>, Vk::ShaderRead<Res_BloomDown2>, Vk::ColorWrite<Res_BloomUp2>>(
                [this](auto& ctx) noexcept {
                    RunKawasePass(device, ctx.Cmd(), self.bloomUpPass[2], self.graphResources.bloomDown3, self.defaultSampler, self.graphResources.bloomDown2);
                }
            );
        } else if constexpr (Index == 1) {
            return Vk::MakePass<"BloomUp1", Vk::ShaderRead<Res_BloomUp2>, Vk::ShaderRead<Res_BloomDown1>, Vk::ColorWrite<Res_BloomUp1>>(
                [this](auto& ctx) noexcept {
                    RunKawasePass(device, ctx.Cmd(), self.bloomUpPass[1], self.graphResources.bloomUp2, self.defaultSampler, self.graphResources.bloomDown1);
                }
            );
        } else {
            return Vk::MakePass<"BloomUp0", Vk::ShaderRead<Res_BloomUp1>, Vk::ShaderRead<Res_BloomThresh>, Vk::ColorWrite<Res_BloomFinal>>(
                [this](auto& ctx) noexcept {
                    RunKawasePass(
                        device, ctx.Cmd(), self.bloomUpPass[0], self.graphResources.bloomUp1, self.defaultSampler, self.graphResources.bloomThresholdTarget
                    );
                }
            );
        }
    }

    [[nodiscard]] auto MakeDecalPass() const noexcept {
        return Vk::MakePass<"DecalPass", Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_NormRough>>([this](auto& ctx) noexcept {
            auto c = ctx.Cmd();
            if (!self.decalPipeline.Valid() || self.queues.decalQueue.empty()) {
                return;
            }

            Profiler::ScopedGpuProfile timer(c, fIdx, self.gpuProfiler, Stage::DecalPass);

            DecalLayout::Write(self.ctx.Device(), self.decalSet, self.presentation.depthTarget.view.Get(), self.pointSampler.Get());

            FrameRecorder recorder(c, self);
            recorder.encoder.BindPipeline(self.decalPipeline.Get(), self.decalPipelineLayout.Get());

            std::array sets = {self.decalSet, self.frames.bindlessSets[fIdx]};
            recorder.encoder.BindDescriptorSets(0, sets);

            for (const auto& decalCmd: self.queues.decalQueue) {
                RenderContext::Impl::DecalPushConstants pc {
                    .world       = decalCmd.transform,
                    .invWorld    = decalCmd.invTransform,
                    .albedoIndex = decalCmd.albedoIndex,
                    .normalIndex = decalCmd.normalIndex,
                    .roughness   = decalCmd.roughness,
                    .metallic    = decalCmd.metallic
                };

                recorder.encoder.Draw(36, 1, pc);
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

                self.taaPass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor),
                    Vk::Assume<Vk::ShaderRead<Res_AccumCurr>>(self.frames.accumBuffers.Current()),
                    Vk::Assume<Vk::ShaderRead<Res_Velocity>>(self.graphResources.velocityBuffer), self.defaultSampler.Get(),
                    self.frames.frameUniformBuffers[fIdx].Handle()
                );

                self.taaPass.Execute(c, TAAPushConstants {.feedback = self.aaState.taaFeedback});
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

                self.fxaaPass.WriteNext(device, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler.Get());

                self.fxaaPass.Execute(
                    c, FXAAPushConstants {rcpW, rcpH, self.aaState.fxaaSubpix, self.aaState.fxaaEdgeThreshold, self.aaState.fxaaEdgeThresholdMin, 0.0f}
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

                self.mlaaPass.WriteNext(device, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler.Get());

                self.mlaaPass.Execute(c, MLAAPushConstants {rcpW, rcpH, self.aaState.mlaaThreshold, self.aaState.mlaaMaxSearchSteps});
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
                } metrics = {rcpW, rcpH, (float) inputColor.extent.width, (float) inputColor.extent.height};

                self.smaaEdgePass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor), self.defaultSampler.Get(), self.pointSampler.Get()
                );
                self.smaaEdgePass.Execute(c, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
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
                } metrics = {rcpW, rcpH, (float) self.graphResources.smaaWeightTarget.extent.width, (float) self.graphResources.smaaWeightTarget.extent.height};

                const auto& [areaView, searchView] = std::tie(self.textureViews[self.smaaAreaTexIdx], self.textureViews[self.smaaSearchTexIdx]);
                self.smaaWeightPass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(self.graphResources.smaaEdgeTarget), areaView, searchView, self.defaultSampler.Get(),
                    self.pointSampler.Get()
                );
                self.smaaWeightPass.Execute(c, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
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
                    } metrics = {rcpW, rcpH, (float) inputColor.extent.width, (float) inputColor.extent.height};

                    self.smaaBlendPass.WriteNext(
                        device, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor),
                        Vk::Assume<Vk::ShaderRead<Res_SmaaWeight>>(self.graphResources.smaaWeightTarget), self.defaultSampler.Get(), self.pointSampler.Get()
                    );
                    self.smaaBlendPass.Execute(c, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
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

                self.blitPass.WriteNext(
                    device, Vk::Assume<Vk::ShaderRead<BlitInputRes>>(blitInputImage), self.defaultSampler.Get(),
                    Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(self.graphResources.bloomFinalTarget),
                    Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presentation.depthTarget), self.frames.frameUniformBuffers[fIdx].Handle()
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

    template <typename SrcImgT, typename PassT>
    static void RunKawasePass(VkDevice device, VkCommandBuffer cmd, PassT& pass, const SrcImgT& src, const Vk::Sampler& defaultSampler) noexcept {
        pass.WriteNext(device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(src), defaultSampler.Get(), Vk::SkipWrite {});
        pass.Execute(
            cmd, RenderContext::Impl::KawasePushConstants {
                     .mode = 0, .rcpWidth = 1.0f / (float) src.extent.width, .rcpHeight = 1.0f / (float) src.extent.height, .padding = 0.0f
                 }
        );
    }

    template <typename SrcImgT, typename SrcImg2T, typename PassT>
    static void
        RunKawasePass(VkDevice device, VkCommandBuffer cmd, PassT& pass, const SrcImgT& src, const Vk::Sampler& defaultSampler, const SrcImg2T& src2) noexcept {
        pass.WriteNext(
            device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(src), defaultSampler.Get(),
            Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(src2)
        );
        pass.Execute(
            cmd, RenderContext::Impl::KawasePushConstants {
                     .mode = 1, .rcpWidth = 1.0f / (float) src.extent.width, .rcpHeight = 1.0f / (float) src.extent.height, .padding = 0.0f
                 }
        );
    }
};

auto BuildComputeGraph(const PassFactory& factory) {
    return Vk::CompileTimeFrameGraph(
        factory.MakeVolumetricClearPass(), factory.MakeVolumetricFogInjectPass(), factory.MakeVolumetricLightInjectPass(),
        factory.MakeVolumetricIntegrationPass(), factory.MakeVolumetricTemporalPass(), factory.MakeParticleUpdatePass(), factory.MakeMeshParticleUpdatePass()
    );
}

template <AAMode Mode, typename GetSwapchainImageT>
auto BuildFrameGraph(const PassFactory& factory, GetSwapchainImageT&& getSwapchainImage) {
    using enum AAMode;

    auto corePasses = std::tuple {factory.MakeShadowPass(),     factory.MakeHiZGeneratePass(),           factory.MakeMainPass2(),   factory.MakeDecalPass(),
                                  factory.MakeViewmodelPass(),  factory.MakeTranslucentPrePass(),        factory.MakeAmbientPass(), factory.MakeLightingPass(),
                                  factory.MakeReflectionPass(), factory.MakeTranslucentReflectionPass(), factory.MakeForwardPass()};

    auto bloomPasses = std::tuple {factory.MakeBloomThresholdPass(), factory.MakeBloomDownPass<0>(), factory.MakeBloomDownPass<1>(),
                                   factory.MakeBloomDownPass<2>(),   factory.MakeBloomUpPass<2>(),   factory.MakeBloomUpPass<1>(),
                                   factory.MakeBloomUpPass<0>()};

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
        const auto& sc = self.presentation.swapchain.Get();
        auto ref = Vk::MakeRef<Res_Swapchain>(sc.images[self.current_image_index], sc.views[self.current_image_index], self.graphResources.sceneColor.extent);
        binder.template Bind<Res_Swapchain>(ref.handle, ref.view, ref.extent);
    }

    graph.Execute(cmd, binder);
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

    PassFactory factory {
        .self         = *this,
        .cmd          = VK_NULL_HANDLE,
        .fIdx         = fIdx,
        .device       = ctx.Device(),
        .pc           = {},
        .lightVariant = (giSettings.enableRTR && rtCtx.Valid()) ? 1u : 0u,
        .reflVariant  = 0
    };

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

    compGraph.Execute(compCmd, compBinder);
}

void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
    uint32_t imageIdx = current_image_index;
    VkDevice device   = ctx.Device();
    uint32_t fIdx     = frame_index;

    using namespace ZHLN::Vk;
    using enum AAMode;

    auto getSwapchainImage = [&]() -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
        return {
            .handle = presentation.swapchain.Get().images[imageIdx],
            .view   = presentation.swapchain.Get().views[imageIdx],
            .extent = {.width = graphResources.sceneColor.extent.width, .height = graphResources.sceneColor.extent.height, .depth = 1},
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            .format = presentation.swapchain.Get().format
        };
    };

    uint32_t lightVariant = (giSettings.enableRTR && rtCtx.Valid()) ? 1 : 0;
    uint32_t reflVariant  = (giSettings.enableSSR ? 1 : 0) | ((giSettings.enableRTR && rtCtx.Valid()) ? 2 : 0);

    PassFactory factory {
        .self   = *this,
        .cmd    = cmd,
        .fIdx   = fIdx,
        .device = device,
        .pc =
            {.invViewProj = current_view_proj.Inversed(),
             .viewProj    = current_view_proj,
             .camPos      = {currentUniforms.camPos[0], currentUniforms.camPos[1], currentUniforms.camPos[2], currentUniforms.camPos[3]},
             .giMode      = giSettings.mode,
             .aoRadius    = giSettings.aoRadius,
             .aoBias      = giSettings.aoBias,
             .aoPower     = giSettings.aoPower,
             .giIntensity = giSettings.giIntensity,
             .giSamples   = giSettings.giSamples,
             .enableSSR   = giSettings.enableSSR,
             .enableRTR   = (frames.tlas.Current() != VK_NULL_HANDLE) ? giSettings.enableRTR : 0,
             ._pad        = {}},
        .lightVariant = lightVariant,
        .reflVariant  = reflVariant
    };

    DispatchAAMode(*this, cmd, aaState.mode, factory, getSwapchainImage);
}

} // namespace ZHLN
