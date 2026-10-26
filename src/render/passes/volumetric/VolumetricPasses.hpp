// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "PassPayloads.hpp"
#include "TargetManager.hpp"
#include "graph/RenderPassDecl.hpp"
#include "passes/ComputePass.hpp"
#include "features/VolumetricFogSystem.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// The volumetric injection chain, one descriptor each. These dispatch the
// VolumetricFogSystem's fixed-domain double-buffered passes (the dispatch
// size is reflected from the shader), which is why their descriptors carry no
// meaningful extent; the slot writes and zeroed push payloads are the former
// per-pass operator() bodies, verbatim.

struct VolumetricFogInjectDesc {
    static constexpr auto name = Vk::ResourceName("VolumetricFogInject");
    using Usages               = Vk::TypeList<Vk::ComputeWrite<Res_VoxelMedia>>;
    using ShaderDecl           = Shaders::VolumetricFogInject;
    using ShaderModule         = Shaders::Modules::VolumetricFogInjectCS;
    using PushConstants        = VolumetricFogPushConstants;

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>& {
        return frame.fog.FogInject();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const VolumetricFogPushConstants fogPC = {};
        return MakeComputePassArgs(
            fogPC, frame.fog.VoxelDispatchExtent(), Vk::Slot<"outVoxelMedia">(Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(frame.graphResources.voxelMedia)),
            Vk::Slot<"noiseTexture">(frame.fog.NoiseWrite()), Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
        );
    }
};

struct VolumetricLightInjectDesc {
    static constexpr auto name = Vk::ResourceName("VolumetricLightInject");
    using Usages = Vk::TypeList<Vk::ComputeReadGeneral<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>, Vk::ComputeRead<Res_ShadowMap>>;
    using ShaderDecl           = Shaders::VolumetricLightInject;
    using ShaderModule         = Shaders::Modules::VolumetricLightInjectCS;
    using PushConstants        = VolumetricLightInjectPushConstants;

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout>& {
        return frame.fog.LightInject();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const VolumetricLightInjectPushConstants lightInjectPC = {};
        return MakeComputePassArgs(
            lightInjectPC, frame.fog.VoxelDispatchExtent(),
            Vk::Slot<"inVoxelMedia">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(frame.graphResources.voxelMedia)),
            Vk::Slot<"outVoxelLight">(Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(frame.graphResources.voxelLight)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"lights">(frame.frames.lightStorageBuffers[fIdx]),
            Vk::Slot<"clusterGrid">(frame.frames.clusterGridBuffers[fIdx]), Vk::Slot<"clusterIndexList">(frame.frames.lightIndexListBuffers[fIdx]),
            Vk::Slot<"shadowMap">(Vk::Assume<Vk::ComputeRead<Res_ShadowMap>>(frame.graphResources.shadowMap))
        );
    }
};

struct VolumetricIntegrationDesc {
    static constexpr auto name = Vk::ResourceName("VolumetricIntegrate");
    using Usages               = Vk::TypeList<Vk::ComputeReadGeneral<Res_VoxelLight>, Vk::ComputeWrite<Res_VoxelInt>>;
    using ShaderDecl           = Shaders::VolumetricIntegration;
    using ShaderModule         = Shaders::Modules::VolumetricIntegrationCS;
    using PushConstants        = void;

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout>& {
        return frame.fog.Integrate();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        return MakeComputePassArgs(
            frame.fog.VoxelDispatchExtent(),
            Vk::Slot<"inVoxelLight">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(frame.graphResources.voxelLight)),
            Vk::Slot<"outVoxelIntegrated">(Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(frame.graphResources.voxelIntegrated)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
        );
    }
};

struct VolumetricTemporalDesc {
    static constexpr auto name = Vk::ResourceName("VolumetricTemporal");
    using Usages = Vk::TypeList<Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>>;
    using ShaderDecl           = Shaders::VolumetricTemporal;
    using ShaderModule         = Shaders::Modules::VolumetricTemporalCS;
    using PushConstants        = VolumetricTemporalPushConstants;

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>& {
        return frame.fog.Temporal();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const VolumetricTemporalPushConstants temporalPC = {};
        return MakeComputePassArgs(
            temporalPC, frame.fog.VoxelDispatchExtent(),
            Vk::Slot<"inVoxelIntegratedCurrent">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(frame.graphResources.voxelIntegrated)),
            Vk::Slot<"inVoxelIntegratedHistory">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(frame.graphResources.voxelHistory)),
            Vk::Slot<"outVoxelIntegratedResolved">(Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(frame.graphResources.voxelResolved)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
        );
    }
};

} // namespace ZHLN::Passes
