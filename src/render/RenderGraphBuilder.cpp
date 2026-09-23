// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include "Zahlen/Math3D.hpp"
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN {

// Frame-level binding sources
// Specializations of Vk::ResourceResolver for the tags the reflected
// GraphResources bundle does not supply: presentation depth (owned by the
// active destination), the shadow map (kept out of the bundle's metadata),
// the ping-ponged accumulation pair, and the swapchain image (three possible
// sources). Everything the frame graph binds comes from either one of these
// or the bundle itself -- Vk::ResourceBinder::AutoBind folds both in a single
// pass, replacing the old reflected loop plus per-tag external bindings.
// (Plain nested `namespace Vk`, not `namespace ZHLN::Vk`: the qualified form
// inside `namespace ZHLN` would define a new ZHLN::ZHLN::Vk namespace.)
namespace Vk {

template <>
struct ResourceResolver<Res_Depth> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_Depth>(impl.ActivePresentation().depthTarget);
    }
};

template <>
struct ResourceResolver<Res_ShadowMap> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_ShadowMap>(impl.graphResources.shadowMap);
    }
};

template <>
struct ResourceResolver<Res_AccumCurr> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_AccumCurr>(impl.frames.accumBuffers.Current());
    }
};

template <>
struct ResourceResolver<Res_AccumNext> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_AccumNext>(impl.frames.accumBuffers.Next());
    }
};

template <>
struct ResourceResolver<Res_Swapchain> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        if (impl.sceneTarget.has_value()) {
            const ImageSlice& target = impl.sceneTarget->image;
            return MakeRef<Res_Swapchain>(target.handle, target.view, target.Extent2D());
        }
        auto& dest = impl.ActivePresentation();
        if (dest.swapchain.Valid()) {
            const auto& sc = dest.swapchain.Get();
            // The image the frame's destination acquired, read from the
            // destination itself: the frame does not remember an image index
            // beside the window it belongs to.
            const uint32_t imageIndex = impl.destinations.ActiveImageIndex();
            return MakeRef<Res_Swapchain>(sc.images[imageIndex], sc.views[imageIndex], impl.graphResources.sceneColor.extent);
        }
        return MakeRef<Res_Swapchain>(
            dest.headlessColorTarget.image.Handle(), dest.headlessColorTarget.view.Get(), dest.headlessColorTarget.extent
        );
    }
};

} // namespace Vk

namespace {

struct PassFactory {
    RenderContext::Impl&                        self;
    uint32_t                                    fIdx;
    const RenderContext::Impl::PPPushConstants& pc;
    uint32_t                                    lightVariant;
    uint32_t                                    reflVariant;

    [[nodiscard]] auto RcpExtent(VkExtent2D e) const noexcept {
        return std::pair {1.0f / static_cast<float>(e.width), 1.0f / static_cast<float>(e.height)};
    }

    [[nodiscard]] auto BuildSceneResources() const noexcept {
        return SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> {
            .sceneColor = Vk::Assume<Vk::ColorWrite<Res_SceneColor>>(self.graphResources.sceneColor),
            .velocity   = Vk::Assume<Vk::ColorWrite<Res_Velocity>>(self.graphResources.velocityBuffer),
            .normRough  = Vk::Assume<Vk::ColorWrite<Res_NormRough>>(self.graphResources.normalRoughnessBuffer),
            .emissive   = Vk::Assume<Vk::ColorWrite<Res_Emissive>>(self.graphResources.emissiveBuffer),
            .depth      = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(self.ActivePresentation().depthTarget)
        };
    }

    [[nodiscard]] auto MakeMainPass1() const noexcept {
        return Vk::Passieren<
            "MainPass1", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::ColorWrite<Res_Emissive>,
            Vk::DepthStencilWrite<Res_Depth>>(
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
            // Generate down to kMaxGeneratedHiZMips levels only: every level
            // costs a full compute-to-compute pipeline barrier, and the
            // culling consumer clamps its sample level to maxHiZMipLevel
            // (derived from the same constant), so deeper mips were written
            // but never read.
            uint32_t mips = std::min(self.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

            for (uint32_t mip = 0; mip < mips; ++mip) {
                if (mip > 0) {
                    Vk::MemoryBarrier(
                        c, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead
                    );
                }

                uint32_t srcW = std::max(1u, width >> (mip == 0 ? 0 : mip - 1));
                uint32_t srcH = std::max(1u, height >> (mip == 0 ? 0 : mip - 1));
                uint32_t dstW = std::max(1u, width >> mip);
                uint32_t dstH = std::max(1u, height >> mip);

                struct PC {
                    float    rcpSrcWidth, rcpSrcHeight;
                    uint32_t srcWidth, srcHeight;
                    uint32_t isFirstPass;
                };
                PC hizPC = {1.0f / static_cast<float>(srcW), 1.0f / static_cast<float>(srcH), srcW, srcH, mip == 0 ? 1u : 0u};

                // VK_EXT_descriptor_heap: every mip reads a different pair of
                // views, so it gets its own block from the frame's partition.
                const Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL> outMip {
                    .handle   = self.graphResources.hizMap.image.Handle(),
                    .view     = self.graphResources.hizMap.mipViews[mip].Get(),
                    .extent   = {.width = self.graphResources.hizMap.extent.width, .height = self.graphResources.hizMap.extent.height, .depth = 1},
                    .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                    .format   = VK_FORMAT_R32_SFLOAT,
                    .viewInfo = &self.graphResources.hizMap.mipViewInfos[mip]
                };
                // The previous mip is the shader's sampled input and this pass's
                // storage output, so the graph holds it in GENERAL; mip 0 samples
                // the depth target instead.
                const Vk::ImageWrite inDepth =
                    mip == 0 ? Vk::ImageWrite {
                                   .view     = self.presenter.depthTarget.view.Get(),
                                   .layout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   .viewInfo = &self.presenter.depthTarget.viewInfo
                               } :
                               Vk::ImageWrite {
                                   .view     = self.graphResources.hizMap.mipViews[mip - 1].Get(),
                                   .layout   = VK_IMAGE_LAYOUT_GENERAL,
                                   .viewInfo = &self.graphResources.hizMap.mipViewInfos[mip - 1]
                               };
                const Vk::HeapBlockBase block =
                    self.heapManager.WriteHeapParameters<Shaders::Hiz>(
                        self.ctx, self.hizHeapBindings, Vk::Slot<"inDepth">(inDepth), Vk::Slot<"outDepth">(outMip)
                    );
                self.hizGeneratePass.DispatchHeapIndexedThreads<Shaders::Modules::HizGenerateCS>(self.ctx, c, block, dstW, dstH, 1, hizPC);
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

            const Vk::HeapBlockBase block = self.heapManager.WriteHeapParameters<Shaders::ClusterCulling>(
                self.ctx, self.clusterCullingHeapBindings,
                Vk::Slot<"in_Bounds">(self.clusterBoundsBuffer),
                Vk::Slot<"out_Grid">(self.frames.clusterGridBuffers[fIdx]),
                Vk::Slot<"out_IndexList">(self.frames.lightIndexListBuffers[fIdx]),
                Vk::Slot<"out_Counter">(self.frames.globalCounterBuffers[fIdx]),
                Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                Vk::Slot<"lights">(self.frames.lightStorageBuffers[fIdx])
            );

            // Both the logical grid and [numthreads] are reflected from Slang;
            // the host supplies no shader-specific dimensions.
            self.clusterCullingPass.DispatchHeapIndexed(self.ctx, c, block);

            // Cluster grid / light-index SSBO writes are invisible to the frame
            // graph (this pass declares no image usages). Lighting and volumetric
            // inject read them on the compute/graphics queues.
            Vk::MemoryBarrier(
                c, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead
            );
        });
    }

    [[nodiscard]] auto MakeMainPass2() const noexcept {
        return Vk::Passieren<
            "MainPass2", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::ColorWrite<Res_Emissive>,
            Vk::DepthStencilWrite<Res_Depth>, Vk::ComputeRead<Res_HiZ>>([this](VkCommandBuffer c) noexcept {
            FrameRecorder mainRec(c, self);
            Passes::MainPass2 {}.Execute(mainRec, BuildSceneResources());
        });
    }

    // Shadow cascades. Declares *only* the shadow targets it writes; the
    // G-buffer work it used to inline is its own pass now. The two touch
    // disjoint resources, so the automatic forking in BuildFrameGraph
    // bundles them into one concurrently recorded run.
    [[nodiscard]] auto MakeShadowPass() const noexcept {
        return Vk::Passieren<"MainShadow", Vk::DepthWrite<Res_ShadowMap>, Vk::DepthWrite<Res_ShadowAtlas>>([this](VkCommandBuffer c) noexcept {
            // InheritsHeaps(): the same body records either straight into the
            // primary (no fork executor) or into a forked secondary that
            // inherits the primary's heap bindings.
            FrameRecorder shadowRec(c, self, self.InheritsHeaps());
            Passes::ShadowPass {}.Execute(shadowRec);
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

                self.particleUpdatePass.DispatchHeapThreads<Shaders::Modules::ParticleUpdateCS>(self.ctx, c, emitter.maxParticles, 1, 1, particlePC);
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

                self.meshParticleUpdatePass.DispatchHeapThreads<Shaders::Modules::MeshParticleUpdateCS>(self.ctx, c, emitter.maxParticles, 1, 1, pushPC);
            }
        });
    }

    [[nodiscard]] auto MakeVolumetricFogInjectPass() const noexcept {
        return Vk::MakePass<"VolumetricFogInject", Vk::ComputeWrite<Res_VoxelMedia>>([this](VkCommandBuffer c) noexcept {
            const Vk::HeapBlockBase block = self.volumetricFogInjectPass.WriteHeapParameters<Shaders::VolumetricFogInject>(
                self.ctx, self.heapManager,
                Vk::Slot<"outVoxelMedia">(Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(self.graphResources.voxelMedia)),
                // The 3D noise tile is a plain sampled image: its static sampler
                // lives in the sampler heap (InitHeapPassSamplers), the image is
                // named per block.
                Vk::Slot<"noiseTexture">(
                    Vk::ImageWrite {
                        .view = self.volumetricNoiseView.Get(), .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .viewInfo = &self.volumetricNoiseViewInfo
                    }
                ),
                Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                Vk::Slot<"fogVolumes">(self.frames.fogVolumesBuffer[fIdx])
            );

            RenderContext::Impl::VolumetricFogPushConstants fogPC = {};
            self.volumetricFogInjectPass.DispatchHeap<Shaders::Modules::VolumetricFogInjectCS>(self.ctx, c, block, fogPC);
        });
    }

    [[nodiscard]] auto MakeVolumetricLightInjectPass() const noexcept {
        return Vk::MakePass<"VolumetricLightInject", Vk::ComputeReadGeneral<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>, Vk::ComputeRead<Res_ShadowMap>>(
            [this](VkCommandBuffer c) noexcept {
                const Vk::HeapBlockBase block = self.volumetricLightInjectPass.WriteHeapParameters<Shaders::VolumetricLightInject>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"inVoxelMedia">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(self.graphResources.voxelMedia)),
                    Vk::Slot<"outVoxelLight">(Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(self.graphResources.voxelLight)),
                    Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                    Vk::Slot<"lights">(self.frames.lightStorageBuffers[fIdx]),
                    Vk::Slot<"clusterGrid">(self.frames.clusterGridBuffers[fIdx]),
                    Vk::Slot<"clusterIndexList">(self.frames.lightIndexListBuffers[fIdx]),
                    Vk::Slot<"shadowMap">(Vk::Assume<Vk::ComputeRead<Res_ShadowMap>>(self.graphResources.shadowMap))
                );
                RenderContext::Impl::VolumetricLightInjectPushConstants lightInjectPC = {};
                self.volumetricLightInjectPass.DispatchHeap<Shaders::Modules::VolumetricLightInjectCS>(self.ctx, c, block, lightInjectPC);
            }
        );
    }

    [[nodiscard]] auto MakeVolumetricIntegrationPass() const noexcept {
        return Vk::MakePass<"VolumetricIntegrate", Vk::ComputeReadGeneral<Res_VoxelLight>, Vk::ComputeWrite<Res_VoxelInt>>([this](VkCommandBuffer c) noexcept {
            const Vk::HeapBlockBase block = self.volumetricIntegrationPass.WriteHeapParameters<Shaders::VolumetricIntegration>(
                self.ctx, self.heapManager,
                Vk::Slot<"inVoxelLight">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(self.graphResources.voxelLight)),
                Vk::Slot<"outVoxelIntegrated">(Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(self.graphResources.voxelIntegrated))
            );
            self.volumetricIntegrationPass.DispatchHeap(self.ctx, c, block);
        });
    }

    [[nodiscard]] auto MakeVolumetricTemporalPass() const noexcept {
        return Vk::MakePass<
            "VolumetricTemporal", Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>>(
            [this](VkCommandBuffer c) noexcept {
                const Vk::HeapBlockBase block = self.volumetricTemporalPass.WriteHeapParameters<Shaders::VolumetricTemporal>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"inVoxelIntegratedCurrent">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(self.graphResources.voxelIntegrated)),
                    Vk::Slot<"inVoxelIntegratedHistory">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(self.graphResources.voxelHistory)),
                    Vk::Slot<"outVoxelIntegratedResolved">(Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(self.graphResources.voxelResolved)),
                    Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx])
                );
                RenderContext::Impl::VolumetricTemporalPushConstants temporalPC = {};

                self.volumetricTemporalPass.DispatchHeap<Shaders::Modules::VolumetricTemporalCS>(self.ctx, c, block, temporalPC);
            }
        );
    }

    [[nodiscard]] auto MakeGtaoPass() const noexcept {
        // Half-resolution GTAO horizon search for the AO-only GI modes
        // (giMode 3/4), split out of the lighting pass: the 4-slice loop is
        // the most expensive term in the inline ambient evaluation and its
        // result is low-frequency, so it is evaluated here at quarter pixel
        // count into a single-channel R8 target that lighting
        // depth-weighted-upsamples. Needs only the final G-buffer (depth +
        // normals), so it runs right before Lighting consumes the result.
        return Vk::MakePass<"GtaoAo", Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_NormRough>, Vk::ComputeWrite<Res_Ao>>(
            [this](VkCommandBuffer c) noexcept {
                const int giMode = self.settings.post.mode;
                if (giMode != 3 && giMode != 4) {
                    return;
                }
                self.BindHeapsAndPushFrame(c);

                const Vk::HeapBlockBase block = self.heapManager.WriteHeapParameters<Shaders::Gtao>(
                    self.ctx, self.gtaoHeapBindings,
                    Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.ActivePresentation().depthTarget)),
                    Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer)),
                    Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                    Vk::Slot<"outAo">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.ao))
                );

                const auto& fullExt = self.presenter.depthTarget.extent;
                RenderContext::Impl::GtaoPushConstants push {
                    .halfRes     = {self.graphResources.ao.extent.width, self.graphResources.ao.extent.height},
                    .rcpFullRes  = {1.0f / static_cast<float>(fullExt.width), 1.0f / static_cast<float>(fullExt.height)},
                    .time        = pc.camPos[3],
                    .aoRadius    = pc.aoRadius,
                    .aoBias      = pc.aoBias,
                    .aoPower     = pc.aoPower,
                    .giSamples   = static_cast<uint32_t>(pc.giSamples),
                    .invViewProj = pc.invViewProj,
                    .viewProj    = pc.viewProj,
                };
                self.gtaoCS.DispatchHeapIndexedThreads<Shaders::Modules::GtaoCS>(
                    self.ctx, c, block, self.graphResources.ao.extent.width, self.graphResources.ao.extent.height, 1, push
                );
            }
        );
    }

    [[nodiscard]] auto MakeLightingPass() const noexcept {
        return Vk::MakePass<
            "Lighting", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Emissive>, Vk::ShaderRead<Res_Depth>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderRead<Res_Ao>, Vk::ColorWrite<Res_Lighting>>([this](auto& ctx) noexcept {
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
            const Vk::AsAddressWrite tlas {
                .address = (self.rtCtx.Valid() && self.frames.tlas.Current() != VK_NULL_HANDLE) ?
                               self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) :
                               0
            };
            const Vk::HeapBlockBase block = self.lightingPass.WriteHeapParameters<Shaders::Lighting>(
                self.ctx, self.heapManager,
                Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor)),
                Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presenter.depthTarget)),
                Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer)),
                Vk::Slot<"lights">(self.frames.lightStorageBuffers[fIdx]),
                Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                Vk::Slot<"shadowMap">(Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(self.graphResources.shadowMap)),
                Vk::Slot<"ltc_mat">(ltcMatHeap),
                Vk::Slot<"ltc_amp">(ltcAmpHeap),
                Vk::Slot<"clusterGrid">(self.frames.clusterGridBuffers[fIdx]),
                Vk::Slot<"clusterIndexList">(self.frames.lightIndexListBuffers[fIdx]),
                Vk::Slot<"punctualShadowCube">(atlasCubeHeap),
                Vk::Slot<"punctualShadow2D">(atlas2DHeap),
                Vk::Slot<"blueNoiseTex">(blueNoiseHeap),
                Vk::Slot<"texEmissive">(Vk::Assume<Vk::ShaderRead<Res_Emissive>>(self.graphResources.emissiveBuffer)),
                Vk::Slot<"texAo">(Vk::Assume<Vk::ShaderRead<Res_Ao>>(self.graphResources.ao)),
                Vk::Slot<"tlas">(tlas)
            );
            self.lightingPass.ExecuteVariantHeap<Shaders::Modules::LightingPS, Shaders::Modules::LightingNortPS>(self.ctx, ctx.Cmd(), lightVariant, pc, block);
        });
    }

    [[nodiscard]] auto MakeRtrHalfTracePass() const noexcept {
        // Half-resolution RT reflection tracing for the VNDF roughness band
        // (0.04, 0.40]: the divergent lobe rays are traced here at quarter
        // count instead of per fragment, and the reflection pass bilinearly
        // upsamples the composed result. Runs after Lighting (the
        // reprojection fast path reads Res_Lighting) and before Reflection.
        return Vk::MakePass<
            "RtrHalfTrace", Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Lighting>, Vk::ComputeWrite<Res_RtrHalf>>(
            [this](VkCommandBuffer c) noexcept {
                // The pass exists iff the RT context did (BuildBloomPipelines
                // gates its creation the same way), so rtCtx remains the higher-
                // level feature guard even though the compute wrapper now also
                // exposes Valid().
                if (!self.rtCtx.Valid() || !self.settings.rayTracing.enableReflections || !self.settings.post.enableRTR) {
                    return;
                }
                self.BindHeapsAndPushFrame(c);

                auto& heap = self.heapManager;

                const auto blueNoiseHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                    .handle   = self.textureImages[self.blueNoiseTexIdx].Handle(),
                    .view     = self.textureViews[self.blueNoiseTexIdx].Get(),
                    .extent   = {.width = self.blueNoiseWidth, .height = self.blueNoiseHeight, .depth = 1},
                    .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                    .format   = VK_FORMAT_R8G8B8A8_UNORM,
                    .viewInfo = &self.blueNoiseViewInfo
                };
                const Vk::AsAddressWrite tlas {
                    .address = self.frames.tlas.Current() != VK_NULL_HANDLE ? self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) : 0
                };
                const Vk::HeapBlockBase block = heap.WriteHeapParameters<Shaders::RtrHalf>(
                    self.ctx, self.rtrHalfHeapBindings,
                    Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presenter.depthTarget)),
                    Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer)),
                    Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget)),
                    Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                    Vk::Slot<"g_instances">(self.frames.instanceDataBuffers[fIdx]),
                    Vk::Slot<"blueNoiseTex">(blueNoiseHeap),
                    Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.rtrHalf)),
                    Vk::Slot<"tlas">(tlas)
                );

                RenderContext::Impl::RtrHalfPushConstants push {
                    .halfRes = {self.graphResources.rtrHalf.extent.width, self.graphResources.rtrHalf.extent.height}, ._pad = {}
                };
                self.rtrHalfCS.DispatchHeapIndexedThreads<Shaders::Modules::RtrHalfCS>(
                    self.ctx, c, block, self.graphResources.rtrHalf.extent.width, self.graphResources.rtrHalf.extent.height, 1, push
                );
            }
        );
    }

    [[nodiscard]] auto MakeReflectionPass() const noexcept {
        return Vk::MakePass<
            "Reflection", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>, Vk::ShaderRead<Res_Lighting>,
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ShaderRead<Res_RtrHalf>,
            Vk::ColorWrite<Res_HdrSceneColor>>([this](auto& ctx) noexcept {
            const auto prefilteredHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.iblPayload.prefilteredImage.Handle(),
                .view     = self.iblPayload.prefilteredView.Get(),
                .extent   = {.width = 128, .height = 128, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.iblPayload.prefilteredViewInfo
            };
            const auto brdfLutHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.iblPayload.brdfLutImage.Handle(),
                .view     = self.iblPayload.brdfLutView.Get(),
                .extent   = {.width = 512, .height = 512, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.iblPayload.brdfLutViewInfo
            };
            const auto blueNoiseHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.textureImages[self.blueNoiseTexIdx].Handle(),
                .view     = self.textureViews[self.blueNoiseTexIdx].Get(),
                .extent   = {.width = self.blueNoiseWidth, .height = self.blueNoiseHeight, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.blueNoiseViewInfo
            };
            const Vk::AsAddressWrite tlas {
                .address = (self.rtCtx.Valid() && self.frames.tlas.Current() != VK_NULL_HANDLE) ?
                               self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) :
                               0
            };
            const Vk::HeapBlockBase block = self.reflectionPass.WriteHeapParameters<Shaders::Reflection>(
                self.ctx, self.heapManager,
                Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor)),
                Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presenter.depthTarget)),
                Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer)),
                Vk::Slot<"texEnvMap">(prefilteredHeap),
                Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                Vk::Slot<"brdfLUT">(brdfLutHeap),
                Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget)),
                Vk::Slot<"texVoxelIntegrated">(Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(self.graphResources.voxelResolved)),
                Vk::Slot<"g_instances">(self.frames.instanceDataBuffers[fIdx]),
                Vk::Slot<"blueNoiseTex">(blueNoiseHeap),
                Vk::Slot<"texRtrHalf">(Vk::Assume<Vk::ShaderRead<Res_RtrHalf>>(self.graphResources.rtrHalf)),
                Vk::Slot<"tlas">(tlas)
            );

            self.reflectionPass.ExecuteVariantHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(
                self.ctx, ctx.Cmd(), reflVariant, pc, block
            );
        });
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
            Vk::ShaderRead<Res_ShadowMap>, Vk::ShaderRead<Res_ShadowAtlas>, Vk::ShaderReadGeneral<Res_VoxelResolved>, Vk::ShaderRead<Res_RtrHalf>,
            Vk::ColorWrite<Res_TransLighting>>([this](auto& ctx) noexcept {
            const auto prefilteredHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.iblPayload.prefilteredImage.Handle(),
                .view     = self.iblPayload.prefilteredView.Get(),
                .extent   = {.width = 128, .height = 128, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.iblPayload.prefilteredViewInfo
            };
            const auto brdfLutHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.iblPayload.brdfLutImage.Handle(),
                .view     = self.iblPayload.brdfLutView.Get(),
                .extent   = {.width = 512, .height = 512, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.iblPayload.brdfLutViewInfo
            };
            const auto blueNoiseHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
                .handle   = self.textureImages[self.blueNoiseTexIdx].Handle(),
                .view     = self.textureViews[self.blueNoiseTexIdx].Get(),
                .extent   = {.width = self.blueNoiseWidth, .height = self.blueNoiseHeight, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R8G8B8A8_UNORM,
                .viewInfo = &self.blueNoiseViewInfo
            };
            const Vk::AsAddressWrite tlas {
                .address = (self.rtCtx.Valid() && self.frames.tlas.Current() != VK_NULL_HANDLE) ?
                               self.rtCtx.GetAccelerationStructureAddress(self.frames.tlas.Current()) :
                               0
            };
            const Vk::HeapBlockBase block = self.translucentReflectionPass.WriteHeapParameters<Shaders::Reflection>(
                self.ctx, self.heapManager,
                Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(self.graphResources.sceneColor)),
                Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_TransDepth>>(self.graphResources.transDepthBuffer)),
                Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_TransNorm>>(self.graphResources.transNormalBuffer)),
                Vk::Slot<"texEnvMap">(prefilteredHeap),
                Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx]),
                Vk::Slot<"brdfLUT">(brdfLutHeap),
                Vk::Slot<"texLighting">(Vk::Assume<Vk::ShaderRead<Res_Lighting>>(self.graphResources.lightingTarget)),
                Vk::Slot<"texVoxelIntegrated">(Vk::Assume<Vk::ShaderReadGeneral<Res_VoxelResolved>>(self.graphResources.voxelResolved)),
                Vk::Slot<"g_instances">(self.frames.instanceDataBuffers[fIdx]),
                Vk::Slot<"blueNoiseTex">(blueNoiseHeap),
                Vk::Slot<"texRtrHalf">(Vk::Assume<Vk::ShaderRead<Res_RtrHalf>>(self.graphResources.rtrHalf)),
                Vk::Slot<"tlas">(tlas)
            );
            self.translucentReflectionPass.ExecuteVariantHeap<Shaders::Modules::ReflectionPS, Shaders::Modules::ReflectionNortPS>(
                self.ctx, ctx.Cmd(), reflVariant, pc, block
            );
        });
    }

    [[nodiscard]] auto MakeForwardPass() const noexcept {
        auto& targetImage = self.graphResources.hdrSceneColor;
        return Vk::Passieren<"Forward", Vk::ColorWrite<Res_HdrSceneColor>, Vk::DepthStencilWrite<Res_Depth>, Vk::ShaderRead<Res_TransLighting>>(
            [this, &targetImage](VkCommandBuffer c) noexcept {
                FrameRecorder fwdRecorder(c, self);
                Passes::ForwardPass {}.Execute(
                    fwdRecorder, Vk::Assume<Vk::ColorWrite<Res_HdrSceneColor>>(targetImage),
                    Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(self.presenter.depthTarget)
                );
            }
        );
    }

    [[nodiscard]] auto MakeBloomPass() const noexcept {
        return Vk::MakePass<
            "BloomKawase", Vk::ComputeReadGeneral<Res_HdrSceneColor>, Vk::ComputeRead<Res_Emissive>, Vk::ComputeWrite<Res_BloomThresh>,
            Vk::ComputeWrite<Res_BloomDown1>, Vk::ComputeWrite<Res_BloomDown2>, Vk::ComputeWrite<Res_BloomDown3>, Vk::ComputeWrite<Res_BloomUp2>,
            Vk::ComputeWrite<Res_BloomUp1>, Vk::ComputeWrite<Res_BloomFinal>>([this](VkCommandBuffer c) noexcept {
            self.BindHeapsAndPushFrame(c);

            auto& heap = self.heapManager;

            // Everything stays in GENERAL layout for the whole chain: each
            // level is written by an imageStore and re-read as a sampled image
            // by the next dispatch, so only in-pass compute->compute barriers
            // separate the dispatches -- no render pass boundaries, no layout
            // ping-pong.
            const auto srcHdr     = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.hdrSceneColor);
            // Sampled in its post-lighting layout rather than dragged into
            // GENERAL with the rest of the chain: the bright pass only reads it.
            const auto emissive   = Vk::Assume<Vk::ComputeRead<Res_Emissive>>(self.graphResources.emissiveBuffer);
            const auto thresh     = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomThresholdTarget);
            const auto down1      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomDown1);
            const auto down2      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomDown2);
            const auto down3      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomDown3);
            const auto up2        = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomUp2);
            const auto up1        = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomUp1);
            const auto bloomFinal = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.bloomFinalTarget);

            // One ComputeChain per binding table: each allocates a block per
            // step and owns the barriers between its own steps. The chain is
            // three levels deep because there are three down and three up
            // targets to write -- a level is a graph resource, not a count --
            // so the steps below are spelled out one per target.
            Vk::ComputeChain thresholdChain(self.ctx, heap, c);
            Vk::ComputeChain downChain(self.ctx, heap, c);
            Vk::ComputeChain upChain(self.ctx, heap, c);

            const auto Kawase = [](int mode, const auto& src) noexcept {
                return RenderContext::Impl::KawasePushConstants {
                    .mode          = mode,
                    .rcpWidth      = 1.0f / static_cast<float>(src.extent.width),
                    .rcpHeight     = 1.0f / static_cast<float>(src.extent.height),
                    .glowIntensity = 0.0f
                };
            };

            // Only the bright pass reads the glow feed; the rest of the chain
            // is blurring whatever it produced.
            auto thresholdPush          = Kawase(0, self.graphResources.hdrSceneColor);
            thresholdPush.glowIntensity = std::max(self.settings.post.glowIntensity, 0.0f);

            // 0. Bright pass: HDR scene color -> half-res threshold target,
            //    plus the emission channel ungated (the glow layer -- see
            //    bloom_threshold_cs.slang).
            thresholdChain.Step<Shaders::BloomThreshold>(
                self.bloomThresholdCS, self.bloomThresholdHeapBindings, thresh.extent, thresholdPush,
                Vk::Slot<"texInput">(srcHdr),
                Vk::Slot<"texEmissive">(emissive),
                Vk::Slot<"outImage">(thresh)
            );

            // Separate heap tables, so separate chains: each prepends barriers
            // between its own steps. Cross-chain (threshold -> down, down -> up)
            // is a domain boundary and names the hazard explicitly.
            Vk::MemoryBarrier(
                c, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead
            );

            // 1-3. Downsample chain: thresh -> down1 -> down2 -> down3.
            downChain.Step<Shaders::BloomDown>(
                self.bloomDownCS, self.bloomDownHeapBindings, down1.extent, Kawase(0, thresh),
                Vk::Slot<"texInput">(thresh),
                Vk::Slot<"outImage">(down1)
            );
            downChain.Step<Shaders::BloomDown>(
                self.bloomDownCS, self.bloomDownHeapBindings, down2.extent, Kawase(0, down1),
                Vk::Slot<"texInput">(down1),
                Vk::Slot<"outImage">(down2)
            );
            downChain.Step<Shaders::BloomDown>(
                self.bloomDownCS, self.bloomDownHeapBindings, down3.extent, Kawase(0, down2),
                Vk::Slot<"texInput">(down2),
                Vk::Slot<"outImage">(down3)
            );

            Vk::MemoryBarrier(
                c, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead
            );

            // 4-6. Upsample chain with additive recombination of the same-
            //      resolution downsample stages.
            upChain.Step<Shaders::BloomUp>(
                self.bloomUpCS, self.bloomUpHeapBindings, up2.extent, Kawase(1, down3),
                Vk::Slot<"texInput">(down3),
                Vk::Slot<"texLow">(down2),
                Vk::Slot<"outImage">(up2)
            );
            upChain.Step<Shaders::BloomUp>(
                self.bloomUpCS, self.bloomUpHeapBindings, up1.extent, Kawase(1, up2),
                Vk::Slot<"texInput">(up2),
                Vk::Slot<"texLow">(down1),
                Vk::Slot<"outImage">(up1)
            );
            upChain.Step<Shaders::BloomUp>(
                self.bloomUpCS, self.bloomUpHeapBindings, bloomFinal.extent, Kawase(1, up1),
                Vk::Slot<"texInput">(up1),
                Vk::Slot<"texLow">(thresh),
                Vk::Slot<"outImage">(bloomFinal)
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
        // HdrSceneColor is a compute write (still GENERAL, same layout BloomKawase
        // then reads): the graph orders the write-back against bloom. The chain
        // prepends barriers between wavelet steps and never trails.
        return Vk::MakePass<
            "HdrDenoise", Vk::ComputeWrite<Res_HdrSceneColor>, Vk::ComputeWrite<Res_DenoiseA>, Vk::ComputeWrite<Res_DenoiseB>, Vk::ShaderRead<Res_Depth>,
            Vk::ShaderRead<Res_NormRough>>([this](VkCommandBuffer c) noexcept {
            const uint32_t passes = self.settings.rayTracing.denoiserPasses;
            const bool     active = self.rtCtx.Valid() && passes > 0 && (self.settings.rayTracing.enableShadows || self.settings.rayTracing.enableReflections);
            if (!active) {
                return;
            }
            self.BindHeapsAndPushFrame(c);

            auto& heap = self.heapManager;

            const auto hdr      = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.hdrSceneColor);
            const auto denoiseA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.denoiseA);
            const auto denoiseB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(self.graphResources.denoiseB);
            const auto depth    = Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presenter.depthTarget);
            const auto norm     = Vk::Assume<Vk::ShaderRead<Res_NormRough>>(self.graphResources.normalRoughnessBuffer);

            // Heap descriptor writes are immediate host writes, so each in-frame
            // iteration dispatches through its OWN block: the chain allocates one
            // per step, so a later iteration's descriptors cannot clobber an
            // earlier one's before the GPU has read them.
            Vk::ComputeChain atrousChain(self.ctx, heap, c);

            const auto Atrous = [](uint32_t stepSize) noexcept {
                return RenderContext::Impl::HdrAtrousPushConstants {.stepSize = stepSize, .phiDepth = 0.02f, .phiNormal = 16.0f, ._pad = 0u};
            };
            const auto Dispatch = [&](const auto& src, const auto& dst, uint32_t stepSize) noexcept {
                atrousChain.Step<Shaders::HdrDenoise>(
                    self.hdrDenoiseCS, self.hdrDenoiseHeapBindings, dst.extent, Atrous(stepSize),
                    Vk::Slot<"inColor">(src),
                    Vk::Slot<"texDepth">(depth),
                    Vk::Slot<"texNormalRoughness">(norm),
                    Vk::Slot<"outColor">(dst),
                    Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx])
                );
            };

            // Wavelet ladder: doubling tap spacing reaches a wide footprint
            // with narrow kernels, and the last dispatch always lands back on
            // hdrSceneColor, so bloom and the AA chain read a denoised scene
            // color without knowing the ladder ran. `denoiserPasses` picks the
            // shape: 1 runs scales 1, 2; 2 runs 1, 2, 2; the full ladder --
            // scales 1, 2, 4, three dispatches, the most any setting runs --
            // is what this default branch is.
            switch (passes) {
                case 1:
                    Dispatch(hdr, denoiseA, 1);
                    Dispatch(denoiseA, hdr, 2);
                    break;
                case 2:
                    Dispatch(hdr, denoiseA, 1);
                    Dispatch(denoiseA, denoiseB, 2);
                    Dispatch(denoiseB, hdr, 2);
                    break;
                default:
                    Dispatch(hdr, denoiseA, 1);
                    Dispatch(denoiseA, denoiseB, 2);
                    Dispatch(denoiseB, hdr, 4);
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

            // The decal PS needs invWorld * invViewProj per fragment; compose
            // it once per decal on the CPU instead. Must use the same
            // unjittered inverse the frame CB publishes, because that is the
            // matrix the depth-reconstruction it replaces was using.
            const JPH::Mat44 invViewProj = self.unjittered_view_proj.Inversed();

            for (const auto& decalCmd: self.queues.decalQueue) {
                RenderContext::Impl::DecalPushConstants decalPC {
                    .worldMatrix = decalCmd.transform,
                    .clipToLocal = decalCmd.invTransform * invViewProj,
                    .albedoIndex = decalCmd.albedoIndex,
                    .normalIndex = decalCmd.normalIndex,
                    .roughness   = decalCmd.roughness,
                    .metallic    = decalCmd.metallic
                };

                recorder.encoder.BindPipeline(self.decalPipeline.Get(), self.decalPipelineLayout);
                recorder.encoder.DrawHeap<Shaders::Modules::DecalVS, Shaders::Modules::DecalPS>(36, 1, decalPC);
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
                static_assert(GpuAbi::ScenePassPayload<TAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");
                const Vk::HeapBlockBase block = self.taaPass.WriteHeapParameters<Shaders::Taa>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"texCurrent">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)),
                    Vk::Slot<"texHistory">(Vk::Assume<Vk::ShaderRead<Res_AccumCurr>>(self.frames.accumBuffers.Current())),
                    Vk::Slot<"texVelocity">(Vk::Assume<Vk::ShaderRead<Res_Velocity>>(self.graphResources.velocityBuffer)),
                    Vk::Slot<"frame">(self.frames.frameUniformBuffers[fIdx])
                );

                self.taaPass.ExecuteHeap<Shaders::Modules::TaaPS>(self.ctx, c, TAAPushConstants {.feedback = self.settings.antiAliasing.taaFeedback}, block);
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
                static_assert(GpuAbi::ScenePassPayload<FXAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");
                const Vk::HeapBlockBase block = self.fxaaPass.WriteHeapParameters<Shaders::Fxaa>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
                );

                self.fxaaPass.ExecuteHeap<Shaders::Modules::FxaaPS>(
                    self.ctx, c,
                    FXAAPushConstants {
                        rcpW, rcpH, self.settings.antiAliasing.fxaaSubpix, self.settings.antiAliasing.fxaaEdgeThreshold,
                        self.settings.antiAliasing.fxaaEdgeThresholdMin, 0.0f
                    },
                    block
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
                static_assert(GpuAbi::ScenePassPayload<MLAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");
                const Vk::HeapBlockBase block = self.mlaaPass.WriteHeapParameters<Shaders::Mlaa>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
                );

                self.mlaaPass.ExecuteHeap<Shaders::Modules::MlaaPS>(
                    self.ctx, c, MLAAPushConstants {rcpW, rcpH, self.settings.antiAliasing.mlaaThreshold, self.settings.antiAliasing.mlaaMaxSearchSteps}, block
                );
            }
        });
    }

    [[nodiscard]] auto MakeSMAAEdgePass() const noexcept {
        return Vk::MakePass<"SmaaEdge", Vk::ShaderRead<Res_HdrSceneColor>, Vk::ColorWrite<Res_SmaaEdge>>([this](auto& ctx) noexcept {
            auto  c          = ctx.Cmd();
            auto& inputColor = self.graphResources.hdrSceneColor;
            if (self.smaaEdgePass.pipeline.Valid()) {
                auto [rcpW, rcpH] = RcpExtent(inputColor.extent);
                const RenderContext::Impl::SmaaPushConstants metrics {
                    .rtMetrics = {rcpW, rcpH, static_cast<float>(inputColor.extent.width),
                                  static_cast<float>(inputColor.extent.height)}
                };

                const Vk::HeapBlockBase block = self.smaaEdgePass.WriteHeapParameters<Shaders::SmaaEdge>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
                );
                self.smaaEdgePass.ExecuteHeap<Shaders::Modules::SmaaEdgeVS>(self.ctx, c, metrics, block);
            }
        });
    }

    [[nodiscard]] auto MakeSMAAWeightPass() const noexcept {
        return Vk::MakePass<"SmaaWeight", Vk::ShaderRead<Res_SmaaEdge>, Vk::ColorWrite<Res_SmaaWeight>>([this](auto& ctx) noexcept {
            auto c = ctx.Cmd();
            if (self.smaaWeightPass.pipeline.Valid()) {
                auto [rcpW, rcpH] = RcpExtent(self.graphResources.smaaWeightTarget.extent);
                const RenderContext::Impl::SmaaPushConstants metrics {
                    .rtMetrics = {rcpW, rcpH, static_cast<float>(self.graphResources.smaaWeightTarget.extent.width),
                                  static_cast<float>(self.graphResources.smaaWeightTarget.extent.height)}
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
                const Vk::HeapBlockBase block = self.smaaWeightPass.WriteHeapParameters<Shaders::SmaaWeight>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"edgesTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(self.graphResources.smaaEdgeTarget)),
                    Vk::Slot<"areaTex">(areaHeap),
                    Vk::Slot<"searchTex">(searchHeap)
                );
                self.smaaWeightPass.ExecuteHeap<Shaders::Modules::SmaaWeightVS, Shaders::Modules::SmaaWeightPS>(self.ctx, c, metrics, block);
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
                    const RenderContext::Impl::SmaaPushConstants metrics {
                        .rtMetrics = {rcpW, rcpH, static_cast<float>(inputColor.extent.width),
                                      static_cast<float>(inputColor.extent.height)}
                    };

                    const Vk::HeapBlockBase block = self.smaaBlendPass.WriteHeapParameters<Shaders::SmaaBlend>(
                        self.ctx, self.heapManager,
                        Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)),
                        Vk::Slot<"blendTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaWeight>>(self.graphResources.smaaWeightTarget))
                    );
                    self.smaaBlendPass.ExecuteHeap<Shaders::Modules::SmaaBlendVS, Shaders::Modules::SmaaBlendPS>(self.ctx, c, metrics, block);
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

                const Vk::HeapBlockBase block = self.blitPass.WriteHeapParameters<Shaders::Blit>(
                    self.ctx, self.heapManager,
                    Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<BlitInputRes>>(blitInputImage)),
                    Vk::Slot<"texBloom">(Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(self.graphResources.bloomFinalTarget)),
                    // blit.slang declares both of these without reading them, so
                    // the cook strips the bindings and the write is a no-op --
                    // the depth read is still what the pass declares to the
                    // graph, and both stay written for a shader that reads them.
                    Vk::Unread<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(self.presenter.depthTarget)),
                    Vk::Unread<"frame">(self.frames.frameUniformBuffers[fIdx])
                );

                // The overlay is drawn by the frame's present path, after this
                // blit, so this one leaves drawUI at its default.
                Passes::BlitPass {}.Execute(
                    blitRecorder, Vk::Assume<Vk::ShaderRead<BlitInputRes>>(blitInputImage), getSwapchainImage(), block,
                    self.currentUniforms.fullBright != 0 ? 1 : 0
                );
            }
        );
    }

    [[nodiscard]] auto MakeViewmodelPass() const noexcept {
        return Vk::Passieren<
            "Viewmodel", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>, Vk::ColorWrite<Res_Emissive>,
            Vk::DepthStencilWrite<Res_Depth>>(
            [this](VkCommandBuffer c) noexcept {
                FrameRecorder vmRec(c, self);
                Passes::ViewmodelPass {}.Execute(vmRec, BuildSceneResources());
            }
        );
    }
};

auto BuildComputeGraph(const PassFactory& factory) {
    // Automatic forking partitions this flat list at compile time (see
    // Vk::AutoForkPasses). This graph executes without a fork executor, so any
    // bundle it forms replays its bodies sequentially in order -- the bundling
    // is pure bookkeeping here, and the hazard check is what keeps it honest.
    return Vk::MakePassPack(
        factory.MakeClusterCullingPass(), factory.MakeVolumetricFogInjectPass(), factory.MakeVolumetricLightInjectPass(),
        factory.MakeVolumetricIntegrationPass(), factory.MakeVolumetricTemporalPass(), factory.MakeParticleUpdatePass(), factory.MakeMeshParticleUpdatePass()
    )
        .BuildGraph();
}

template <AAMode Mode, typename GetSwapchainImageT>
auto BuildFrameGraph(const PassFactory& factory, GetSwapchainImageT&& getSwapchainImage) {
    using enum AAMode;

    // The whole frame is a few flat pass packs, concatenated at compile time.
    // BuildGraph partitions the joined list (Vk::AutoForkPasses): every maximal
    // run of neighbour passes that are pairwise hazard-free (ArePassesDisjoint
    // over their declared usages) becomes one ParallelPass, so the graph emits
    // the union of the run's barriers up front and records its bodies
    // concurrently through the fork executor -- no hand-written Vk::Fork. A
    // run of one pass stays exactly as it was, so hazard-adjacent passes keep
    // their original stream behaviour.
    auto core = Vk::MakePassPack(
        factory.MakeShadowPass(), factory.MakeMainPass1(), factory.MakeHiZGeneratePass(),
        factory.MakeMainPass2(),   factory.MakeDecalPass(),   factory.MakeViewmodelPass(),
        factory.MakeTranslucentPrePass(), factory.MakeGtaoPass(),          factory.MakeLightingPass(),
        factory.MakeRtrHalfTracePass(),   factory.MakeReflectionPass(),    factory.MakeTranslucentReflectionPass(),
        factory.MakeForwardPass(), factory.MakeHdrDenoisePass(), factory.MakeBloomPass()
    );

    // The anti-aliasing tail; empty in mode None. Every branch is inside the
    // constexpr-if chain (an unguarded trailing return would be a second,
    // differently-typed return statement for every non-None mode).
    auto aa = [&] {
        if constexpr (Mode == TAA) {
            return Vk::MakePassPack(factory.MakeTAAPass());
        } else if constexpr (Mode == FXAA) {
            return Vk::MakePassPack(factory.MakeFXAAPass());
        } else if constexpr (Mode == MLAA) {
            return Vk::MakePassPack(factory.MakeMLAAPass());
        } else if constexpr (Mode == SMAA) {
            return Vk::MakePassPack(factory.MakeSMAAEdgePass(), factory.MakeSMAAWeightPass(), factory.MakeSMAABlendPass());
        } else {
            return Vk::MakePassPack();
        }
    }();

    auto blit = Vk::MakePassPack(factory.MakeBlitPass<Mode>(std::forward<GetSwapchainImageT>(getSwapchainImage)));

    return (std::move(core) + std::move(aa) + std::move(blit)).BuildGraph();
}

// Bind one target outside `AutoBind`, but only when the compiled graph
// actually declares the tag. `makeRef` is a callable rather than a value so
// the lookup is never instantiated — let alone evaluated — for a graph that
// does not use the tag.
template <typename Resources, typename Tag, typename Binder, typename RefFn>
void BindExternalReflected(Binder& binder, RefFn&& makeRef) {
    if constexpr (Vk::IsInList<Resources, Tag>::value) {
        auto ref = std::forward<RefFn>(makeRef)();
        binder.template Bind<Tag>(ref.handle, ref.view, ref.extent);
    }
}

template <AAMode Mode, typename GetSwapchainImageT>
void ExecuteFrameGraph(RenderContext::Impl& self, VkCommandBuffer cmd, const PassFactory& factory, GetSwapchainImageT&& getSwapchainImage) {
    auto graph = BuildFrameGraph<Mode>(factory, std::forward<GetSwapchainImageT>(getSwapchainImage));

    typename decltype(graph)::Binder binder;
    binder.AutoBind(self);

    auto* diagnostics = self.gpuDiagnostics.IsActive() ? &self.gpuDiagnostics : nullptr;
    graph.Execute(cmd, binder, self.presenter.frameIndex, &self.gpuProfiler, diagnostics, self.ForkExecutor());
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
    uint32_t               fIdx = presenter.frameIndex;

    BindHeapsAndPushFrame(compCmd);

    if (clusterBoundsDirty && clusterBoundsPass.Valid() && clusterBoundsPass.HasFixedDispatchDomain()) {
        // The pass dispatches only when the bounds are dirty, so its block is
        // written here rather than cached across frames.
        const Vk::HeapBlockBase block = heapManager.WriteHeapParameters<Shaders::ClusterBounds>(
            ctx, clusterBoundsHeapBindings, Vk::Slot<"out_Bounds">(clusterBoundsBuffer), Vk::Slot<"frame">(frames.frameUniformBuffers[fIdx])
        );
        clusterBoundsPass.DispatchHeapIndexed(ctx, compCmd, block);
        Vk::MemoryBarrier(
            compCmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead
        );
        clusterBoundsDirty = false;
    }

    PassFactory factory {
        .self = *this, .fIdx = fIdx, .pc = {}, .lightVariant = (settings.rayTracing.enableReflections && rtCtx.Valid()) ? 1u : 0u, .reflVariant = 0
    };

    auto compGraph = BuildComputeGraph(factory);
    typename decltype(compGraph)::Binder compBinder;
    using CompResources = typename decltype(compGraph)::Resources;

    compBinder.AutoBind(*this);

    // The compute graph reads last frame's shadow map, not the current one:
    // AutoBind resolved the tag from the frame's resolver, so the previous
    // frame's atlas overwrites that binding here.
    BindExternalReflected<CompResources, Res_ShadowMap>(compBinder, [&] { return Vk::MakeRef<Res_ShadowMap>(shadowMapPrev); });

    auto* diagnostics = gpuDiagnostics.IsActive() ? &gpuDiagnostics : nullptr;
    compGraph.Execute(compCmd, compBinder, presenter.frameIndex, &gpuProfiler, diagnostics);
}

void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& sceneSettings) {
    const uint32_t fIdx = presenter.frameIndex;

    using namespace ZHLN::Vk;
    using enum AAMode;

    // The scene's output goes exactly where the caller pointed the view: a
    // window's acquired image or an offscreen render texture. Falling back to
    // the active destination keeps a caller that rendered into a vended window
    // attachment without resolving it working unchanged.
    auto getSwapchainImage = [&]() -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
        // A destination is a slice and the layout is what this pass declares
        // over it, so the three ways a frame can have an image differ only in
        // where the slice comes from.
        if (sceneTarget.has_value()) {
            return sceneTarget->image.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
        }
        auto& dest = ActivePresentation();
        if (dest.swapchain.Valid()) {
            const auto&    sc         = dest.swapchain.Get();
            const uint32_t imageIndex = destinations.ActiveImageIndex();
            return MakeSlice(sc.images[imageIndex], sc.views[imageIndex], sc.extent, sc.format).Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
        }
        // The headless target is an owned RenderTarget, so the conversion that
        // already exists for one applies -- and it carries the view's
        // create-info, which a slice built from raw handles has none of.
        return AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(dest.headlessColorTarget);
    };

    const bool rtrActive    = sceneSettings.rayTracing.enableReflections && rtCtx.Valid();
    uint32_t   lightVariant = rtrActive ? 1 : 0;
    uint32_t   reflVariant  = (sceneSettings.post.enableSSR ? 1 : 0) | (rtrActive ? 2 : 0);

    // Pass constants are the view's optics, not the renderer's cached state:
    // the same frame may render two views, and each must push its own matrices.
    PassFactory factory {
        .self = *this,
        .fIdx = fIdx,
        .pc =
            {.invViewProj = view.invViewProjMatrix,
             .viewProj    = view.viewProjMatrix,
             .camPos      = {view.worldPosition.GetX(), view.worldPosition.GetY(), view.worldPosition.GetZ(), view.time},
             .giMode      = sceneSettings.post.mode,
             .aoRadius    = sceneSettings.post.aoRadius,
             .aoBias      = sceneSettings.post.aoBias,
             .aoPower     = sceneSettings.post.aoPower,
             .giIntensity = sceneSettings.post.giIntensity,
             .giSamples   = sceneSettings.post.giSamples,
             .enableSSR   = sceneSettings.post.enableSSR,
             .enableRTR   = (frames.tlas.Current() != VK_NULL_HANDLE && sceneSettings.rayTracing.enableReflections) ? sceneSettings.post.enableRTR : 0,
             ._pad        = {}},
        .lightVariant = lightVariant,
        .reflVariant  = reflVariant
    };

    DispatchAAMode(*this, cmd, sceneSettings.antiAliasing.mode, factory, getSwapchainImage);
}

} // namespace ZHLN
