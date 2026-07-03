// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "Commands.hpp"
#include "ParallelDraw.hpp"
#include "RenderInternal.hpp"
#include "RenderParams.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <array>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

namespace {

// ============================================================================
// Private Core Refactoring Helpers
// ============================================================================

enum class RenderPassType : uint8_t { Main, Shadow };

// flags & 0xFF == 2 marks an instance as forward-only (transparent / non-deferred).
[[nodiscard]] constexpr bool IsForwardOnly(uint32_t instanceFlags) noexcept {
	return (instanceFlags & 0xFF) == 2;
}

[[nodiscard]] inline const NativeMaterial* ToNative(const void* material) noexcept {
	return std::bit_cast<const NativeMaterial*>(material);
}

[[nodiscard]] inline bool IsVisibleIn(DrawFlags flags, RenderPassType passType) noexcept {
	const bool hasMain = (flags & DrawFlags::VisibleInMain) != DrawFlags::None;
	const bool hasShadow = (flags & DrawFlags::VisibleInShadow) != DrawFlags::None;

	if (!hasMain && !hasShadow) {
		return true;
	}

	return (passType == RenderPassType::Main) ? hasMain : hasShadow;
}

template <typename T>
inline void SubmitDrawInstanced(VkCommandBuffer cmd, const DrawCommand& drawCmd,
								uint32_t instanceIdx, VkDescriptorSet bindlessSet,
								const T& pushConstants,
								VkPipeline pipelineOverride = VK_NULL_HANDLE,
								VkPipelineLayout layoutOverride = VK_NULL_HANDLE,
								VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
															VK_SHADER_STAGE_FRAGMENT_BIT) noexcept {
	const auto* nativeMat = ToNative(drawCmd.material);
	const VkPipeline pipeline =
		(pipelineOverride != VK_NULL_HANDLE) ? pipelineOverride : nativeMat->pipeline.Get();
	const VkPipelineLayout layout =
		(layoutOverride != VK_NULL_HANDLE) ? layoutOverride : nativeMat->layout.Get();

	// Resolved from pre-assembled GPU structure
	const uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0
									 ? drawCmd.instanceData.indexCount
									 : drawCmd.instanceData.vertexCount;

	Vk::DrawInstanced(cmd,
					  {.pipeline = pipeline,
					   .layout = layout,
					   .set = bindlessSet,
					   .vertexCount = vertexCount,
					   .instanceCount = 1,
					   .firstVertex = 0,
					   .firstInstance = instanceIdx},
					  pushConstants, stages);
}

} // namespace

// ============================================================================
// Culling Policy Implementations
// ============================================================================

struct GpuCullingPolicy {
	static void
	Record(const FrameRecorder& recorder, const ZHLN::Array<GroupRange>& groups, uint32_t drawCount,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> color_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> vel_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> norm_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att) noexcept {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		struct FrustumPlanes {
			std::array<JPH::Vec4, 6> planes;
			uint32_t drawCount;
		} planes{};

		const auto& vp = ctx.unjittered_view_proj;
		JPH::Vec4 r0(vp(0, 0), vp(0, 1), vp(0, 2), vp(0, 3));
		JPH::Vec4 r1(vp(1, 0), vp(1, 1), vp(1, 2), vp(1, 3));
		JPH::Vec4 r2(vp(2, 0), vp(2, 1), vp(2, 2), vp(2, 3));
		JPH::Vec4 r3(vp(3, 0), vp(3, 1), vp(3, 2), vp(3, 3));

		auto NormalizePlane = [&](const JPH::Vec4& plane) {
			float len = JPH::Vec3(plane.GetX(), plane.GetY(), plane.GetZ()).Length();
			return plane / std::max(len, 1e-6f);
		};

		planes.planes[0] = NormalizePlane(r3 + r0);
		planes.planes[1] = NormalizePlane(r3 - r0);
		planes.planes[2] = NormalizePlane(r3 + r1);
		planes.planes[3] = NormalizePlane(r3 - r1);
		planes.planes[4] = NormalizePlane(r2);
		planes.planes[5] = NormalizePlane(r3 - r2);
		planes.drawCount = drawCount;

		ctx.cullingPass.Dispatch(cmd, ctx.cullingSets[recorder.frameIndex], (drawCount + 63) / 64,
								 1, 1, planes);

		Vk::BarrierBuilder()
			.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
			.To(cmd, Vk::BarrierStage::Indirect, Vk::BarrierAccess::IndirectRead);

		Vk::DynamicPass(color_att.extent)
			.AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorScene)
			.AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorVelocity)
			.AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorNormalRoughness)
			.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearDepthValue)
			.Execute(cmd, [&]() {
				const VkDeviceSize stride = sizeof(VkDrawIndirectCommand);
				for (const auto& group : groups) {
					if (!group.material->pipeline.Valid()) {
						continue;
					}

					Vk::DrawIndirect(
						cmd,
						{.pipeline = group.material->pipeline.Get(),
						 .layout = group.material->layout.Get(),
						 .set = recorder.bindlessSet,
						 .argumentBuffer =
							 ctx.indirectCommandsBuffers[recorder.frameIndex].Handle(),
						 .offset = group.start * stride,
						 .drawCount = group.count,
						 .stride = static_cast<uint32_t>(stride)},
						ObjectConstants{.instanceId = kGpuCullingSentinel, .isShadowPass = 0});
				}
			});
	}
};

struct CpuCullingPolicy {
	static void
	Record(const FrameRecorder& recorder, const ZHLN::Array<GroupRange>& /*groups*/,
		   uint32_t drawCount, Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> color_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> vel_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> norm_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att) noexcept {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		const auto& colorFormats = ActiveGBuffer::array;

		Vk::DynamicPass(color_att.extent)
			.AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorScene)
			.AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorVelocity)
			.AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorNormalRoughness)
			.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearDepthValue)
			.Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
			.Execute(cmd, [&]() {
				Vk::ParallelDrawDispatch(
					cmd,
					Vk::SecondaryInheritance{.colorFormats = colorFormats,
											 .depthFormat = VK_FORMAT_D32_SFLOAT},
					color_att.extent, drawCount, kParallelChunkSize, recorder.frameIndex,
					std::span<WorkerCmdContext>(ctx.workerCmds.data(), ctx.workerCmds.size()),
					[&](VkCommandBuffer sec_cmd, uint32_t i) {
						const auto& drawCmd = ctx.drawQueue[i];
						if (!IsVisibleIn(drawCmd.flags, RenderPassType::Main)) {
							return;
						}

						if (!ToNative(drawCmd.material)->pipeline.Valid() ||
							IsForwardOnly(drawCmd.instanceData.flags)) {
							return;
						}

						const ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};
						SubmitDrawInstanced(sec_cmd, drawCmd, i, recorder.bindlessSet,
											pushConstants);
					});
			});
	}
};

template <typename CullingPolicy, typename... Args>
void ExecutePass(const FrameRecorder& recorder, const ZHLN::Array<GroupRange>& groups,
				 uint32_t drawCount, Args&&... args) {
	CullingPolicy::Record(recorder, groups, drawCount, std::forward<Args>(args)...);
}

// ============================================================================
// RenderPass Implementation
// ============================================================================

namespace Passes {

void ShadowPass::Execute(const FrameRecorder& recorder) const noexcept {
	VkCommandBuffer cmd = recorder.cmd;
	auto& ctx = recorder.ctx;

	// 1. Initialize frustums for all 4 cascades on the stack using existing SIMD structures
	std::array<Frustum, RenderContext::Impl::NUM_CASCADES> cascadeFrustums{};
	for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
		cascadeFrustums[c].Update(ctx.currentUniforms.lightSpaceMatrices[c]);
	}

	// 2. Map our double-buffered shadow indirect command buffer (CPU_TO_GPU memory)
	auto mapped = ctx.shadowIndirectBuffers[recorder.frameIndex].Map();
	auto* indirectCmdsBase = static_cast<VkDrawIndirectCommand*>(mapped.data);

	// Track write-pointers (offsets) for all 8 potential shadow passes
	// [0..3] are Cascades, [4..7] are Punctual Lights
	std::array<uint32_t, 8> passWriteOffsets{};

	for (uint32_t c = 0; c < 4; ++c) {
		passWriteOffsets[c] = c * kGpuCullingMaxInstances;
	}
	for (uint32_t l = 0; l < 4; ++l) {
		passWriteOffsets[4 + l] = (4 + l) * kGpuCullingMaxInstances;
	}

	// Track actual draw counts for each pass
	std::array<uint32_t, 8> passDrawCounts = {0, 0, 0, 0, 0, 0, 0, 0};

	// 3. SINGLE CACHE-COHERENT MEMORY SWEEP (O(N))
	for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
		const auto& drawCmd = ctx.drawQueue[i];

		// Fast skip if the mesh is not a shadow caster or is forward-only
		if (!IsVisibleIn(drawCmd.flags, RenderPassType::Shadow) ||
			IsForwardOnly(drawCmd.instanceData.flags)) {
			continue;
		}

		uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0
								   ? drawCmd.instanceData.indexCount
								   : drawCmd.instanceData.vertexCount;

		JPH::Vec3 meshPos = drawCmd.instanceData.world.GetTranslation();
		float radius = drawCmd.instanceData.cullRadius;

		// --- Check Cascades (0 to 3) ---
		for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
			if (cascadeFrustums[c].IsSphereVisible(meshPos, radius)) {
				uint32_t writeIdx = passWriteOffsets[c] + passDrawCounts[c];
				indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount,
											  .instanceCount = 1,
											  .firstVertex = 0,
											  .firstInstance = i};
				passDrawCounts[c]++;
			}
		}

		// --- Check Punctual Lights (4 to 7) ---
		for (const auto& light : ctx.mappedLights) {
			if (light.shadowLayer < 0 || light.type != LightType::Point) {
				continue;
			}

			// Map the shadowLayer directly to our [4..7] slots
			uint32_t slotIdx = 4 + light.shadowLayer;
			ZHLN::Assert(passDrawCounts[slotIdx] < kGpuCullingMaxInstances);
			if (slotIdx >= 8) {
				continue;
			}

			JPH::Vec3 lightPos(light.position[0], light.position[1], light.position[2]);

			// Use squared length to avoid the expensive square root calculation
			float distToLightSq = (meshPos - lightPos).LengthSq();
			float maxRange = light.range + radius;
			float maxRangeSq = maxRange * maxRange;

			if (distToLightSq <= maxRangeSq) {
				uint32_t writeIdx = passWriteOffsets[slotIdx] + passDrawCounts[slotIdx];
				indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount,
											  .instanceCount = 1,
											  .firstVertex = 0,
											  .firstInstance = i};
				passDrawCounts[slotIdx]++;
			}
		}
	}

	// 4. DISPATCH DIRECTIONAL SHADOW CASCADES (MDI)
	{
		Profiler::ScopedGpuProfile<Stages::ShadowPass, FrameProfiler> timer(
			cmd, recorder.frameIndex, ctx.gpuProfiler);

		auto ExecuteCascadePass = [&](uint32_t cascade, auto&& recordFn) {
			Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> cascadeLayerImage = {
				.handle = ctx.shadowMap.image.Handle(),
				.view = ctx.shadowCascadeViews[cascade].Get(),
				.extent = ctx.shadowMap.extent,
				.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};

			auto [cascade_att, scope] =
				Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
					cmd, cascadeLayerImage, VK_IMAGE_ASPECT_DEPTH_BIT);

			Vk::DynamicPass(cascadeLayerImage.extent)
				.AddDepth(cascade_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						  kShadowClearDepth)
				.Execute(cmd, std::forward<decltype(recordFn)>(recordFn));
		};

		for (uint32_t cascade = 0; cascade < RenderContext::Impl::NUM_CASCADES; ++cascade) {
			uint32_t drawCount = passDrawCounts[cascade];
			if (drawCount == 0) {
				continue;
			}

			ExecuteCascadePass(cascade, [&]() {
				Vk::DrawIndirect(
					cmd,
					{.pipeline = ctx.shadowPipeline.Get(),
					 .layout = ctx.shadowPipelineLayout.Get(),
					 .set = recorder.bindlessSet,
					 .argumentBuffer = ctx.shadowIndirectBuffers[recorder.frameIndex].Handle(),
					 .offset = passWriteOffsets[cascade] * sizeof(VkDrawIndirectCommand),
					 .drawCount = drawCount,
					 .stride = sizeof(VkDrawIndirectCommand)},
					ObjectConstants{.instanceId = kGpuCullingSentinel, .isShadowPass = cascade + 1},
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			});
		}
	}

	// 5. DISPATCH PUNCTUAL SHADOWS (MDI)
	if (ctx.punctualShadowPipeline.Valid() && !ctx.punctualShadowViews.empty()) {
		auto [atlas_att, scope] = Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
			cmd, ctx.shadowAtlas, VK_IMAGE_ASPECT_DEPTH_BIT);

		auto ExecutePunctualPass =
			[&](const Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>& subViewImage,
				auto&& recordFn) {
				Vk::DynamicPass(subViewImage.extent)
					.ViewMask(kCubemapFaceMask)
					.AddDepth(subViewImage, VK_ATTACHMENT_LOAD_OP_CLEAR,
							  VK_ATTACHMENT_STORE_OP_STORE, kShadowClearDepth)
					.Execute(cmd, std::forward<decltype(recordFn)>(recordFn));
			};

		for (uint32_t l_idx = 0; l_idx < ctx.mappedLights.size(); ++l_idx) {
			const auto& light = ctx.mappedLights[l_idx];
			if (light.shadowLayer < 0 || light.type != LightType::Point) {
				continue;
			}

			uint32_t slotIdx = 4 + light.shadowLayer;
			uint32_t drawCount = passDrawCounts[slotIdx];
			if (drawCount == 0) {
				continue;
			}

			Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> subViewImage = {
				.handle = ctx.shadowAtlas.image.Handle(),
				.view = ctx.punctualShadowViews[light.shadowLayer].Get(),
				.extent = {.width = 1024, .height = 1024},
				.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};

			ExecutePunctualPass(subViewImage, [&]() {
				const struct PunctualPush {
					uint32_t lightIdx;
				} pc = {l_idx};

				Vk::DrawIndirect(
					cmd,
					{.pipeline = ctx.punctualShadowPipeline.Get(),
					 .layout = ctx.punctualShadowPipelineLayout.Get(),
					 .set = recorder.bindlessSet,
					 .argumentBuffer = ctx.shadowIndirectBuffers[recorder.frameIndex].Handle(),
					 .offset = passWriteOffsets[slotIdx] * sizeof(VkDrawIndirectCommand),
					 .drawCount = drawCount,
					 .stride = sizeof(VkDrawIndirectCommand)},
					pc, VK_SHADER_STAGE_VERTEX_BIT);
			});
		}
	}
}

void MainPass::Execute(const FrameRecorder& recorder,
					   SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
									  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
						   in) const noexcept {
	VkCommandBuffer cmd = recorder.cmd;
	auto& ctx = recorder.ctx;

	Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																	  ctx.gpuProfiler);

	const auto [color_att, scope1] = Vk::ReadToColor(cmd, in.sceneColor);
	const auto [vel_att, scope2] = Vk::ReadToColor(cmd, in.velocity);
	const auto [norm_att, scope3] = Vk::ReadToColor(cmd, in.normRough);
	const auto [depth_att, scope4] =
		Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(cmd, in.depth,
																		 VK_IMAGE_ASPECT_DEPTH_BIT);

	const auto drawCount = static_cast<uint32_t>(ctx.drawQueue.size());
	if (drawCount == 0) {
		Vk::DynamicPass(color_att.extent)
			.AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorScene)
			.AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorVelocity)
			.AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorNormalRoughness)
			.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearDepthValue)
			.Execute(cmd, []() {});
		return;
	}

	ZHLN::Array<GroupRange> groups;
	groups.reserve((drawCount + 15) / 16);

	const NativeMaterial* currentMaterial = nullptr;

	for (uint32_t i = 0; i < drawCount; ++i) {
		const auto& drawCmd = ctx.drawQueue[i];
		const auto* const drawMat = ToNative(drawCmd.material);

		if (IsForwardOnly(drawCmd.instanceData.flags)) {
			currentMaterial = nullptr;
			continue;
		}

		if (i == 0 || drawMat != currentMaterial) {
			groups.push_back(GroupRange{
				.material = const_cast<NativeMaterial*>(drawMat), .start = i, .count = 1});
			currentMaterial = drawMat;
		} else {
			groups.back().count++;
		}
	}

	const bool useGpuCulling = [&]() {
		return ctx.cullingPass.pipeline.Valid() &&
			   ctx.indirectCommandsBuffers[recorder.frameIndex].Valid() &&
			   (drawCount <= kGpuCullingMaxInstances);
	}();

	if (useGpuCulling) {
		ExecutePass<GpuCullingPolicy>(recorder, groups, drawCount, color_att, vel_att, norm_att,
									  depth_att);
	} else {
		ExecutePass<CpuCullingPolicy>(recorder, groups, drawCount, color_att, vel_att, norm_att,
									  depth_att);
	}
}

[[nodiscard]] auto DeferredLightingPass::Execute(
	const FrameRecorder& recorder,
	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> in) const noexcept
	-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {

	VkCommandBuffer cmd = recorder.cmd;
	auto& ctx = recorder.ctx;
	VkDevice device = ctx.ctx.Device();
	uint32_t fIdx = recorder.frameIndex;

	// ughhh please Khronos wizards give us a better translation layer for MacOS
	auto GetTLAS = [](const FrameRecorder& rec) noexcept {
		if constexpr (isMac) {
			return Vk::SkipWrite{};
		} else {
			return rec.ctx.rtCtx.Valid() ? &rec.ctx.tlas.Current() : nullptr;
		}
	};

	Profiler::ScopedGpuProfile<Stages::PostProcessPass, FrameProfiler> timer(cmd, fIdx,
																			 ctx.gpuProfiler);

	struct PPPushConstants {
		JPH::Mat44 invViewProj;
		JPH::Mat44 viewProj;
		alignas(16) std::array<float, 4> camPos;
		int giMode;
		float aoRadius;
		float aoBias;
		float aoPower;
		float giIntensity;
		int giSamples;
		int enableSSR;
		int enableRTR;
		int _pad;
	} pc = {
		.invViewProj = ctx.current_view_proj.Inversed(),
		.viewProj = ctx.current_view_proj,
		.camPos = {ctx.currentUniforms.camPos[0], ctx.currentUniforms.camPos[1],
				   ctx.currentUniforms.camPos[2], ctx.currentUniforms.camPos[3]},
		.giMode = ctx.giSettings.mode,
		.aoRadius = ctx.giSettings.aoRadius,
		.aoBias = ctx.giSettings.aoBias,
		.aoPower = ctx.giSettings.aoPower,
		.giIntensity = ctx.giSettings.giIntensity,
		.giSamples = ctx.giSettings.giSamples,
		.enableSSR = ctx.giSettings.enableSSR,
		.enableRTR = (ctx.tlas.Current() != VK_NULL_HANDLE) ? ctx.giSettings.enableRTR : 0,
		._pad = {},
	};

	PostProcessResources res = {
		.gbuffer = {.sceneColor = in.sceneColor, .depth = in.depth, .normRough = in.normRough},
		.frameUniforms = ctx.frameUniformBuffers[fIdx],
		.defaultSampler = ctx.defaultSampler,
		.pointSampler = ctx.pointSampler,
		.clampSampler = ctx.clampSampler};

	auto ambient_ro = ctx.ambientPass.ExecuteWithTransitions(
		cmd, device, ctx.ambientTarget, pc,
		AmbientPassParams{.sceneColor = res.gbuffer.sceneColor,
						  .defaultSampler = res.defaultSampler.Get(),
						  .depth = res.gbuffer.depth,
						  .normRough = res.gbuffer.normRough,
						  .pointSampler = res.pointSampler.Get(),
						  .prefilteredView = ctx.iblPayload.prefilteredView.Get(),
						  .brdfLutView = ctx.iblPayload.brdfLutView.Get(),
						  .clampSampler = res.clampSampler.Get(),
						  .frameUniformBuffer = res.frameUniforms.Handle()});

	uint32_t lightVariant = DetermineLightingVariant(ctx.giSettings, ctx.rtCtx.Valid());

	auto light_ro = ctx.lightingPass.ExecuteVariantWithTransitions(
		cmd, device, ctx.lightingTarget, lightVariant, pc,
		LightingPassParams{.sceneColor = res.gbuffer.sceneColor,
						   .defaultSampler = res.defaultSampler.Get(),
						   .depth = res.gbuffer.depth,
						   .normRough = res.gbuffer.normRough,
						   .lightStorageBuffer = ctx.lightStorageBuffers[fIdx].Handle(),
						   .frameUniformBuffer = res.frameUniforms.Handle(),
						   .shadowMapView = ctx.shadowMap.view.Get(),
						   .shadowSampler = ctx.shadowSampler.Get(),
						   .ltcMatView = ctx.ltcMatView.Get(),
						   .ltcAmpView = ctx.ltcAmpView.Get(),
						   .clampSampler = res.clampSampler.Get(),
						   .clusterGridBuffer = ctx.clusterGridBuffers[fIdx].Handle(),
						   .lightIndexListBuffer = ctx.lightIndexListBuffers[fIdx].Handle(),
						   .ambientTarget = ambient_ro,
						   .pointSampler = res.pointSampler.Get(),
						   .tlas = GetTLAS(recorder),
						   .shadowAtlasCubeView = ctx.shadowAtlasCubeView.Get(),
						   .shadowAtlas2DView = ctx.shadowAtlas2DView.Get()});

	uint32_t reflVariant = DetermineReflectionVariant(ctx.giSettings, ctx.rtCtx.Valid());

	return ctx.reflectionPass.ExecuteVariantWithTransitions(
		cmd, device, ctx.postProcessTarget, reflVariant, pc,
		ReflectionPassParams{.sceneColor = res.gbuffer.sceneColor,
							 .defaultSampler = res.defaultSampler.Get(),
							 .depth = res.gbuffer.depth,
							 .normRough = res.gbuffer.normRough,
							 .pointSampler = res.pointSampler.Get(),
							 .prefilteredView = ctx.iblPayload.prefilteredView.Get(),
							 .tlas = GetTLAS(recorder),
							 .frameUniformBuffer = res.frameUniforms.Handle(),
							 .brdfLutView = ctx.iblPayload.brdfLutView.Get(),
							 .clampSampler = res.clampSampler.Get(),
							 .lightingTarget = light_ro});
}

[[nodiscard]] constexpr uint32_t
DeferredLightingPass::DetermineLightingVariant(const GISettings& gi, bool hasRt) const noexcept {
	return (gi.enableRTR && hasRt) ? 1 : 0;
}

[[nodiscard]] constexpr uint32_t
DeferredLightingPass::DetermineReflectionVariant(const GISettings& gi, bool hasRt) const noexcept {
	return (gi.enableSSR ? 1 : 0) | ((gi.enableRTR && hasRt) ? 2 : 0);
}

void ForwardPass::Execute(
	const FrameRecorder& recorder,
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> litColor,
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth) const noexcept {

	VkCommandBuffer cmd = recorder.cmd;
	const auto& ctx = recorder.ctx;

	const auto [color_att, color_scope] = Vk::ReadToColor(cmd, litColor);
	const auto [depth_att, depth_scope] =
		Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(cmd, depth,
																		 VK_IMAGE_ASPECT_DEPTH_BIT);

	Vk::DynamicPass(color_att.extent)
		.AddColor(color_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		.Execute(cmd, [&]() {
			for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
				const auto& drawCmd = ctx.drawQueue[i];
				if (IsForwardOnly(drawCmd.instanceData.flags) &&
					ToNative(drawCmd.material)->pipeline.Valid()) {
					const ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};
					SubmitDrawInstanced(cmd, drawCmd, i, recorder.bindlessSet, pushConstants);
				}
			}
		});
}

[[nodiscard]] auto
BloomPass::Execute(const FrameRecorder& recorder,
				   Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor) const noexcept
	-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
	VkCommandBuffer cmd = recorder.cmd;
	const auto& ctx = recorder.ctx;
	VkDevice device = ctx.ctx.Device();

	Profiler::ScopedGpuProfile<Stages::BloomPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																	   ctx.gpuProfiler);

	const auto bloomThreshold_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.bloomThresholdTarget);
	const auto bloomBlur_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.bloomBlurTarget);
	const auto bloomFinal_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.bloomFinalTarget);

	{
		const auto [bloomThreshold_att, scope] = Vk::ReadToColor(cmd, bloomThreshold_u);
		ctx.bloomThresholdPass.WriteNext(device, inColor, ctx.defaultSampler.Get());

		Vk::DynamicPass(ctx.bloomThresholdTarget.extent)
			.AddColor(bloomThreshold_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() { ctx.bloomThresholdPass.Execute(cmd); });
	}

	struct BlurPushConstants {
		int horizontal;
		float texelSize;
	};

	auto DispatchBlurPass = [&](auto& passObject, auto inputImage, auto targetImage, int horizontal,
								float texelSize) noexcept {
		const auto [targetAttachment, scope] = Vk::ReadToColor(cmd, targetImage);
		passObject.WriteNext(device, inputImage, ctx.defaultSampler.Get());

		Vk::DynamicPass(targetImage.extent)
			.AddColor(targetAttachment, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				passObject.Execute(
					cmd, BlurPushConstants{.horizontal = horizontal, .texelSize = texelSize});
			});
	};

	DispatchBlurPass(ctx.bloomBlurHPass, bloomThreshold_u, bloomBlur_u, 1,
					 1.0f / (float)ctx.bloomThresholdTarget.extent.width);

	DispatchBlurPass(ctx.bloomBlurVPass, bloomBlur_u, bloomFinal_u, 0,
					 1.0f / (float)ctx.bloomBlurTarget.extent.height);

	return bloomFinal_u;
}

[[nodiscard]] auto AAPass::Execute(const FrameRecorder& recorder, SceneRO in) const noexcept
	-> SceneRO {
	VkCommandBuffer cmd = recorder.cmd;
	auto& ctx = recorder.ctx;

	Profiler::ScopedGpuProfile<Stages::AAPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																	ctx.gpuProfiler);

	auto color_ro = in.sceneColor;

	if (ctx.aaState.mode == AAMode::TAA && ctx.taaPass.pipeline.Valid()) {
		color_ro = ExecuteTAA(cmd, recorder, in, color_ro);
	} else if (ctx.aaState.mode == AAMode::FXAA && ctx.fxaaPass.pipeline.Valid()) {
		color_ro = ExecuteFXAA(cmd, recorder, in, color_ro);
	} else if (ctx.aaState.mode == AAMode::SMAA && ctx.smaaEdgePass.pipeline.Valid()) {
		color_ro = ExecuteSMAA(cmd, recorder, in, color_ro);
	}

	return {.sceneColor = color_ro,
			.velocity = in.velocity,
			.normRough = in.normRough,
			.depth = in.depth};
}

[[nodiscard]] auto AAPass::ExecuteTAA(VkCommandBuffer cmd, const FrameRecorder& recorder,
									  const SceneRO& in, ColorImageRO color_ro) const noexcept
	-> ColorImageRO {
	const auto& ctx = recorder.ctx;
	const auto accumCurr_ro =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Current());
	const auto accumNext_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

	{
		auto [accumNext_att, scope] = Vk::ReadToColor(cmd, accumNext_u);

		struct TAAPushConstants {
			float feedback;
		};

		Vk::DynamicPass(in.sceneColor.extent)
			.AddColor(accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				ctx.taaPass.WriteNext(ctx.ctx.Device(), color_ro, accumCurr_ro, in.velocity,
									  ctx.defaultSampler.Get(),
									  ctx.frameUniformBuffers[recorder.frameIndex].Handle());

				ctx.taaPass.Execute(cmd, TAAPushConstants{.feedback = ctx.aaState.taaFeedback});
			});
	}

	return accumNext_u;
}

[[nodiscard]] auto AAPass::ExecuteFXAA(VkCommandBuffer cmd, const FrameRecorder& recorder,
									   const SceneRO& in, ColorImageRO color_ro) const noexcept
	-> ColorImageRO {
	const auto& ctx = recorder.ctx;
	const auto accumNext_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

	{
		auto [accumNext_att, scope] = Vk::ReadToColor(cmd, accumNext_u);

		struct FXAAPushConstants {
			float rcpFrameX;
			float rcpFrameY;
			float subpix;
			float edgeThreshold;
			float edgeThresholdMin;
			float _pad;
		} pc = {.rcpFrameX = 1.0f / (float)in.sceneColor.extent.width,
				.rcpFrameY = 1.0f / (float)in.sceneColor.extent.height,
				.subpix = ctx.aaState.fxaaSubpix,
				.edgeThreshold = ctx.aaState.fxaaEdgeThreshold,
				.edgeThresholdMin = ctx.aaState.fxaaEdgeThresholdMin,
				._pad = 0.0f};

		Vk::DynamicPass(in.sceneColor.extent)
			.AddColor(accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				ctx.fxaaPass.WriteNext(ctx.ctx.Device(), color_ro, ctx.defaultSampler.Get());
				ctx.fxaaPass.Execute(cmd, pc);
			});
	}

	return accumNext_u;
}

[[nodiscard]] auto AAPass::ExecuteSMAA(VkCommandBuffer cmd, const FrameRecorder& recorder,
									   const SceneRO& in, ColorImageRO color_ro) const noexcept
	-> ColorImageRO {
	const auto& ctx = recorder.ctx;
	struct SMAAMetrics {
		float rcpWidth;
		float rcpHeight;
		float width;
		float height;
	} metrics = {.rcpWidth = 1.0f / (float)in.sceneColor.extent.width,
				 .rcpHeight = 1.0f / (float)in.sceneColor.extent.height,
				 .width = (float)in.sceneColor.extent.width,
				 .height = (float)in.sceneColor.extent.height};

	auto ExecuteSubPass = [&](auto& targetImage, VkAttachmentLoadOp loadOp, auto&& recordFn) {
		auto [attachment, scope] = Vk::ReadToColor(cmd, targetImage);
		Vk::DynamicPass(in.sceneColor.extent)
			.AddColor(attachment, loadOp, VK_ATTACHMENT_STORE_OP_STORE, kClearColorBlack)
			.Execute(cmd, std::forward<decltype(recordFn)>(recordFn));
	};

	const auto smaaEdge_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.smaaEdgeTarget);
	const auto smaaEdge_ro = smaaEdge_u;

	ExecuteSubPass(smaaEdge_u, VK_ATTACHMENT_LOAD_OP_CLEAR, [&]() {
		ctx.smaaEdgePass.WriteNext(ctx.ctx.Device(), color_ro, ctx.defaultSampler.Get(),
								   ctx.pointSampler.Get());
		ctx.smaaEdgePass.Execute(cmd, metrics,
								 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	});

	const auto smaaWeight_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.smaaWeightTarget);
	const auto smaaWeight_ro = smaaWeight_u;

	ExecuteSubPass(smaaWeight_u, VK_ATTACHMENT_LOAD_OP_CLEAR, [&]() {
		const auto& [areaView, searchView] =
			std::tie(ctx.textureViews[ctx.smaaAreaTexIdx], ctx.textureViews[ctx.smaaSearchTexIdx]);

		ctx.smaaWeightPass.WriteNext(ctx.ctx.Device(), smaaEdge_ro, areaView, searchView,
									 ctx.defaultSampler.Get(), ctx.pointSampler.Get());
		ctx.smaaWeightPass.Execute(cmd, metrics,
								   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	});

	const auto accumNext_u =
		AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

	ExecuteSubPass(accumNext_u, VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
		ctx.smaaBlendPass.WriteNext(ctx.ctx.Device(), color_ro, smaaWeight_ro,
									ctx.defaultSampler.Get(), ctx.pointSampler.Get());
		ctx.smaaBlendPass.Execute(cmd, metrics,
								  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	});

	return accumNext_u;
}

void BlitPass::Execute(
	const FrameRecorder& recorder, Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> bloomColor) const noexcept {
	VkCommandBuffer cmd = recorder.cmd;
	auto& ctx = recorder.ctx;

	Profiler::ScopedGpuProfile<Stages::BlitPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																	  ctx.gpuProfiler);

	uint32_t imageIdx = ctx.current_image_index;
	Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_u = {
		.handle = ctx.presentation.swapchain.Get().images[imageIdx],
		.view = ctx.presentation.swapchain.Get().views[imageIdx],
		.extent = inColor.extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	// 1. One-way transition for Swapchain (Undefined -> ColorAttachment)
	auto swap_att = Vk::IssueBarrier<Vk::UndefinedState, Vk::ColorAttachmentState>(cmd, swap_u);

	ctx.blitPass.WriteNext(ctx.ctx.Device(), inColor, ctx.defaultSampler.Get(), bloomColor);

	RenderContext::Impl::BlitPushConstants pc = {.vignetteIntensity =
													 ctx.giSettings.vignetteIntensity,
												 .vignettePower = ctx.giSettings.vignettePower,
												 .fullBright = ctx.currentUniforms.fullBright};

	if (ctx.blitPass.pipeline.Valid()) {
		Vk::DynamicPass(inColor.extent)
			.AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				ctx.blitPass.Execute(cmd, pc);
				if (!ctx.uiDrawQueue.empty()) {
					UIObjectConstants uipc{};
					uipc.orthoMatrix =
						Math::CreateOrthoMatrix(inColor.extent.width, inColor.extent.height);

					VkRect2D defaultScissor = {
						.offset = {.x = 0, .y = 0},
						.extent = {.width = inColor.extent.width, .height = inColor.extent.height}};

					for (const auto& draw : ctx.uiDrawQueue) {
						uipc.albedoIdx = draw.fontIndex;
						uipc.posAddress = draw.posMesh->vboAddress;
						uipc.attrAddress = draw.attrMesh->vboAddress;

						Vk::ScopedScissor scissorGuard(
							cmd,
							Vk::ScopedScissor::ScissorDesc{
								.target =
									draw.useScissor
										? VkRect2D{.offset = {.x = draw.scissorRect.x,
															  .y = draw.scissorRect.y},
												   .extent = {.width = draw.scissorRect.width,
															  .height = draw.scissorRect.height}}
										: defaultScissor,
								.fallback = defaultScissor});

						Vk::DrawInstanced(cmd,
										  {.pipeline = ctx.uiPipeline.Get(),
										   .layout = ctx.uiPipelineLayout.Get(),
										   .set = recorder.bindlessSet,
										   .vertexCount = draw.posMesh->vertexCount},
										  uipc);
					}
					ctx.uiDrawQueue.clear();
				}
				if (!ctx.window.IsTTY()) {
					ImGui::Render();
					ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
				}
			});
	}

	// 2. Final pipeline barrier to pass off to OS presentation engine (ColorAttachment -> Present)
	Vk::IssueBarrier<Vk::ColorAttachmentState, Vk::PresentState>(cmd, swap_att);
}

} // namespace Passes
} // namespace ZHLN
