// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// --- src/engine/RenderFrame.cpp ---

#include "ParallelDraw.hpp"
#include "RenderInternal.hpp"
#include "RenderParams.hpp"
#include "Zahlen/GUI.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "detail/RadixSort.hpp"
#include "imgui.h"
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/QuickSort.h>
// clang-format on

#include <array>
#include <bit>
#include <threading/TaskSystem.hpp>

namespace ZHLN {
namespace {
// ============================================================================
// RenderPass Concepts & Interface Structures
// ============================================================================

template <typename T, typename... Args>
concept IsRenderPass = requires(T pass, Args&&... args) {
	{ pass.Execute(std::forward<Args>(args)...) };
};

template <typename Pass, typename... Args>
	requires IsRenderPass<Pass, Args...>
void RunPass(const Pass& pass, Args&&... args) {
	pass.Execute(std::forward<Args>(args)...);
}

// ============================================================================
// Frame Recording Context
// ============================================================================

struct FrameRecorder {
	VkCommandBuffer cmd;
	RenderContext::Impl& ctx;
	uint32_t frameIndex;
	VkDescriptorSet bindlessSet;

	FrameRecorder(VkCommandBuffer c, RenderContext::Impl& impl) noexcept
		: cmd(c), ctx(impl), frameIndex(impl.frame_index),
		  bindlessSet(impl.bindlessSets[impl.frame_index]) {}
};

template <VkImageLayout ColorL, VkImageLayout DepthL> struct SceneResources {
	Vk::TypedImage<ColorL> sceneColor;
	Vk::TypedImage<ColorL> velocity;
	Vk::TypedImage<ColorL> normRough;
	Vk::TypedImage<DepthL> depth;
};

struct GroupRange {
	NativeMaterial* material;
	uint32_t start;
	uint32_t count;
};

// ============================================================================
// Culling Policies
// ============================================================================

struct GpuCullingPolicy {
	static void
	Record(const FrameRecorder& recorder, const JPH::Array<GroupRange>& groups, uint32_t drawCount,
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

		// Block compute writes until indirect drawing parameters are read by the graphics hardware
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
	Record(const FrameRecorder& recorder, const JPH::Array<GroupRange>& /*groups*/,
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
						if (!drawCmd.material->pipeline.Valid() || drawCmd.alphaMode == 2) {
							return;
						}

						ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};

						Vk::DrawInstanced(
							sec_cmd,
							{.pipeline =
								 std::bit_cast<NativeMaterial*>(drawCmd.material)->pipeline.Get(),
							 .layout =
								 std::bit_cast<NativeMaterial*>(drawCmd.material)->layout.Get(),
							 .set = recorder.bindlessSet,
							 .vertexCount =
								 drawCmd.indexMesh
									 ? drawCmd.indexCount
									 : std::bit_cast<NativeMesh*>(drawCmd.mesh)->vertexCount,
							 .instanceCount = 1,
							 .firstVertex = 0,
							 .firstInstance = i},
							pushConstants);
					});
			});
	}
};

template <typename CullingPolicy, typename... Args>
void ExecutePass(const FrameRecorder& recorder, const JPH::Array<GroupRange>& groups,
				 uint32_t drawCount, Args&&... args) {
	CullingPolicy::Record(recorder, groups, drawCount, std::forward<Args>(args)...);
}
} // namespace

// ============================================================================
// Render Passes
// ============================================================================

namespace Passes {

namespace {

struct ShadowPass {
	void Execute(const FrameRecorder& recorder) const noexcept {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		Profiler::ScopedGpuProfile<Stages::ShadowPass, FrameProfiler> timer(
			cmd, recorder.frameIndex, ctx.gpuProfiler);

		// ====================================================================
		// 1. DIRECTIONAL SHADOW PASS (Sun)
		// ====================================================================
		{
			auto [shadow_att, scope] =
				Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
					cmd, ctx.shadowMap, VK_IMAGE_ASPECT_DEPTH_BIT);

			Vk::DynamicPass(ctx.shadowMap.extent)
				.AddDepth(shadow_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						  1.0f)
				.Execute(cmd, [&]() {
					for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
						const auto& draw = ctx.drawQueue[i];
						if (draw.alphaMode == 2) {
							continue;
						}
						auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh);

						ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 1};
						uint32_t vCount = draw.indexMesh ? draw.indexCount : mesh->vertexCount;

						Vk::DrawInstanced(cmd,
										  {.pipeline = ctx.shadowPipeline.Get(),
										   .layout = ctx.shadowPipelineLayout.Get(),
										   .set = recorder.bindlessSet,
										   .vertexCount = vCount,
										   .instanceCount = 1,
										   .firstVertex = 0,
										   .firstInstance = i},
										  pushConstants,
										  VK_SHADER_STAGE_VERTEX_BIT |
											  VK_SHADER_STAGE_FRAGMENT_BIT);
					}
				});
		}

		// ====================================================================
		// 2. PUNCTUAL SHADOW PASSES (Point Lights via Multiview)
		// ====================================================================
		if (ctx.punctualShadowPipeline.Valid() && !ctx.punctualShadowViews.empty()) {
			auto [atlas_att, scope] =
				Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
					cmd, ctx.shadowAtlas, VK_IMAGE_ASPECT_DEPTH_BIT);

			for (uint32_t l_idx = 0; l_idx < ctx.mappedLights.size(); ++l_idx) {
				const auto& light = ctx.mappedLights[l_idx];
				if (light.shadowLayer < 0 || light.type != 1) {
					continue;
				}

				Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> subViewImage = {
					.handle = ctx.shadowAtlas.image.Handle(),
					.view = ctx.punctualShadowViews[light.shadowLayer].Get(),
					.extent = {.width = 1024, .height = 1024},
					.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};

				Vk::DynamicPass(subViewImage.extent)
					.ViewMask(63)
					.AddDepth(subViewImage, VK_ATTACHMENT_LOAD_OP_CLEAR,
							  VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
					.Execute(cmd, [&]() {
						for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
							const auto& draw = ctx.drawQueue[i];
							if (draw.alphaMode == 2) {
								continue;
							}

							JPH::Vec3 meshPos = draw.transform.GetTranslation();
							JPH::Vec3 lightPos(light.position[0], light.position[1],
											   light.position[2]);
							float distToLight = (meshPos - lightPos).Length();
							float maxRange = light.range + draw.cullRadius;

							if (distToLight > maxRange) {
								continue;
							}

							auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh);

							struct PunctualPush {
								uint32_t lightIdx;
							} pc = {l_idx};
							uint32_t vCount = draw.indexMesh ? draw.indexCount : mesh->vertexCount;

							Vk::DrawInstanced(cmd,
											  {.pipeline = ctx.punctualShadowPipeline.Get(),
											   .layout = ctx.punctualShadowPipelineLayout.Get(),
											   .set = recorder.bindlessSet,
											   .vertexCount = vCount,
											   .instanceCount = 1,
											   .firstVertex = 0,
											   .firstInstance = i},
											  pc, VK_SHADER_STAGE_VERTEX_BIT);
						}
					});
			}
		}
	}
};

struct MainPass {
	void Execute(const FrameRecorder& recorder,
				 SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
					 in) const noexcept {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																		  ctx.gpuProfiler);

		auto [color_att, scope1] = Vk::ReadToColor(cmd, in.sceneColor);
		auto [vel_att, scope2] = Vk::ReadToColor(cmd, in.velocity);
		auto [norm_att, scope3] = Vk::ReadToColor(cmd, in.normRough);
		auto [depth_att, scope4] = Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
			cmd, in.depth, VK_IMAGE_ASPECT_DEPTH_BIT);

		auto drawCount = static_cast<uint32_t>(ctx.drawQueue.size());
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

		JPH::Array<GroupRange> groups;
		groups.reserve((drawCount + 15) / 16);

		NativeMaterial* currentMaterial = nullptr;

		for (uint32_t i = 0; i < drawCount; ++i) {
			const auto& drawCmd = ctx.drawQueue[i];
			auto* drawMat = std::bit_cast<NativeMaterial*>(drawCmd.material);

			if (drawCmd.alphaMode == 2) {
				currentMaterial = nullptr;
				continue;
			}

			if (i == 0 || drawMat != currentMaterial) {
				groups.push_back(GroupRange{.material = drawMat, .start = i, .count = 1});
				currentMaterial = drawMat;
			} else {
				groups.back().count++;
			}
		}

		bool useGpuCulling = ctx.cullingPass.pipeline.Valid() &&
							 ctx.indirectCommandsBuffers[recorder.frameIndex].Valid() &&
							 (drawCount <= kGpuCullingMaxInstances);

		if (useGpuCulling) {
			ExecutePass<GpuCullingPolicy>(recorder, groups, drawCount, color_att, vel_att, norm_att,
										  depth_att);
		} else {
			ExecutePass<CpuCullingPolicy>(recorder, groups, drawCount, color_att, vel_att, norm_att,
										  depth_att);
		}
	}
};

struct DeferredLightingPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> in) const noexcept
		-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {

		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;
		VkDevice device = ctx.ctx.Device();
		uint32_t fIdx = recorder.frameIndex;

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

		// --- 1. AMBIENT PASS ---

		auto ambient_ro = ctx.ambientPass.ExecuteWithTransitions(
			cmd, device, ctx.ambientTarget, pc,
			AmbientPassParams{.sceneColor = in.sceneColor,
							  .defaultSampler = ctx.defaultSampler.Get(),
							  .depth = in.depth,
							  .normRough = in.normRough,
							  .pointSampler = ctx.pointSampler.Get(),
							  .prefilteredView = ctx.iblPayload.prefilteredView.Get(),
							  .brdfLutView = ctx.iblPayload.brdfLutView.Get(),
							  .clampSampler = ctx.clampSampler.Get(),
							  .frameUniformBuffer = ctx.frameUniformBuffers[fIdx].Handle()});

		// --- 2. LIGHTING PASS ---
		uint32_t lightVariant = (pc.enableRTR && ctx.rtCtx.Valid()) ? 1 : 0;

		auto light_ro = ctx.lightingPass.ExecuteVariantWithTransitions(
			cmd, device, ctx.lightingTarget, lightVariant, pc,
			LightingPassParams{.sceneColor = in.sceneColor,
							   .defaultSampler = ctx.defaultSampler.Get(),
							   .depth = in.depth,
							   .normRough = in.normRough,
							   .lightStorageBuffer = ctx.lightStorageBuffers[fIdx].Handle(),
							   .frameUniformBuffer = ctx.frameUniformBuffers[fIdx].Handle(),
							   .shadowMapView = ctx.shadowMap.view.Get(),
							   .shadowSampler = ctx.shadowSampler.Get(),
							   .ltcMatView = ctx.ltcMatView.Get(),
							   .ltcAmpView = ctx.ltcAmpView.Get(),
							   .clampSampler = ctx.clampSampler.Get(),
							   .clusterGridBuffer = ctx.clusterGridBuffers[fIdx].Handle(),
							   .lightIndexListBuffer = ctx.lightIndexListBuffers[fIdx].Handle(),
							   .ambientTarget = ambient_ro,
							   .pointSampler = ctx.pointSampler.Get(),
							   .tlas = ctx.rtCtx.Valid() ? &ctx.tlas.Current() : nullptr,
							   .shadowAtlasCubeView = ctx.shadowAtlasCubeView.Get(),
							   .shadowAtlas2DView = ctx.shadowAtlas2DView.Get()});

		// --- 3. REFLECTION PASS ---
		uint32_t reflVariant =
			(pc.enableSSR ? 1 : 0) | ((pc.enableRTR && ctx.rtCtx.Valid()) ? 2 : 0);

		return ctx.reflectionPass.ExecuteVariantWithTransitions(
			cmd, device, ctx.postProcessTarget, reflVariant, pc,
			ReflectionPassParams{.sceneColor = in.sceneColor,
								 .defaultSampler = ctx.defaultSampler.Get(),
								 .depth = in.depth,
								 .normRough = in.normRough,
								 .pointSampler = ctx.pointSampler.Get(),
								 .prefilteredView = ctx.iblPayload.prefilteredView.Get(),
								 .tlas = ctx.rtCtx.Valid() ? &ctx.tlas.Current() : nullptr,
								 .frameUniformBuffer = ctx.frameUniformBuffers[fIdx].Handle(),
								 .brdfLutView = ctx.iblPayload.brdfLutView.Get(),
								 .clampSampler = ctx.clampSampler.Get(),
								 .lightingTarget = light_ro});
	}
};

struct ForwardPass {
	void Execute(const FrameRecorder& recorder,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> litColor,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth) const noexcept {

		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		auto [color_att, color_scope] = Vk::ReadToColor(cmd, litColor);
		auto [depth_att, depth_scope] =
			Vk::ScopedBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
				cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT);

		Vk::DynamicPass(color_att.extent)
			.AddColor(color_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
			.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
			.Execute(cmd, [&]() {
				for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
					const auto& drawCmd = ctx.drawQueue[i];
					if (drawCmd.alphaMode == 2 && drawCmd.material->pipeline.Valid()) {
						ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};

						Vk::DrawInstanced(
							cmd,
							{.pipeline =
								 std::bit_cast<NativeMaterial*>(drawCmd.material)->pipeline.Get(),
							 .layout =
								 std::bit_cast<NativeMaterial*>(drawCmd.material)->layout.Get(),
							 .set = recorder.bindlessSet,
							 .vertexCount =
								 drawCmd.indexMesh
									 ? drawCmd.indexCount
									 : std::bit_cast<NativeMesh*>(drawCmd.mesh)->vertexCount,
							 .instanceCount = 1,
							 .firstVertex = 0,
							 .firstInstance = i},
							pushConstants);
					}
				}
			});
	}
};

struct BloomPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor) const noexcept
		-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;
		VkDevice device = ctx.ctx.Device();

		Profiler::ScopedGpuProfile<Stages::BloomPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																		   ctx.gpuProfiler);

		auto bloomThreshold_u =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.bloomThresholdTarget);
		auto bloomBlur_u =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.bloomBlurTarget);
		auto bloomFinal_u =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.bloomFinalTarget);

		// Pass A: Brightness Thresholding & Downsampling
		{
			auto [bloomThreshold_att, scope] = Vk::ReadToColor(cmd, bloomThreshold_u);
			ctx.bloomThresholdPass.WriteNext(device, inColor, ctx.defaultSampler.Get());

			Vk::DynamicPass(ctx.bloomThresholdTarget.extent)
				.AddColor(bloomThreshold_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() { ctx.bloomThresholdPass.Execute(cmd); });
		}

		// Pass B: Horizontal Gaussian Blur
		{
			auto [bloomBlur_att, scope] = Vk::ReadToColor(cmd, bloomBlur_u);
			ctx.bloomBlurHPass.WriteNext(device, bloomThreshold_u, ctx.defaultSampler.Get());

			struct BlurPushConstants {
				int horizontal;
				float texelSize;
			};

			Vk::DynamicPass(ctx.bloomBlurTarget.extent)
				.AddColor(bloomBlur_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() {
					ctx.bloomBlurHPass.Execute(
						cmd, BlurPushConstants{
								 .horizontal = 1,
								 .texelSize = 1.0f / (float)ctx.bloomThresholdTarget.extent.width});
				});
		}

		// Pass C: Vertical Gaussian Blur (Writing final Bloom Output)
		{
			auto [bloomFinal_att, scope] = Vk::ReadToColor(cmd, bloomFinal_u);
			ctx.bloomBlurVPass.WriteNext(device, bloomBlur_u, ctx.defaultSampler.Get());

			struct BlurPushConstants {
				int horizontal;
				float texelSize;
			};

			Vk::DynamicPass(ctx.bloomFinalTarget.extent)
				.AddColor(bloomFinal_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() {
					ctx.bloomBlurVPass.Execute(
						cmd, BlurPushConstants{
								 .horizontal = 0,
								 .texelSize = 1.0f / (float)ctx.bloomBlurTarget.extent.height});
				});
		}

		return bloomFinal_u;
	}
};

struct AAPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> in) const noexcept
		-> SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		Profiler::ScopedGpuProfile<Stages::AAPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																		ctx.gpuProfiler);

		auto color_ro = in.sceneColor;
		auto vel_ro = in.velocity;
		auto norm_ro = in.normRough;
		auto depth_ro = in.depth;

		if (ctx.aaState.mode == AAMode::TAA && ctx.taaPass.pipeline.Valid()) {
			auto accumCurr_ro =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Current());
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

			// Open transition scope
			{
				auto [accumNext_att, scope] = Vk::ReadToColor(cmd, accumNext_u);

				struct TAAPushConstants {
					float feedback;
				};

				Vk::DynamicPass(in.sceneColor.extent)
					.AddColor(accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
					.Execute(cmd, [&]() {
						ctx.taaPass.WriteNext(
							ctx.ctx.Device(), color_ro, accumCurr_ro, in.velocity,
							ctx.defaultSampler.Get(),
							ctx.frameUniformBuffers[recorder.frameIndex].Handle());

						ctx.taaPass.Execute(cmd,
											TAAPushConstants{.feedback = ctx.aaState.taaFeedback});
					});
			}

			color_ro = accumNext_u;

		} else if (ctx.aaState.mode == AAMode::FXAA && ctx.fxaaPass.pipeline.Valid()) {
			auto accumNext_u =
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
						ctx.fxaaPass.WriteNext(ctx.ctx.Device(), color_ro,
											   ctx.defaultSampler.Get());
						ctx.fxaaPass.Execute(cmd, pc);
					});
			}

			color_ro = accumNext_u;

		} else if (ctx.aaState.mode == AAMode::SMAA && ctx.smaaEdgePass.pipeline.Valid()) {

			struct SMAAMetrics {
				float rcpWidth;
				float rcpHeight;
				float width;
				float height;
			} metrics = {.rcpWidth = 1.0f / (float)in.sceneColor.extent.width,
						 .rcpHeight = 1.0f / (float)in.sceneColor.extent.height,
						 .width = (float)in.sceneColor.extent.width,
						 .height = (float)in.sceneColor.extent.height};

			// --- PASS 1: EDGE DETECTION ---
			auto smaaEdge_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.smaaEdgeTarget);
			auto smaaEdge_ro = smaaEdge_u;

			{
				auto [smaaEdge_att, scope] = Vk::ReadToColor(cmd, smaaEdge_u);

				Vk::DynamicPass(in.sceneColor.extent)
					.AddColor(smaaEdge_att, VK_ATTACHMENT_LOAD_OP_CLEAR,
							  VK_ATTACHMENT_STORE_OP_STORE, kClearColorBlack)
					.Execute(cmd, [&]() {
						ctx.smaaEdgePass.WriteNext(ctx.ctx.Device(), color_ro,
												   ctx.defaultSampler.Get());
						ctx.smaaEdgePass.Execute(cmd, metrics,
												 VK_SHADER_STAGE_VERTEX_BIT |
													 VK_SHADER_STAGE_FRAGMENT_BIT);
					});
			}

			// --- PASS 2: BLENDING WEIGHT CALCULATION ---
			auto smaaWeight_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.smaaWeightTarget);
			auto smaaWeight_ro = smaaWeight_u;

			{
				auto [smaaWeight_att, scope] = Vk::ReadToColor(cmd, smaaWeight_u);

				VkImageView areaView = ctx.textureViews[ctx.smaaAreaTexIdx].Get();
				VkImageView searchView = ctx.textureViews[ctx.smaaSearchTexIdx].Get();

				Vk::DynamicPass(in.sceneColor.extent)
					.AddColor(smaaWeight_att, VK_ATTACHMENT_LOAD_OP_CLEAR,
							  VK_ATTACHMENT_STORE_OP_STORE, kClearColorBlack)
					.Execute(cmd, [&]() {
						ctx.smaaWeightPass.WriteNext(ctx.ctx.Device(), smaaEdge_ro, areaView,
													 searchView, ctx.defaultSampler.Get(),
													 ctx.pointSampler.Get());
						ctx.smaaWeightPass.Execute(cmd, metrics,
												   VK_SHADER_STAGE_VERTEX_BIT |
													   VK_SHADER_STAGE_FRAGMENT_BIT);
					});
			}

			// --- PASS 3: NEIGHBORHOOD BLENDING ---
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

			{
				auto [accumNext_att, scope] = Vk::ReadToColor(cmd, accumNext_u);

				Vk::DynamicPass(in.sceneColor.extent)
					.AddColor(accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
					.Execute(cmd, [&]() {
						ctx.smaaBlendPass.WriteNext(ctx.ctx.Device(), color_ro, smaaWeight_ro,
													ctx.defaultSampler.Get());
						ctx.smaaBlendPass.Execute(cmd, metrics,
												  VK_SHADER_STAGE_VERTEX_BIT |
													  VK_SHADER_STAGE_FRAGMENT_BIT);
					});
			}

			color_ro = accumNext_u;
		}

		return {
			.sceneColor = color_ro, .velocity = vel_ro, .normRough = norm_ro, .depth = depth_ro};
	}
};

struct BlitPass {
	void
	Execute(const FrameRecorder& recorder,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
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

		// 1. One-way transition for Swapchain
		auto swap_att = Vk::IssueBarrier<Vk::UndefinedState, Vk::ColorAttachmentState>(cmd, swap_u);

		ctx.blitPass.WriteNext(ctx.ctx.Device(), inColor, ctx.defaultSampler.Get(), bloomColor);

		struct BlitPushConstants {
			float vignetteIntensity;
			float vignettePower;
		} pc = {.vignetteIntensity = ctx.giSettings.vignetteIntensity,
				.vignettePower = ctx.giSettings.vignettePower};

		if (ctx.blitPass.pipeline.Valid()) {
			Vk::DynamicPass(inColor.extent)
				.AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() {
					ctx.blitPass.Execute(cmd, pc);
					if (!ctx.uiDrawQueue.empty()) {
						UIObjectConstants uipc{};

						uipc.orthoMatrix =
							GUI::CreateOrthoMatrix(inColor.extent.width, inColor.extent.height);

						for (const auto& draw : ctx.uiDrawQueue) {
							uipc.albedoIdx = draw.fontIndex;
							uipc.vboAddress =
								std::bit_cast<NativeMesh*>(draw.mesh)->vboAddress; // Map UI buffer

							Vk::DrawInstanced(
								cmd,
								{.pipeline = ctx.uiPipeline.Get(),
								 .layout = ctx.uiPipelineLayout.Get(),
								 .set = recorder.bindlessSet,
								 .vertexCount = std::bit_cast<NativeMesh*>(draw.mesh)->vertexCount},
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

		// 2. Final pipeline barrier to pass off to OS presentation engine
		Vk::IssueBarrier<Vk::ColorAttachmentState, Vk::PresentState>(cmd, swap_att);
	}
};
} // namespace

} // namespace Passes

// ============================================================================
// RenderFrame Implementations
// ============================================================================

void RenderContext::Impl::SortDrawQueue() {
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	JPH::Array<SortItem> items(drawCount);
	JPH::Array<SortItem> temp(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		items[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].mesh), .payload = i};
	}

	RadixSort64(items.data(), temp.data(), drawCount);

	JPH::Array<DrawCommand> sortedDrawQueue(drawCount);
	for (uint32_t i = 0; i < drawCount; ++i) {
		sortedDrawQueue[i] = drawQueue[items[i].payload];
	}
	drawQueue = std::move(sortedDrawQueue);
}

std::optional<Extent2D> RenderContext::GetFramebufferSize() const {
	Extent2D size = _impl->window.GetSize();
	if (size.width == 0 || size.height == 0) {
		return std::nullopt;
	}
	return size;
}

namespace {
inline RenderFrameResult MapFrameResult(ZHLN_FrameResult res) noexcept {
	switch (res) {
		case ZHLN_FrameResult_Ok:
			return RenderFrameResult::Success;
		case ZHLN_FrameResult_Suboptimal:
			return RenderFrameResult::Suboptimal;
		case ZHLN_FrameResult_OutOfDate:
			return RenderFrameResult::OutOfDate;
		case ZHLN_FrameResult_DeviceLost:
			return RenderFrameResult::DeviceLost;
		default:
			return RenderFrameResult::Error;
	}
}
} // namespace

void RenderContext::Impl::DispatchSkinningPasses() {
	bool hasSkinned = false;
	for (const auto& drawCmd : drawQueue) {
		if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
			hasSkinned = true;
			break;
		}
	}
	if (!hasSkinned) {
		return;
	}

	ZHLN_PROFILE_SCOPE("GPU Compute Skinning");
	VkCommandBuffer cmd = current_cmd;
	skinningPass.Bind(cmd);

	for (const auto& drawCmd : drawQueue) {
		if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
			auto* inMesh = std::bit_cast<NativeMesh*>(drawCmd.mesh);
			auto* outMesh = meshPool.Resolve(static_cast<uint64_t>(drawCmd.skinnedVertexBuffer));

			if (inMesh == nullptr || outMesh == nullptr) {
				continue;
			}

			SkinningConstants pcs = {.inputVerticesAddr = inMesh->vboAddress,
									 .outputVerticesAddr = outMesh->vboAddress,
									 .jointsAddr = Vk::GetBufferDeviceAddress(
										 ctx.Device(), jointBuffers[frame_index].Handle()),
									 .vertexCount = inMesh->vertexCount,
									 .jointOffset = drawCmd.jointOffset};

			skinningPass.PushConstants(cmd, pcs);
			skinningPass.Dispatch(cmd, (inMesh->vertexCount + 63) / 64, 1, 1);
		}
	}

	// Sync skinning output writes to vertex shader reads (VBO address fetch)
	Vk::BarrierBuilder()
		.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
		.To(cmd, Vk::BarrierStage::Vertex, Vk::BarrierAccess::ShaderRead);
}

RenderResult RenderContext::BeginFrame() noexcept {
	if (_impl->stagingContext) {
		_impl->stagingContext->Wait();
		_impl->stagingContext.reset(); // Wait, cleanup, and destroy the fence automatically
	}

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	auto wait_res =
		ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, _impl->pools[_impl->frame_index]);
	if (wait_res == ZHLN_FrameResult_DeviceLost) {
		return std::unexpected(RenderFrameResult::DeviceLost);
	}

	float timestampPeriod = _impl->ctx.PhysicalInfo().properties.properties.limits.timestampPeriod;
	_impl->gpuProfiler.RetrieveResults(
		_impl->frame_index, timestampPeriod,
		[](std::string_view name, float durationMS) { CPUProfiler::Record(name, durationMS); });
	_impl->gpuProfiler.Reset(_impl->frame_index);

	for (auto& worker : _impl->workerCmds) {
		worker.cmdCount[_impl->frame_index].store(0, std::memory_order_relaxed);
		worker.pools[_impl->frame_index].Reset();
	}

	if (_impl->resized) {
		auto fbSize = GetFramebufferSize();
		if (!fbSize.has_value()) {
			return std::unexpected(RenderFrameResult::OutOfDate);
		}

		VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

		if (!_impl->presentation.Rebuild(ext.width, ext.height)) {
			return std::unexpected(RenderFrameResult::Error);
		}

		_impl->sceneColor = Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->velocityBuffer = Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->accumBuffers[0] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->accumBuffers[1] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->normalRoughnessBuffer = Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->postProcessTarget = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->ambientTarget = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->lightingTarget = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->smaaEdgeTarget = Vk::RenderTarget<VK_FORMAT_R8G8_UNORM>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->smaaWeightTarget = Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});

		// Allocate bloom targets at 1/4 resolution of the viewport
		VkExtent2D bloomExt = {.width = std::max(1u, ext.width / 4),
							   .height = std::max(1u, ext.height / 4)};
		_impl->bloomThresholdTarget = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, bloomExt,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->bloomBlurTarget = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, bloomExt,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->bloomFinalTarget = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, bloomExt,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});

		// Safely clearing the vector automatically destroys all old Vulkan image views
		_impl->punctualShadowViews.clear();

		uint32_t maxPointLights = 4;
		_impl->punctualShadowViews.resize(maxPointLights);
		for (uint32_t i = 0; i < maxPointLights; ++i) {
			VkImageViewCreateInfo viewInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.image = _impl->shadowAtlas.image.Handle(),
				.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
				.format = VK_FORMAT_D32_SFLOAT,
				.components = {},
				.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
									 .baseMipLevel = 0,
									 .levelCount = 1,
									 .baseArrayLayer = i * 6,
									 .layerCount = 6}};

			VkImageView rawView = VK_NULL_HANDLE;
			vkCreateImageView(_impl->ctx.Device(), &viewInfo, nullptr, &rawView);

			// Construct the RAII wrapper to manage this view's lifetime automatically
			_impl->punctualShadowViews[i] = Vk::ImageView(_impl->ctx.Device(), rawView);
		}

		_impl->needsInitialClear = true;
		_impl->resized = false;
	}

	ZHLN_AcquireDesc acq = {.swapchain = _impl->presentation.swapchain.Get().handle,
							.image_available = s.image_available,
							.timeout_ns = UINT64_MAX};

	auto acq_res = ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index);
	if (acq_res == ZHLN_FrameResult_OutOfDate || acq_res == ZHLN_FrameResult_Suboptimal) {
		_impl->resized = true;
		_impl->current_cmd = VK_NULL_HANDLE;
		return std::unexpected(MapFrameResult(acq_res));
	}
	if (acq_res == ZHLN_FrameResult_DeviceLost) {
		_impl->current_cmd = VK_NULL_HANDLE;
		return std::unexpected(RenderFrameResult::DeviceLost);
	}
	if (acq_res == ZHLN_FrameResult_Error) {
		_impl->current_cmd = VK_NULL_HANDLE;
		return std::unexpected(RenderFrameResult::Error);
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	if (_impl->needsInitialClear) {
		auto* cmd = _impl->current_cmd;

		// Tie relevant layout groups dynamically into zero-cost reference bundles
		auto mainBundle =
			Vk::TieTargets(_impl->sceneColor, _impl->velocityBuffer, _impl->accumBuffers[0],
						   _impl->accumBuffers[1], _impl->normalRoughnessBuffer);
		auto ppBundle =
			Vk::TieTargets(_impl->postProcessTarget, _impl->ambientTarget, _impl->lightingTarget,
						   _impl->smaaEdgeTarget, _impl->smaaWeightTarget);
		auto bloomBundle = Vk::TieTargets(_impl->bloomThresholdTarget, _impl->bloomBlurTarget,
										  _impl->bloomFinalTarget);

		// --- BATCH TRANSITION 1: Color Attachments ---
		// Transition UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
		auto mainAtts = mainBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
		auto ppAtts = ppBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
		auto bloomAtts = bloomBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);

		auto sShadowMap_att = Vk::Transition(cmd, _impl->shadowMap, Vk::AsDepthAttachment);
		auto sShadowAtlas_att = Vk::Transition(cmd, _impl->shadowAtlas, Vk::AsDepthAttachment);
		auto depth_att =
			Vk::Transition(cmd, _impl->presentation.depthTarget, Vk::AsDepthAttachment);

		// Pass 1: Clear G-Buffer and TAA history
		Vk::DynamicPass(_impl->presentation.swapchain.Get().extent)
			.AddColorGroup(mainAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						   {0, 0, 0, 0})
			.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearDepthValue)
			.Execute(cmd, []() {});

		// Pass 2: Clear Post-Process intermediate targets
		Vk::DynamicPass(_impl->presentation.swapchain.Get().extent)
			.AddColorGroup(ppAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						   kClearColorBlack)
			.Execute(cmd, []() {});

		// Pass 3: Clear Bloom targets (1/4 resolution)
		Vk::DynamicPass(_impl->bloomThresholdTarget.extent)
			.AddColorGroup(bloomAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						   kClearColorBlack)
			.Execute(cmd, []() {});

		// --- BATCH TRANSITION 2: Read Only ---
		// Transition from the active COLOR_ATTACHMENT_OPTIMAL layout tuples
		// (mainAtts, ppAtts, bloomAtts) instead of querying the un-tracked, static UNDEFINED
		// layouts from the base bundle references.
		[[maybe_unused]] auto mainRo = std::apply(
			[&](const auto&... atts) {
				return Vk::TransitionBatch<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts...);
			},
			mainAtts);

		[[maybe_unused]] auto ppRo = std::apply(
			[&](const auto&... atts) {
				return Vk::TransitionBatch<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts...);
			},
			ppAtts);

		[[maybe_unused]] auto bloomRo = std::apply(
			[&](const auto&... atts) {
				return Vk::TransitionBatch<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts...);
			},
			bloomAtts);

		[[maybe_unused]] auto sDepth_ro = Vk::Transition(cmd, depth_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sShadowMap_ro = Vk::Transition(cmd, sShadowMap_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sShadowAtlas_ro =
			Vk::Transition(cmd, sShadowAtlas_att, Vk::AsReadOnly);

		for (int i = 0; i < 2; ++i) {
			_impl->taaPass.WriteIndex(
				_impl->ctx.Device(), i,
				std::get<0>(mainRo),								// sceneColor
				i == 0 ? std::get<3>(mainRo) : std::get<2>(mainRo), // accumNext (accum1 : accum0)
				std::get<1>(mainRo),								// velocity
				_impl->defaultSampler.Get(), _impl->frameUniformBuffers[i].Handle());
		}

		_impl->needsInitialClear = false;
	}

	return {};
}

ZHLN_FrameResult RenderContext::Impl::SubmitFrame() {
	const ZHLN_FrameSync& s = sync[frame_index];
	ZHLN_FrameSubmitDesc submitDesc = {.graphicsQueue = ctx.GraphicsQueue(),
									   .presentQueue = ctx.PresentQueue(),
									   .cmd = current_cmd,
									   .imageAvailable = s.image_available,
									   .renderFinished =
										   presentation.presentSemaphores[current_image_index],
									   .inFlight = s.in_flight,
									   .swapchain = presentation.swapchain.Get().handle,
									   .imageIndex = current_image_index};

	ZHLN_FrameResult res = Vk::SubmitAndPresent(submitDesc);
	if (res != ZHLN_FrameResult_Ok) {
		resized = true;
	}

	auto manager = StaticResourceManager(
		&accumBuffers, &taaPass, &fxaaPass, &smaaEdgePass, &smaaWeightPass, &smaaBlendPass,
		&ambientPass, &lightingPass, &reflectionPass, &blitPass, &bloomThresholdPass,
		&bloomBlurHPass, &bloomBlurVPass, &frameUniformBuffers, &lightStorageBuffers,
		&instanceDataBuffers, &indirectCommandsBuffers, &jointBuffers, &bindlessSets, &tlas,
		&tlasBuffer, &tlasScratchBuffer, &clusterGridBuffers, &lightIndexListBuffers,
		&globalCounterBuffers, &clusterCullingSets);
	manager.FlipAll();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
	return res;
}

RenderResult RenderContext::EndFrame() noexcept {
	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE) {
		_impl->drawQueue.clear();
		_impl->uiDrawQueue.clear();
		return std::unexpected(RenderFrameResult::Error);
	}

	// Execute pre-render compute skinning cleanly and self-contained
	_impl->DispatchSkinningPasses();

	VkCommandBuffer cmd = _impl->current_cmd;

	if (_impl->drawQueue.size() > kGpuCullingMaxInstances) {
		ZHLN::Log("WARNING: Draw queue exceeded max instances ({} / {}). Truncating.",
				  _impl->drawQueue.size(), kGpuCullingMaxInstances);
		_impl->drawQueue.resize(kGpuCullingMaxInstances);
	}

	_impl->tlasCleanupBuffers[_impl->frame_index].clear();

	_impl->SortDrawQueue();

	auto drawCount = static_cast<uint32_t>(_impl->drawQueue.size());
	if (drawCount > 0) {
		auto mapped = _impl->instanceDataBuffers[_impl->frame_index].Map();
		auto* dst = static_cast<InstanceData*>(mapped.data);

		for (uint32_t i = 0; i < drawCount; ++i) {
			const auto& cmdData = _impl->drawQueue[i];

			auto* vboMesh =
				(cmdData.skinnedVertexBuffer != BufferHandle::Invalid)
					? _impl->meshPool.Resolve(static_cast<uint64_t>(cmdData.skinnedVertexBuffer))
					: std::bit_cast<NativeMesh*>(cmdData.mesh);

			if (vboMesh == nullptr) {
				continue;
			}

			dst[i] = InstanceData{
				.world = cmdData.transform,
				.prevWorld = cmdData.prevTransform,
				.vboAddress = vboMesh->vboAddress,
				.iboAddress = (cmdData.indexMesh != nullptr)
								  ? std::bit_cast<NativeMesh*>(cmdData.indexMesh)->vboAddress
								  : 0,
				.vertexCount = cmdData.mesh->vertexCount,
				.indexCount = cmdData.indexCount,
				.albedoIndex = cmdData.albedoIndex,
				.normalIndex = cmdData.normalIndex,
				.pbrIndex = cmdData.pbrIndex,
				.emissiveIndex = cmdData.emissiveIndex,
				.cullRadius = cmdData.cullRadius,
				.metallicFactor = cmdData.metallicFactor,
				.roughnessFactor = cmdData.roughnessFactor,
				.alphaCutoff = cmdData.alphaCutoff,
				.alphaMode = cmdData.alphaMode,
				.jointOffset = cmdData.jointOffset,
				.isSkinned = (cmdData.skinnedVertexBuffer == BufferHandle::Invalid &&
							  (cmdData.flags & DrawFlags::Skinned) != DrawFlags::None)
								 ? 1u
								 : 0u,
				.morphOffset = cmdData.morphOffset,
				.activeMorphCount = cmdData.activeMorphCount,
				._pad = {},
				.morphWeights = cmdData.morphWeights,
				.baseColorFactor = cmdData.baseColorFactor,
				.emissiveFactor = cmdData.emissiveFactor,
			};
		}
	}
	if (_impl->tlas.Current() != VK_NULL_HANDLE && _impl->rtCtx.Valid()) {
		_impl->rtCtx.DestroyAS(_impl->tlas.Current());
		_impl->tlas.Current() = VK_NULL_HANDLE;
		_impl->tlasBuffer.Current() = {};
	}
	_impl->tlasCleanupBuffers[_impl->frame_index].clear();

	std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
	tlasInstances.reserve(_impl->drawQueue.size());

	for (uint32_t i = 0; i < _impl->drawQueue.size(); ++i) {
		auto* mesh = std::bit_cast<NativeMesh*>(_impl->drawQueue[i].mesh);
		const auto& drawCmd = _impl->drawQueue[i];

		bool isSkinned = (drawCmd.flags & DrawFlags::Skinned) != DrawFlags::None;
		bool isExcluded = (drawCmd.flags & DrawFlags::ExcludeFromTLAS) != DrawFlags::None;

		// TODO(Evilpasture): Putting this here just to not forget that there is a weird issue with
		// the character when skipping animated meshes. Apparently it's impossible to skip them in
		// screenspace.
		if (mesh->blasAddress == 0 || isSkinned || isExcluded) {
			continue;
		}

		VkAccelerationStructureInstanceKHR inst{};
		const auto& t = _impl->drawQueue[i].transform;
		inst.transform.matrix[0][0] = t(0, 0);
		inst.transform.matrix[0][1] = t(0, 1);
		inst.transform.matrix[0][2] = t(0, 2);
		inst.transform.matrix[0][3] = t(0, 3); // Correct Translation X

		inst.transform.matrix[1][0] = t(1, 0);
		inst.transform.matrix[1][1] = t(1, 1);
		inst.transform.matrix[1][2] = t(1, 2);
		inst.transform.matrix[1][3] = t(1, 3); // Correct Translation Y

		inst.transform.matrix[2][0] = t(2, 0);
		inst.transform.matrix[2][1] = t(2, 1);
		inst.transform.matrix[2][2] = t(2, 2);
		inst.transform.matrix[2][3] = t(2, 3); // Correct Translation Z
		inst.instanceCustomIndex = i;
		inst.mask = 0xFF;
		inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		inst.accelerationStructureReference = mesh->blasAddress;
		tlasInstances.push_back(inst);
	}

	if (!tlasInstances.empty() && _impl->rtCtx.Valid()) {
		Vk::Buffer instanceBuf = Vk::Buffer::Create(
			_impl->allocator.Get(),
			tlasInstances.size() * sizeof(VkAccelerationStructureInstanceKHR),
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY);
		Vk::Buffer staging =
			Vk::Buffer::Create(_impl->allocator.Get(),
							   tlasInstances.size() * sizeof(VkAccelerationStructureInstanceKHR),
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		std::memcpy(staging.Map().data, tlasInstances.data(),
					tlasInstances.size() * sizeof(VkAccelerationStructureInstanceKHR));

		ZHLN_BufferCopyDesc copy = {.src = staging.Handle(),
									.dst = instanceBuf.Handle(),
									.size = tlasInstances.size() *
											sizeof(VkAccelerationStructureInstanceKHR),
									.src_offset = 0,
									.dst_offset = 0};
		ZHLN_CmdCopyBuffer(cmd, &copy);

		Vk::MemoryBarrier(
			cmd, {.src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT |
							   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
							   VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				  .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
								VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				  .dst_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				  .dst_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
								VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
								VK_ACCESS_2_SHADER_READ_BIT});

		ZHLN_TlasGeometryDesc geom = {
			.instance_data = Vk::GetBufferDeviceAddress(_impl->ctx.Device(), instanceBuf.Handle())};

		ZHLN_AccelerationStructureSizes sizes;
		_impl->rtCtx.GetTlasSizes(static_cast<uint32_t>(tlasInstances.size()), sizes);

		bool needRebuild = !_impl->tlasBuffer.Current().Valid() ||
						   _impl->tlasBuffer.Current().Size() < sizes.acceleration_structure_size;

		if (needRebuild) {
			if (_impl->tlas.Current() != VK_NULL_HANDLE) {
				_impl->rtCtx.DestroyAS(_impl->tlas.Current());
				_impl->tlas.Current() = VK_NULL_HANDLE;
			}
			_impl->tlasBuffer.Current() = Vk::Buffer::Create(
				_impl->allocator.Get(), sizes.acceleration_structure_size,
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VMA_MEMORY_USAGE_GPU_ONLY);
			_impl->tlas.Current() =
				_impl->rtCtx.CreateAS(_impl->tlasBuffer.Current().Handle(),
									  sizes.acceleration_structure_size, ZHLN_AS_TYPE_TOP_LEVEL);
		}

		bool needScratchRebuild =
			!_impl->tlasScratchBuffer.Current().Valid() ||
			_impl->tlasScratchBuffer.Current().Size() < sizes.build_scratch_size;

		if (needScratchRebuild) {
			_impl->tlasScratchBuffer.Current() = Vk::Buffer::Create(
				_impl->allocator.Get(), sizes.build_scratch_size,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				VMA_MEMORY_USAGE_GPU_ONLY);
		}

		_impl->rtCtx.CmdBuildTlas(
			cmd, geom, _impl->tlas.Current(),
			Vk::GetBufferDeviceAddress(_impl->ctx.Device(),
									   _impl->tlasScratchBuffer.Current().Handle()),
			static_cast<uint32_t>(tlasInstances.size()));

		Vk::MemoryBarrier(cmd,
						  {.src_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
						   .src_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
						   .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
						   .dst_access = VK_ACCESS_2_SHADER_READ_BIT});

		_impl->tlasCleanupBuffers[_impl->frame_index].push_back(std::move(staging));
		_impl->tlasCleanupBuffers[_impl->frame_index].push_back(std::move(instanceBuf));
	}

	FrameRecorder recorder(cmd, *_impl);

	{
		VkCommandBuffer c = cmd;
		uint32_t fIdx = _impl->frame_index;

		_impl->clusterBoundsPass.Dispatch(c, _impl->clusterCullingSets[fIdx], 1, 1, 24);

		Vk::FillBuffer(c, _impl->globalCounterBuffers[fIdx]);

		Vk::BarrierBuilder()
			.From(Vk::BarrierStage::Transfer | Vk::BarrierStage::Compute,
				  Vk::BarrierAccess::TransferWrite | Vk::BarrierAccess::ShaderWrite)
			.To(c, Vk::BarrierStage::Compute,
				Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite);

		_impl->clusterCullingPass.Dispatch(c, _impl->clusterCullingSets[fIdx], 1, 1, 24);

		Vk::BarrierBuilder()
			.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
			.To(c, Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead);
	}

	RunPass(Passes::ShadowPass{}, recorder);

	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		initialState = {.sceneColor = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->sceneColor),
						.velocity = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->velocityBuffer),
						.normRough = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->normalRoughnessBuffer),
						.depth = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)};

	// 1. Render the G-Buffer (Geometry Pass)
	Passes::MainPass{}.Execute(recorder, initialState);

	// 2. Compute Deferred Lighting
	auto pp_ro = Passes::DeferredLightingPass{}.Execute(recorder, initialState);

	// 3. Forward Pass
	Passes::ForwardPass{}.Execute(recorder, pp_ro, initialState.depth);

	// 4. Bloom Pass
	auto bloomFinal_ro = Passes::BloomPass{}.Execute(recorder, pp_ro);

	// 5. Run AA (TAA / SMAA / FXAA)
	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		resourcesForAA = {.sceneColor = pp_ro,
						  .velocity = initialState.velocity,
						  .normRough = initialState.normRough,
						  .depth = initialState.depth};

	auto aa_ro = Passes::AAPass{}.Execute(recorder, resourcesForAA);

	// 6. Blit and present
	Passes::BlitPass{}.Execute(recorder, aa_ro.sceneColor, bloomFinal_ro);

	ZHLN_EndCommandBuffer(cmd);

	auto res = _impl->SubmitFrame();
	if (res != ZHLN_FrameResult_Ok) {
		return std::unexpected(MapFrameResult(res));
	}

	_impl->drawQueue.clear();
	_impl->uiDrawQueue.clear();

	return {};
}

namespace Renderer {

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj,
				 const JPH::Mat44& unjitteredViewProj) {
	auto* impl = ctx.GetImpl();
	impl->current_view_proj = viewProj;
	impl->unjittered_view_proj = unjitteredViewProj;
}

void SetFrameData(RenderContext& ctx, const FrameUniforms& uniforms,
				  const JPH::Mat44& shadowProjView) {
	auto* impl = ctx.GetImpl();
	impl->shadowProjView = shadowProjView;
	impl->currentUniforms = uniforms;

	impl->currentUniforms.camPos[3] = static_cast<float>(impl->aaState.frameIndex);

	std::memcpy(impl->currentUniforms.sh, impl->iblPayload.shCoeffs.data(), sizeof(JPH::Vec4) * 9);

	float nZ = 24.0f;
	float nearZ = 0.1f;
	float farZ = 1000.0f;
	float logRatio = std::log(farZ / nearZ);
	impl->currentUniforms.zScale = nZ / logRatio;
	impl->currentUniforms.zBias = -(nZ * std::log(nearZ)) / logRatio;

	std::memcpy(impl->frameUniformBuffers[impl->frame_index].Map().data, &impl->currentUniforms,
				sizeof(FrameUniforms));
}

void SetGISettings(RenderContext& ctx, const GISettings& settings) {
	auto* impl = ctx.GetImpl();
	impl->giSettings = settings;
}

void SetLights(RenderContext& ctx, const GPULight* lights, uint32_t count) {
	auto* impl = ctx.GetImpl();
	uint32_t safeCount = ZHLN::Min(count, 128u);
	if (safeCount > 0 && lights != nullptr) {
		std::memcpy(impl->lightStorageBuffers[impl->frame_index].Map().data, lights,
					sizeof(GPULight) * safeCount);

		impl->mappedLights.assign(lights, lights + safeCount);
	} else {
		impl->mappedLights.clear();
	}
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const DrawParams& params) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	auto* nativeMesh = impl->meshPool.Resolve(static_cast<uint64_t>(mesh.vertexBuffer));
	if (nativeMesh == nullptr) [[unlikely]] {
		return;
	}

	auto* nativeIndexMesh = mesh.indexBuffer != BufferHandle::Invalid
								? impl->meshPool.Resolve(static_cast<uint64_t>(mesh.indexBuffer))
								: nullptr;

	auto* nativeMaterial = impl->materialPool.Resolve(static_cast<uint64_t>(material.pipeline));
	if (nativeMaterial == nullptr) [[unlikely]] {
		return;
	}

	impl->drawQueue.push_back(DrawCommand{
		.material = nativeMaterial,
		.mesh = nativeMesh,
		.indexMesh = nativeIndexMesh,
		.transform = params.transform,
		.prevTransform = params.prevTransform,
		.albedoIndex = material.albedoIndex,
		.normalIndex = material.normalIndex,
		.pbrIndex = material.pbrIndex,
		.emissiveIndex = material.emissiveIndex,
		.cullRadius = params.cullRadius,
		.metallicFactor = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
		.roughnessFactor = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
		.alphaCutoff = material.alphaCutoff,
		.alphaMode = material.alphaMode,
		.jointOffset = params.jointOffset,
		.baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1],
							material.baseColorFactor[2], material.baseColorFactor[3]},
		.emissiveFactor = {material.emissiveFactor[0], material.emissiveFactor[1],
						   material.emissiveFactor[2], material.emissiveFactor[3]},
		.morphOffset = params.morphOffset,
		.activeMorphCount = params.activeMorphCount,
		.morphWeights = {(params.morphWeights != nullptr) ? params.morphWeights[0] : 0.0f,
						 (params.morphWeights != nullptr) ? params.morphWeights[1] : 0.0f,
						 (params.morphWeights != nullptr) ? params.morphWeights[2] : 0.0f,
						 (params.morphWeights != nullptr) ? params.morphWeights[3] : 0.0f},
		.indexCount = mesh.indexCount,
		.skinnedVertexBuffer = params.skinnedVertexBuffer,
		.flags = params.flags,
	});
}

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	if (mesh.vertexBuffer == BufferHandle::Invalid) {
		return;
	}

	auto* nativeMesh = impl->meshPool.Resolve(static_cast<uint64_t>(mesh.vertexBuffer));
	if (nativeMesh == nullptr) [[unlikely]] {
		return;
	}

	impl->uiDrawQueue.push_back({.mesh = nativeMesh, .fontIndex = fontIndex});
}

} // namespace Renderer
} // namespace ZHLN
