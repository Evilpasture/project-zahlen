// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/init/RenderInitDevice.cpp
#include "../OpenGLHacks/HostBlit.hpp"
#include "../PresentationSurface.hpp"
#include "../RenderInternal.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstdlib>
#include <vector>

namespace {

struct HardwareCaps {
    bool supportsDrawIndirectCount = false;
    bool supportsInt64             = false;
    // VK_EXT_mesh_shader: extension + features + the hardware limits the
    // Zahlen task/mesh shaders were authored against.
    bool supportsMeshShader = false;
    // Requested separately: FeatureChain::Optional drops the WHOLE feature
    // struct when any single requested bit is unsupported, so asking for
    // multiviewMeshShader unconditionally would silently disable taskShader
    // and meshShader too on a device that lacks only the multiview bit.
    bool supportsMultiviewMeshShader = false;
    // meshShaderQueries (also VK_EXT_mesh_shader): without it the task/mesh
    // pipeline-statistic bits are illegal in a query pool
    // (VUID-VkQueryPoolCreateInfo-meshShaderQueries-07069). Probed separately
    // for the same reason as multiview: FeatureChain::Optional drops the
    // whole struct when any requested bit is unsupported, and the GpuProfiler
    // adds those bits only when the feature was actually enabled.
    bool supportsMeshShaderQueries = false;
    // VkPhysicalDeviceFeatures::pipelineStatisticsQuery: feeds GpuProfiler's
    // opt-in pipeline counter capture (clipper and task/mesh shader
    // statistics). Probed because it is a diagnostic feature and must never
    // veto device creation on a device that lacks it.
    bool supportsPipelineStatisticsQuery = false;
    // VkPhysicalDeviceSubgroupProperties: the subgroup width and the op
    // classes this device supports. Zahlen targets plain Vulkan 1.3, where
    // only BASIC subgroup ops are guaranteed in compute; arithmetic/ballot/
    // shuffle become mandatory only under the Roadmap2022 milestone /
    // Vulkan 1.4. Probed (not assumed) because cluster_culling.slang's scan
    // runs WavePrefixSum/WaveActiveSum/WaveReadLaneAt.
    uint32_t               subgroupSize = 0;
    VkSubgroupFeatureFlags subgroupOps  = 0;
    // Presentation pacing (see PresentPacer): FIFO latest-ready plus the
    // VK_EXT_present_timing group. Two spellings exist for fifo-latest-ready
    // (KHR, and the EXT alias sharing its feature struct and enumerant) and
    // for calibrated timestamps (KHR/EXT): the KHR spelling is enabled when
    // advertised, else the EXT one. All false on sessions without a native
    // swapchain -- headless and HostBlit never pace, so they probe nothing.
    bool supportsFifoLatestReadyKHR      = false;
    bool supportsFifoLatestReadyEXT      = false;
    bool supportsPresentTiming           = false;
    bool supportsCalibratedTimestampsKHR = false;
};

class HardwareCapsProber {
  public:
    explicit HardwareCapsProber(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept: _physicalDevice(physicalDevice), _apiVersion(apiVersion) {
    }

    auto ProbeInt64(bool& target) && noexcept -> HardwareCapsProber&& {
        VkPhysicalDeviceFeatures2 features2 {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
        target = (features2.features.shaderInt64 == VK_TRUE);
        return std::move(*this);
    }

    auto ProbeDrawIndirectCount(bool& target) && noexcept -> HardwareCapsProber&& {
        const bool hasExt = ZHLN::Vk::QueryDeviceExtensions(_physicalDevice, "VK_KHR_draw_indirect_count").All();
        if (hasExt || _apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceFeatures2 features2 {};

            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            VkPhysicalDeviceVulkan12Features features12 {};
            features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            features2.pNext  = &features12;
            vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
            target = (features12.drawIndirectCount == VK_TRUE);
        } else {
            target = false;
        }
        return std::move(*this);
    }

    auto ProbePipelineStatisticsQuery(bool& target) && noexcept -> HardwareCapsProber&& {
        VkPhysicalDeviceFeatures2 features2 {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
        target = (features2.features.pipelineStatisticsQuery == VK_TRUE);
        return std::move(*this);
    }

    auto ProbeSubgroups(uint32_t& size, VkSubgroupFeatureFlags& ops) && noexcept -> HardwareCapsProber&& {
        VkPhysicalDeviceSubgroupProperties subgroup {};
        subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2 {};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &subgroup;
        vkGetPhysicalDeviceProperties2(_physicalDevice, &properties2);
        size = subgroup.subgroupSize;
        ops  = subgroup.supportedOperations;
        return std::move(*this);
    }

    // Presentation pacing (see PresentPacer): fifo-latest-ready (either
    // spelling, plus its feature bit) and the present-timing group -- the
    // timing and present-id2 extensions with all three feature bits, plus
    // either calibrated-timestamps spelling from the dependency closure.
    // Sessions without a native swapchain never pace, so they probe nothing
    // and every pacing cap stays false.
    auto ProbePresentPacing(bool canPresent, bool& fifoKhr, bool& fifoExt, bool& timing, bool& calibKhr) && noexcept
        -> HardwareCapsProber&& {
        if (!canPresent) {
            return std::move(*this);
        }
        using ZHLN::Vk::QueryDeviceExtensions;
        fifoKhr = QueryDeviceExtensions(_physicalDevice, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME).All();
        fifoExt = QueryDeviceExtensions(_physicalDevice, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME).All();
        if (fifoKhr || fifoExt) {
            const auto fifoFeatures = ZHLN::Vk::QueryFeatureSupport<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>(_physicalDevice);
            if (fifoFeatures.presentModeFifoLatestReady != VK_TRUE) {
                fifoKhr = false;
                fifoExt = false;
                ZHLN::Log("[RenderInit] FIFO latest-ready extension present but presentModeFifoLatestReady is not advertised; paced policies fall back.");
            }
        } else {
            ZHLN::Log("[RenderInit] FIFO latest-ready present mode not advertised; paced policies fall back to legacy V-blank.");
        }

        timing         = false;
        const bool groupExts = QueryDeviceExtensions(_physicalDevice, VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME).All();
        calibKhr             = QueryDeviceExtensions(_physicalDevice, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME).All();
        const bool calibExt  = QueryDeviceExtensions(_physicalDevice, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME).All();
        if (groupExts && (calibKhr || calibExt)) {
            const auto timingFeatures = ZHLN::Vk::QueryFeatureSupport<VkPhysicalDevicePresentTimingFeaturesEXT>(_physicalDevice);
            const auto id2Features     = ZHLN::Vk::QueryFeatureSupport<VkPhysicalDevicePresentId2FeaturesKHR>(_physicalDevice);
            timing = timingFeatures.presentTiming == VK_TRUE && timingFeatures.presentAtAbsoluteTime == VK_TRUE &&
                     id2Features.presentId2 == VK_TRUE;
            if (!timing) {
                ZHLN::Log(
                    "[RenderInit] Present-timing extensions present but timing/absolute/id2 features are not fully advertised; closed loop disabled."
                );
            }
        } else if (fifoKhr || fifoExt) {
            // Only news when latest-ready exists: without it the policy is
            // legacy V-blank either way, and the line above already said so.
            ZHLN::Log("[RenderInit] VK_EXT_present_timing extension group not fully advertised; latest-ready presents run untimed.");
        }
        return std::move(*this);
    }

  private:
    VkPhysicalDevice _physicalDevice;
    uint32_t         _apiVersion;
};

auto CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;
auto CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;
auto CheckMeshShaderQueriesSupport(VkPhysicalDevice physicalDevice) noexcept -> bool;

auto ProbeHardware(VkPhysicalDevice physicalDevice, uint32_t apiVersion, bool canPresent) noexcept -> HardwareCaps {
    HardwareCaps caps {};
    HardwareCapsProber(physicalDevice, apiVersion)
        .ProbeInt64(caps.supportsInt64)
        .ProbeDrawIndirectCount(caps.supportsDrawIndirectCount)
        .ProbePipelineStatisticsQuery(caps.supportsPipelineStatisticsQuery)
        .ProbeSubgroups(caps.subgroupSize, caps.subgroupOps)
        .ProbePresentPacing(
            canPresent, caps.supportsFifoLatestReadyKHR, caps.supportsFifoLatestReadyEXT, caps.supportsPresentTiming,
            caps.supportsCalibratedTimestampsKHR
        );
    caps.supportsMeshShader          = CheckMeshShaderSupport(physicalDevice);
    caps.supportsMultiviewMeshShader = caps.supportsMeshShader && CheckMultiviewMeshShaderSupport(physicalDevice);
    caps.supportsMeshShaderQueries   = caps.supportsMeshShader && CheckMeshShaderQueriesSupport(physicalDevice);

    // cluster_culling.slang's two-level scan executes subgroup arithmetic
    // and shuffles on every dispatch. Log the width once per device so
    // capture/profile readings land next to the scan path they describe,
    // and warn when the op classes the shader needs are missing: plain
    // Vulkan 1.3 only guarantees BASIC, the full set is Roadmap2022 /
    // Vulkan 1.4.
    constexpr VkSubgroupFeatureFlags kUsedSubgroupOps =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    if ((caps.subgroupOps & kUsedSubgroupOps) != kUsedSubgroupOps) {
        ZHLN::Log(
            "[RenderInit] WARNING: device reports subgroup width {} but supportedOperations={:#x} lacks BASIC/ARITHMETIC/SHUFFLE; "
            "cluster_culling.slang's subgroup scan needs those op classes.",
            caps.subgroupSize, caps.subgroupOps
        );
    } else {
        ZHLN::Log("[RenderInit] Subgroup width {} (supportedOperations={:#x}); cluster scan runs its subgroup path.", caps.subgroupSize, caps.subgroupOps);
    }

    return caps;
}

// VK_EXT_mesh_shader is only usable when the extension is present, the two
// feature bits are advertised AND the device's mesh-shader limits cover the
// geometry budget baked into basic_task.slang / basic_mesh.slang. Anything
// less and the engine silently keeps the vertex pipeline.
auto CheckMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    const auto meshExt = ZHLN::Vk::QueryDeviceExtensions(physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (!meshExt.All()) {
        // Log the count too: a suspiciously round number here (128, 256...)
        // means something is truncating the enumeration again.
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader not present among the {} device extensions reported; using the vertex pipeline.", meshExt.reportedCount
        );
        return false;
    }

    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    if (meshFeatures.taskShader != VK_TRUE || meshFeatures.meshShader != VK_TRUE) {
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader present but its features are not advertised (taskShader={}, meshShader={}); using the vertex pipeline.",
            meshFeatures.taskShader, meshFeatures.meshShader
        );
        return false;
    }

    const ZHLN_MeshShaderLimits limits = ZHLN_QueryMeshShaderLimits(physicalDevice);
    if (!ZHLN_MeshShaderLimitsSufficient(&limits)) {
        ZHLN::Log(
            "[RenderInit] VK_EXT_mesh_shader present but limits are insufficient "
            "(maxMeshOutputVertices={}, maxMeshOutputPrimitives={}, maxTaskWorkGroupInvocations={}); using the vertex pipeline.",
            limits.max_mesh_output_vertices, limits.max_mesh_output_primitives, limits.max_task_work_group_invocations
        );
        return false;
    }

    // Deliberately silent on success: a working feature is not news. Every
    // return false above explains itself.
    return true;
}

auto CheckMultiviewMeshShaderSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return meshFeatures.multiviewMeshShader == VK_TRUE;
}

auto CheckMeshShaderQueriesSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures {};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &meshFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return meshFeatures.meshShaderQueries == VK_TRUE;
}

} // namespace

namespace ZHLN {

auto CheckRayTracingSupport(VkPhysicalDevice physicalDevice) noexcept -> bool {
    return ZHLN::Vk::QueryDeviceExtensions(
               physicalDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
               VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    )
        .All();
}

namespace {

auto GetPlatformInstanceExtensions(const PresentationTarget& target) noexcept -> std::expected<Vk::ExtensionResult, ErrorCode> {
    auto builder = Vk::ExtensionBuilder::ForInstance();

    if constexpr (isMac) {
        // macOS has no native Vulkan WSI: requiring surface extensions here
        // would fail instance creation outright. Windowed sessions present
        // through the HostBlit plugin's own OpenGL window instead, so no
        // WSI extensions are requested at all.
    } else {
        // The whole platform decision is the handle's: the window subsystem
        // published what the OS gave it, and this names the WSI extension that
        // matches it. A session with no descriptor -- a headless one, or a
        // window on a platform this build has no native backend for -- asks for
        // nothing, so there is no special case here and no window-system
        // function is reachable from this file.
        AppendPlatformSurfaceExtensions(builder, target.GetNativeSurface());
    }

    return std::move(builder)
        .Debug(true) // Render-graph checkpoints use VK_EXT_debug_utils when available.
        .OptionalIf("VK_KHR_portability_enumeration", isMac)
        // The present-timing device group depends on the instance-side
        // get_surface_capabilities2. Windowed sessions already require it
        // (see requireWsi); this covers the display/TTY path, which builds
        // its surface without one -- the builder does not dedupe, so the
        // windowed case must NOT match here.
        .OptionalIf(
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, !target.IsHeadless() && !target.GetNativeSurface().Valid()
        )
        .Build()
        .transform_error([](auto err) -> ErrorCode { return err; });
}

// The renderer's half of the device's feature chain: only capabilities a pass,
// a pipeline or the frame scheduler branches on.
//
// The backend's half -- robustness, crash dumps, shader abort, swapchain
// maintenance, the stencil-less-secondary feature -- is negotiated inside
// Vk::Context::Builder::Build and chained behind this one. Vulkan takes a
// single feature chain and a struct whose sType appears twice in it is
// invalid, so the two halves must stay disjoint: if a feature is wanted on
// both sides, it belongs on one.
auto BuildFeatureChain(VkPhysicalDevice physicalDevice, const HardwareCaps& caps, ValidationMode validationMode) noexcept {
    return Vk::FeatureChainBuilder(physicalDevice)
        // VK_KHR_swapchain_maintenance1 stays here rather than moving into the
        // backend with the other quiet features: its extension is enabled only
        // when there is a swapchain at all, and a feature struct chained
        // without its extension is a VUID. The pair has to live on whichever
        // side controls the extension.
        .Optional<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([](auto& f) -> auto { f.swapchainMaintenance1 = VK_TRUE; })
        // Presentation pacing (see PresentPacer): FIFO latest-ready plus the
        // VK_EXT_present_timing group. All optional and caps-gated; when a cap
        // is missing the struct chains with FALSE bits (accepted by device
        // creation), and the pacer resolves its policy from what actually got
        // enabled. The relative-timing bit stays off: the pacer only schedules
        // absolute targets.
        .Optional<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>([&caps](auto& f) -> auto {
            f.presentModeFifoLatestReady = (caps.supportsFifoLatestReadyKHR || caps.supportsFifoLatestReadyEXT) ? VK_TRUE : VK_FALSE;
        })
        .Optional<VkPhysicalDevicePresentTimingFeaturesEXT>([&caps](auto& f) -> auto {
            f.presentTiming         = caps.supportsPresentTiming ? VK_TRUE : VK_FALSE;
            f.presentAtAbsoluteTime = caps.supportsPresentTiming ? VK_TRUE : VK_FALSE;
            f.presentAtRelativeTime = VK_FALSE;
        })
        .Optional<VkPhysicalDevicePresentId2FeaturesKHR>([&caps](auto& f) -> auto { f.presentId2 = caps.supportsPresentTiming ? VK_TRUE : VK_FALSE; })
        .Require<VkPhysicalDeviceVulkan11Features>([](auto& f) -> auto {
            f.multiview                          = VK_TRUE;
            f.storageBuffer16BitAccess           = VK_TRUE;
            f.uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f.shaderDrawParameters               = VK_TRUE;
        })
        .Require<VkPhysicalDeviceVulkan13Features>([](auto& f) -> auto {
            f.synchronization2               = VK_TRUE;
            f.dynamicRendering               = VK_TRUE;
            f.shaderDemoteToHelperInvocation = VK_TRUE;
        })
        .Require<VkPhysicalDeviceVulkan12Features>([&](auto& f) -> auto {
            f.descriptorIndexing                           = VK_TRUE;
            f.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
            f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            f.descriptorBindingPartiallyBound              = VK_TRUE;
            f.runtimeDescriptorArray                       = VK_TRUE;
            f.bufferDeviceAddress                          = VK_TRUE;
            f.hostQueryReset                               = VK_TRUE;
            f.timelineSemaphore                            = VK_TRUE;
            f.drawIndirectCount                            = caps.supportsDrawIndirectCount ? VK_TRUE : VK_FALSE;
            f.uniformAndStorageBuffer8BitAccess            = VK_TRUE;
            f.shaderFloat16                                = VK_TRUE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.scalarBlockLayout            = VK_TRUE;
                f.storageBuffer8BitAccess      = VK_TRUE;
                f.shaderInt8                   = VK_TRUE;
                f.vulkanMemoryModel            = VK_TRUE;
                f.vulkanMemoryModelDeviceScope = VK_TRUE;
            }
        })
        .Optional<VkPhysicalDeviceAccelerationStructureFeaturesKHR>([](auto& f) -> auto { f.accelerationStructure = VK_TRUE; })
        .Optional<VkPhysicalDeviceRayQueryFeaturesKHR>([](auto& f) -> auto { f.rayQuery = VK_TRUE; })
        // VK_EXT_descriptor_heap: the whole scene binding model now lives in
        // descriptor heaps; the legacy set path remains only for passes that
        // have not been ported yet (post-processing, volumetric, ...).
        .Require<VkPhysicalDeviceDescriptorHeapFeaturesEXT>([](auto& f) -> auto { f.descriptorHeap = VK_TRUE; })
        // VK_EXT_mesh_shader. multiviewMeshShader lets the shadow pass render
        // all cascades from a single dispatch; it is only requested when the
        // device actually supports mesh shading, because the feature struct
        // must not be chained on a device that lacks the extension.
        .Optional<VkPhysicalDeviceMeshShaderFeaturesEXT>([&caps](auto& f) -> auto {
            f.taskShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            f.meshShader = caps.supportsMeshShader ? VK_TRUE : VK_FALSE;
            // Only requested when the device actually has it: one unsupported
            // bit would make FeatureChain::Optional discard the entire struct,
            // leaving the extension enabled but task/mesh shading OFF.
            f.multiviewMeshShader = caps.supportsMultiviewMeshShader ? VK_TRUE : VK_FALSE;
            // Gated by VUID-VkQueryPoolCreateInfo-meshShaderQueries-07069:
            // the task/mesh pipeline-statistic bits need this feature. Only
            // asked for when present, same discard hazard as multiview above.
            f.meshShaderQueries = caps.supportsMeshShaderQueries ? VK_TRUE : VK_FALSE;
        })
        .Require<VkPhysicalDeviceFeatures2>([&](auto& f) -> auto {
            f.features.multiDrawIndirect         = VK_TRUE;
            f.features.samplerAnisotropy         = VK_TRUE;
            f.features.drawIndirectFirstInstance = VK_TRUE;
            f.features.shaderInt64               = caps.supportsInt64 ? VK_TRUE : VK_FALSE;
            f.features.imageCubeArray            = VK_TRUE;
            f.features.shaderInt16               = VK_TRUE;
            // GpuProfiler's opt-in pipeline counters (VK_QUERY_TYPE_PIPELINE_
            // STATISTICS): only requested when the device advertises the bit,
            // matching GpuProfiler::Init's support probe -- a diagnostic
            // feature must never veto device creation.
            f.features.pipelineStatisticsQuery = caps.supportsPipelineStatisticsQuery ? VK_TRUE : VK_FALSE;

            if (validationMode == ZHLN::ValidationMode::GPU) {
                f.features.robustBufferAccess             = VK_TRUE;
                f.features.fragmentStoresAndAtomics       = VK_TRUE;
                f.features.vertexPipelineStoresAndAtomics = VK_TRUE;
                f.features.shaderInt16                    = VK_TRUE;
            }
        })
        .Build();
}

auto GetDeviceExtensions(VkPhysicalDevice physicalDevice, bool noSwapchain, const HardwareCaps& caps) noexcept
    -> std::expected<Vk::ExtensionResult, ErrorCode> {
    auto builder = Vk::ExtensionBuilder::ForDevice(physicalDevice);

    if (!noSwapchain) {
        builder.Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            .Optional(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)
            .Optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
        // Presentation pacing (see PresentPacer): latest-ready prefers the KHR
        // spelling, else the EXT alias (same feature struct, same enumerant --
        // the pacer cannot tell them apart). The timing group enables
        // all-or-nothing with its dependency closure, and the
        // calibrated-timestamps spelling follows whichever the device
        // advertises (the instance side -- surface plus
        // get_surface_capabilities2 -- is already required for windowed
        // sessions, and get_physical_device_properties2 is core in 1.3).
        if (caps.supportsFifoLatestReadyKHR) {
            builder.Optional(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
        } else if (caps.supportsFifoLatestReadyEXT) {
            builder.Optional(VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
        }
        const char* calibName = caps.supportsCalibratedTimestampsKHR ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                                                                     : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        builder.OptionalGroup(
            {VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME, calibName}, caps.supportsPresentTiming
        );
    }

    return builder.OptionalIf("VK_KHR_portability_subset", isMac)
        .OptionalGroup(
            {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
            CheckRayTracingSupport(physicalDevice)
        )
        // VK_EXT_descriptor_heap replaces descriptor sets/pools/layouts for the
        // scene path. VK_KHR_maintenance5 (or Vulkan 1.4) provides
        // VkPipelineCreateFlags2CreateInfoKHR for the mandatory
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT pipeline flag.
        .Require(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)
        .Require(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)
        // VK_EXT_mesh_shader replaces the input assembler + vertex stage of the
        // geometry passes with task/mesh shaders. It stays OPTIONAL: the vertex
        // pipeline is still built for every material, so devices without mesh
        // shading (or with limits below our meshlet budget) keep rendering.
        // Support was already probed once into HardwareCaps; re-probing here
        // would repeat the diagnostics for every failure.
        .OptionalGroup({VK_EXT_MESH_SHADER_EXTENSION_NAME}, caps.supportsMeshShader)
        .Build()
        .transform_error([](auto err) -> ErrorCode { return err; });
}

// Chooses how frames reach the display (see PresentationMode). Fixed for
// the lifetime of the context; `headless` keeps its strict meaning —
// OffscreenOnly is only for sessions that genuinely have no window.
auto SelectPresentationMode(const PresentationTarget& target) noexcept -> PresentationMode {
    if (target.IsHeadless()) {
        return PresentationMode::OffscreenOnly;
    }
    if constexpr (isMac) {
        // No native Vulkan WSI on macOS: render to the offscreen target and
        // let the HostBlit plugin blit it through its own OpenGL window.
        return PresentationMode::HostBlit;
    } else {
        return PresentationMode::NativeSwapchain;
    }
}

} // namespace

RenderContext::RenderContext(PrivateToken /*unused*/, std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Create(
    PresentationTarget& target, const RenderConfig& cfg, FS::FileSystemWatcher* fileSystemWatcher
) noexcept -> std::expected<std::unique_ptr<RenderContext>, ErrorCode> {
    auto impl     = std::make_unique<Impl>(target, fileSystemWatcher);
    impl->appName = cfg.appName;
    // Where the driver pipeline cache is read from and written back to, decided
    // by the engine and handed over in the config: a dev tree keeps the
    // historical `build/cache/...`, anything else gets the per-user cache
    // directory, because a distributed binary cannot write into a build tree
    // that is not there. An empty path would mean "no persistence".
    impl->pipelineCachePath = cfg.pipelineCachePath;
    impl->enableMeshShading = cfg.enableMeshShading && (std::getenv("ZHLN_NO_MESH_SHADING") == nullptr);

    const PresentationMode mode = SelectPresentationMode(target);
    impl->presentationMode      = mode;

    Vk::Instance            instanceObject;
    VkInstance              instance    = VK_NULL_HANDLE;
    VkSurfaceKHR            raw_surface = VK_NULL_HANDLE;
    int                     width       = 0;
    int                     height      = 0;
    ZHLN_PhysicalDeviceInfo physicalInfo {};

    return GetPlatformInstanceExtensions(target)
        .and_then([&](auto&& inst_exts) -> std::expected<void, ErrorCode> {
            return Vk::Context::Builder()
                .AppName(impl->appName)
                .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                .InstanceExtensions(inst_exts)
                .BuildInstance()
                .transform([&](Vk::Instance inst) -> void {
                    instanceObject = std::move(inst);
                    instance       = instanceObject.Handle();
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (mode == PresentationMode::OffscreenOnly || mode == PresentationMode::HostBlit) {
                // No WSI surface to create: the frame lives in the offscreen
                // target and, in HostBlit mode, the plugin presents it after
                // each submit. Size that target from the presentation target's
                // FRAMEBUFFER (points != pixels on Retina displays), falling
                // back to a sane default for a target that has no drawable area
                // yet.
                width  = 1280;
                height = 720;
                if (const Extent2D fb = target.GetFramebufferExtent(); fb.width > 0 && fb.height > 0) {
                    width  = static_cast<int>(fb.width);
                    height = static_cast<int>(fb.height);
                }
                raw_surface = VK_NULL_HANDLE;
                return {};
            }
            if (!target.IsTTY()) {
                // The windowed path: the RHI reads the platform descriptor out of
                // the handle and builds the surface with the matching
                // vkCreate*SurfaceKHR. This layer hands over the handle and gets
                // a VkSurfaceKHR back, and never learns which platform it was.
                auto surfaceRes = CreateSurfaceFromNative(instance, target.GetNativeSurface());
                if (!surfaceRes) {
                    return std::unexpected(surfaceRes.error());
                }
                raw_surface = surfaceRes->Release();
                if (const Extent2D fb = target.GetFramebufferExtent(); fb.width > 0 && fb.height > 0) {
                    width  = static_cast<int>(fb.width);
                    height = static_cast<int>(fb.height);
                }
                return {};
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return Vk::Context::Builder()
                .Instance(instance)
                .Surface(raw_surface)
                .SelectPhysicalDevice()
                .transform([&](const ZHLN_PhysicalDeviceInfo& info) -> void { physicalInfo = info; });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (target.IsTTY() && mode == PresentationMode::NativeSwapchain) {
                // Direct to display: VK_KHR_display builds this from the
                // physical device, which is why it waits for device selection
                // and why the mode's visible region is what sizes the frame.
                uint32_t modeWidth  = 0;
                uint32_t modeHeight = 0;
                auto     surfaceRes = Vk::CreateDisplaySurface(instance, physicalInfo.handle, modeWidth, modeHeight);
                if (!surfaceRes) {
                    return std::unexpected(surfaceRes.error());
                }
                width       = static_cast<int>(modeWidth);
                height      = static_cast<int>(modeHeight);
                raw_surface = surfaceRes->Release();
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            impl->presenter.surface = Vk::Surface(instance, raw_surface);
            HardwareCaps caps     = ProbeHardware(
                physicalInfo.handle, physicalInfo.properties.properties.apiVersion, mode == PresentationMode::NativeSwapchain
            );
            // The probed caps decide what the chain below REQUESTS; they are not
            // how anything later reads back what got enabled. The chain goes to
            // the Builder whole, Context snapshots the structs it enabled, and
            // the passes ask ctx.HasFeature<T>(...) -- so no probe result is
            // parked on Impl and no per-feature flag has to be threaded through.
            //
            // The probes themselves stay load-bearing, and it is worth being
            // precise about why. FeatureChain::Optional is all-or-nothing: it
            // drops the WHOLE struct when the device lacks any single requested
            // bit. So the only way to enable one bit of a struct without
            // forfeiting its neighbours is to ask for exactly the bits the
            // device has -- which is what a per-bit probe is for. Blindly
            // requesting every bit and masking off what is missing would leave,
            // say, taskShader enabled without meshShader, and
            // ZHLN_Device::mesh_shader_enabled is computed from the Volk entry
            // points and the limits alone (RenderCore.c) -- it never reads those
            // feature bits -- so the renderer would take the mesh path and
            // dispatch vkCmdDrawMeshTasks on a device that did not enable it.
            auto features = BuildFeatureChain(physicalInfo.handle, caps, cfg.validationMode);

            return GetDeviceExtensions(physicalInfo.handle, mode != PresentationMode::NativeSwapchain, caps)
                .and_then([&](auto&& dev_exts) -> std::expected<void, ErrorCode> {
                    const std::vector<const char*>& devExtList = dev_exts;

                    return Vk::Context::Builder()
                        .Instance(std::move(instanceObject))
                        .Surface(raw_surface)
                        .PhysicalDevice(physicalInfo)
                        .DeviceExtensions(devExtList)
                        .DeviceFeatures(features)
                        .ValidationMode(static_cast<Vk::ValidationMode>(cfg.validationMode))
                        .Build()
                        .transform([&](auto&& context) -> auto {
                            impl->ctx         = std::forward<decltype(context)>(context);
                            const auto vendor = static_cast<Vk::GPUVendor>(physicalInfo.properties.properties.vendorID);
                            impl->gpuDiagnostics.Create(
                                vendor, impl->ctx.Device(), impl->ctx.Physical(), Vk::DiagnosticConfig{.crashDumpPath = cfg.crashDumpPath}
                            );
                        });
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if constexpr (isMac) {
                if (mode == PresentationMode::HostBlit) {
                    // Hand the plugin device access so it can allocate its
                    // own command pool + fence (it touches no engine state).
                    // On failure the session continues rendering offscreen
                    // instead of dying — screenshots and tests still work.
                    const bool ok = HostBlit::Init(
                        impl->ctx.Physical(), impl->ctx.Device(), impl->ctx.GraphicsQueue(), physicalInfo.graphics_family
                    );
                    if (!ok) {
                        ZHLN::Log("WARNING: HostBlit presenter failed to initialize; continuing offscreen-only.");
                        impl->presentationMode = PresentationMode::OffscreenOnly;
                    }
                }
            }
            return {};
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return impl->InitSubsystems(cfg, width, height); })
        .transform([&]() -> std::unique_ptr<ZHLN::RenderContext> {
            impl->BeginShaderObservation();
            return std::make_unique<RenderContext>(PrivateToken {}, std::move(impl));
        });
}

RenderContext::~RenderContext() {
    if (_impl && (_impl->ctx.Device() != nullptr)) {
        // Wait for idle once, then drop every destination (each owns its
        // window's swapchain and the render targets vended for it).
        if (auto idle = Vk::WaitIdle(_impl->ctx.Device()); !idle) {
            ZHLN::Log("ERROR: Failed to wait for idle while destroying destinations ({})", idle.error());
        }
        _impl->DestroyDestinations();
        if constexpr (isMac) {
            if (_impl->presentationMode == PresentationMode::HostBlit) {
                // Releases the plugin's GL window and its Vulkan staging
                // resources; safe to call before the device is destroyed.
                HostBlit::Shutdown();
            }
        }
        _impl->gpuDiagnostics.Shutdown();
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::Log("ERROR: Failed to wait for idle on device destruction.");
        }
        // Flush the driver pipeline cache now that the device is idle: every
        // pipeline built this run has been recorded into it. Doing this before
        // the Impl members unwind keeps the device alive for the write, and the
        // cache itself is destroyed afterwards because it is declared after ctx.
        Vk::SavePipelineCache(_impl->ctx.Device(), _impl->pipelineCache.Get(), _impl->pipelineCachePath);
        _impl->stagingContext.reset();


    }
}

} // namespace ZHLN
