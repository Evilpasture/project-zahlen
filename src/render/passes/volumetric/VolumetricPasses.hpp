// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PassPayloads.hpp"
#include "RenderFrame.hpp"
#include "Renderer.hpp"
#include "TargetManager.hpp"
#include "features/VolumetricFogSystem.hpp"
#include "graph/RenderPassDecl.hpp"
#include "passes/ComputePass.hpp"
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

    [[nodiscard]] static auto GetPass(RenderFrame& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>& {
        return frame.renderer.fog.FogInject();
    }

    [[nodiscard]] static auto Prepare(RenderFrame& frame, const Slang::ScenePassPushConstants&) noexcept {
        const VolumetricFogPushConstants fogPC = {};
        return MakeComputePassArgs(
            fogPC, frame.renderer.fog.VoxelDispatchExtent(),
            Vk::Slot<"outVoxelMedia">(Vk::Assume<Vk::ComputeWrite<Res_VoxelMedia>>(frame.graphResources.voxelMedia)),
            Vk::Slot<"noiseTexture">(frame.renderer.fog.NoiseWrite()), Vk::Slot<"frame">(frame.frameUniformBuffers)
        );
    }
};

struct VolumetricLightInjectDesc {
    static constexpr auto name = Vk::ResourceName("VolumetricLightInject");
    using Usages = Vk::TypeList<Vk::ComputeReadGeneral<Res_VoxelMedia>, Vk::ComputeWrite<Res_VoxelLight>, Vk::ComputeRead<Res_ShadowMap>>;
    using ShaderDecl           = Shaders::VolumetricLightInject;
    using ShaderModule         = Shaders::Modules::VolumetricLightInjectCS;
    using PushConstants        = VolumetricLightInjectPushConstants;

    [[nodiscard]] static auto GetPass(RenderFrame& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout>& {
        return frame.renderer.fog.LightInject();
    }

    [[nodiscard]] static auto Prepare(RenderFrame& frame, const Slang::ScenePassPushConstants&) noexcept {
        const VolumetricLightInjectPushConstants lightInjectPC = {};
        return MakeComputePassArgs(
            lightInjectPC, frame.renderer.fog.VoxelDispatchExtent(),
            Vk::Slot<"inVoxelMedia">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelMedia>>(frame.graphResources.voxelMedia)),
            Vk::Slot<"outVoxelLight">(Vk::Assume<Vk::ComputeWrite<Res_VoxelLight>>(frame.graphResources.voxelLight)),
            Vk::Slot<"frame">(frame.frameUniformBuffers), Vk::Slot<"lights">(frame.lightStorageBuffers), Vk::Slot<"clusterGrid">(frame.clusterGridBuffers),
            Vk::Slot<"clusterIndexList">(frame.lightIndexListBuffers),
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

    [[nodiscard]] static auto GetPass(RenderFrame& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout>& {
        return frame.renderer.fog.Integrate();
    }

    [[nodiscard]] static auto Prepare(RenderFrame& frame, const Slang::ScenePassPushConstants&) noexcept {
        return MakeComputePassArgs(
            frame.renderer.fog.VoxelDispatchExtent(),
            Vk::Slot<"inVoxelLight">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelLight>>(frame.graphResources.voxelLight)),
            Vk::Slot<"outVoxelIntegrated">(Vk::Assume<Vk::ComputeWrite<Res_VoxelInt>>(frame.graphResources.voxelIntegrated)),
            Vk::Slot<"frame">(frame.frameUniformBuffers)
        );
    }
};

struct VolumetricTemporalDesc {
    static constexpr auto name = Vk::ResourceName("VolumetricTemporal");
    using Usages = Vk::TypeList<Vk::ComputeReadGeneral<Res_VoxelInt>, Vk::ComputeReadGeneral<Res_VoxelHist>, Vk::ComputeWrite<Res_VoxelResolved>>;
    using ShaderDecl           = Shaders::VolumetricTemporal;
    using ShaderModule         = Shaders::Modules::VolumetricTemporalCS;
    using PushConstants        = VolumetricTemporalPushConstants;

    [[nodiscard]] static auto GetPass(RenderFrame& frame) noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>& {
        return frame.renderer.fog.Temporal();
    }

    [[nodiscard]] static auto Prepare(RenderFrame& frame, const Slang::ScenePassPushConstants&) noexcept {
        const VolumetricTemporalPushConstants temporalPC = {};
        return MakeComputePassArgs(
            temporalPC, frame.renderer.fog.VoxelDispatchExtent(),
            Vk::Slot<"inVoxelIntegratedCurrent">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelInt>>(frame.graphResources.voxelIntegrated)),
            Vk::Slot<"inVoxelIntegratedHistory">(Vk::Assume<Vk::ComputeReadGeneral<Res_VoxelHist>>(frame.graphResources.voxelHistory)),
            Vk::Slot<"outVoxelIntegratedResolved">(Vk::Assume<Vk::ComputeWrite<Res_VoxelResolved>>(frame.graphResources.voxelResolved)),
            Vk::Slot<"frame">(frame.frameUniformBuffers)
        );
    }
};

} // namespace ZHLN::Passes
