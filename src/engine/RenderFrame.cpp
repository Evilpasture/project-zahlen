// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// --- src/engine/RenderFrame.cpp ---

#include "ParallelDraw.hpp"
#include "RenderInternal.hpp"
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

// --- Graph Helper ---
template <VkImageLayout L, VkFormat F>
Vk::TypedImage<L> AssumeLayout(const Vk::RenderTarget<F>& rt,
							   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
	return {rt.image.Handle(), rt.view.Get(), rt.extent, aspect};
}

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
		Vk::CmdBarrierComputeToIndirect(cmd);

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
						if (!drawCmd.material->pipeline.Valid()) {
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

		// Compile-time state translation
		auto shadow_att = IssueBarrier<Vk::UndefinedState, Vk::DepthAttachmentState>(
			cmd, ctx.shadowMap, VK_IMAGE_ASPECT_DEPTH_BIT);

		Vk::DynamicPass({.width = ZHLN::RenderContext::Impl::SHADOW_RES,
						 .height = ZHLN::RenderContext::Impl::SHADOW_RES})
			.AddDepth(shadow_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
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
									  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				}
			});

		[[maybe_unused]] auto end = IssueBarrier<Vk::DepthAttachmentState, Vk::ShaderReadState>(
			cmd, shadow_att, VK_IMAGE_ASPECT_DEPTH_BIT);
	}
};

struct MainPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> in) const noexcept
		-> SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																		  ctx.gpuProfiler);

		// Compile-time state translation
		auto color_att =
			IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, in.sceneColor);
		auto vel_att =
			IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, in.velocity);
		auto norm_att =
			IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, in.normRough);
		auto depth_att = IssueBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
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

			return {.sceneColor = color_att,
					.velocity = vel_att,
					.normRough = norm_att,
					.depth = depth_att};
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

		return {.sceneColor = color_att,
				.velocity = vel_att,
				.normRough = norm_att,
				.depth = depth_att};
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

		// Resources are already transitioned to ShaderReadState by preceding passes
		auto color_ro = in.sceneColor;
		auto vel_ro = in.velocity;
		auto norm_ro = in.normRough;
		auto depth_ro = in.depth;

		if (ctx.aaState.mode == AAMode::TAA && ctx.taaPass.pipeline.Valid()) {
			auto accumCurr_ro =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Current());
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

			auto accumNext_att =
				IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, accumNext_u);

			struct TAAPushConstants {
				float feedback;
			};

			Vk::DynamicPass(in.sceneColor.extent)
				.AddColor(accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() {
					ctx.taaPass.WriteNext(
						ctx.ctx.Device(), color_ro, accumCurr_ro, vel_ro,
						Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()},
						Vk::BufferWrite{.buffer =
											ctx.frameUniformBuffers[recorder.frameIndex].Handle()});

					ctx.taaPass.Execute(cmd, TAAPushConstants{.feedback = ctx.aaState.taaFeedback});
				});

			// Promote output into the downstream read chain
			color_ro =
				IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, accumNext_att);

		} else if (ctx.aaState.mode == AAMode::FXAA && ctx.fxaaPass.pipeline.Valid()) {
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());
			auto accumNext_att =
				IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, accumNext_u);

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
										   Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()});
					ctx.fxaaPass.Execute(cmd, pc);
				});

			color_ro =
				IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, accumNext_att);

		} else if (ctx.aaState.mode == AAMode::SMAA && ctx.smaaEdgePass.pipeline.Valid()) {

			// Capture texture metrics push constants
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
			auto smaaEdge_att =
				IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, smaaEdge_u);

			Vk::DynamicPass(in.sceneColor.extent)
				.AddColor(smaaEdge_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						  kClearColorBlack)
				.Execute(cmd, [&]() {
					ctx.smaaEdgePass.WriteNext(
						ctx.ctx.Device(), color_ro,
						Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()});
					ctx.smaaEdgePass.Execute(
						cmd, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				});

			auto smaaEdge_ro =
				IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, smaaEdge_att);

			// --- PASS 2: BLENDING WEIGHT CALCULATION ---
			auto smaaWeight_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.smaaWeightTarget);
			auto smaaWeight_att =
				IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, smaaWeight_u);

			// Bind edge-detection output along with precomputed LUT views
			VkImageView areaView = ctx.textureViews[ctx.smaaAreaTexIdx].Get();
			VkImageView searchView = ctx.textureViews[ctx.smaaSearchTexIdx].Get();

			Vk::DynamicPass(in.sceneColor.extent)
				.AddColor(smaaWeight_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
						  kClearColorBlack)
				.Execute(cmd, [&]() {
					ctx.smaaWeightPass.WriteNext(
						ctx.ctx.Device(), smaaEdge_ro,
						Vk::ImageWrite{.view = areaView,
									   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
						Vk::ImageWrite{.view = searchView,
									   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
						Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()}, // Slot 3: Linear
						Vk::SamplerWrite{.sampler = ctx.pointSampler.Get()}
						// Slot 4: Point / Nearest

					);
					ctx.smaaWeightPass.Execute(
						cmd, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				});

			auto smaaWeight_ro =
				IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, smaaWeight_att);

			// --- PASS 3: NEIGHBORHOOD BLENDING ---
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());
			auto accumNext_att =
				IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, accumNext_u);

			Vk::DynamicPass(in.sceneColor.extent)
				.AddColor(accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() {
					ctx.smaaBlendPass.WriteNext(
						ctx.ctx.Device(), color_ro, smaaWeight_ro,
						Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()});
					ctx.smaaBlendPass.Execute(
						cmd, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				});

			// Feed output into final presentation pipeline
			color_ro =
				IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, accumNext_att);
		}

		return {
			.sceneColor = color_ro, .velocity = vel_ro, .normRough = norm_ro, .depth = depth_ro};
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

		Profiler::ScopedGpuProfile<Stages::PostProcessPass, FrameProfiler> timer(
			cmd, recorder.frameIndex, ctx.gpuProfiler);

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
		auto ambient_u = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.ambientTarget);
		auto ambient_att =
			IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, ambient_u);

		ctx.ambientPass.WriteNext(
			ctx.ctx.Device(), in.sceneColor, Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()},
			in.depth, in.normRough, Vk::SamplerWrite{.sampler = ctx.pointSampler.Get()},
			Vk::ImageWrite{.view = ctx.iblPayload.prefilteredView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::ImageWrite{.view = ctx.iblPayload.brdfLutView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::SamplerWrite{.sampler = ctx.clampSampler.Get()},
			Vk::BufferWrite{.buffer = ctx.frameUniformBuffers[recorder.frameIndex].Handle()});

		Vk::DynamicPass(ctx.ambientTarget.extent)
			.AddColor(ambient_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() { ctx.ambientPass.Execute(cmd, pc); });

		auto ambient_ro =
			IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, ambient_att);

		// --- 2. LIGHTING PASS ---
		auto light_u = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.lightingTarget);
		auto light_att = IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, light_u);

		ctx.lightingPass.WriteNext(
			ctx.ctx.Device(), in.sceneColor, Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()},
			in.depth, in.normRough,
			Vk::BufferWrite{.buffer = ctx.lightStorageBuffers[recorder.frameIndex].Handle()},
			Vk::BufferWrite{.buffer = ctx.frameUniformBuffers[recorder.frameIndex].Handle()},
			Vk::ImageWrite{.view = ctx.shadowMap.view.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::SamplerWrite{.sampler = ctx.shadowSampler.Get()},
			Vk::ImageWrite{.view = ctx.ltcMatView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::ImageWrite{.view = ctx.ltcAmpView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::SamplerWrite{.sampler = ctx.clampSampler.Get()},
			Vk::BufferWrite{.buffer = ctx.clusterGridBuffers[recorder.frameIndex].Handle()},
			Vk::BufferWrite{.buffer = ctx.lightIndexListBuffers[recorder.frameIndex].Handle()},
			Vk::ImageWrite{.view = ambient_ro.view,
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::SamplerWrite{.sampler = ctx.pointSampler.Get()});

		Vk::DynamicPass(ctx.lightingTarget.extent)
			.AddColor(light_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() { ctx.lightingPass.Execute(cmd, pc); });

		auto light_ro = IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, light_att);

		// --- 3. REFLECTION PASS ---
		auto pp_u = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.postProcessTarget);
		auto pp_att = IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, pp_u);

		if (ctx.rtCtx.Valid()) {
			ctx.reflectionPass.WriteNext(
				ctx.ctx.Device(), in.sceneColor,
				Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()}, in.depth, in.normRough,
				Vk::SamplerWrite{.sampler = ctx.pointSampler.Get()},
				Vk::ImageWrite{.view = ctx.iblPayload.prefilteredView.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				&ctx.tlas.Current(),
				Vk::BufferWrite{.buffer = ctx.frameUniformBuffers[recorder.frameIndex].Handle()},
				Vk::ImageWrite{.view = ctx.iblPayload.brdfLutView.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::SamplerWrite{.sampler = ctx.clampSampler.Get()},
				Vk::ImageWrite{.view = light_ro.view,
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
		} else {
			ctx.reflectionPass.WriteNext(
				ctx.ctx.Device(), in.sceneColor,
				Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()}, in.depth, in.normRough,
				Vk::SamplerWrite{.sampler = ctx.pointSampler.Get()},
				Vk::ImageWrite{.view = ctx.iblPayload.prefilteredView.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::SkipWrite{}, // Slot 6: tlas (skipped)
				Vk::BufferWrite{.buffer = ctx.frameUniformBuffers[recorder.frameIndex].Handle()},
				Vk::ImageWrite{.view = ctx.iblPayload.brdfLutView.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::SamplerWrite{.sampler = ctx.clampSampler.Get()},
				Vk::ImageWrite{.view = light_ro.view,
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
		}

		Vk::DynamicPass(ctx.postProcessTarget.extent)
			.AddColor(pp_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				uint32_t variantIdx =
					(pc.enableSSR ? 1 : 0) | ((pc.enableRTR && ctx.rtCtx.Valid()) ? 2 : 0);
				ctx.reflectionPass.ExecuteVariant(cmd, variantIdx, pc);
			});

		return IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, pp_att);
	}
};

struct ForwardPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> litColor,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth) const noexcept
		-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {

		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		auto color_att = IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, litColor);
		auto depth_att = IssueBarrier<Vk::ShaderReadState, Vk::DepthAttachmentState>(
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

		IssueBarrier<Vk::DepthAttachmentState, Vk::ShaderReadState>(cmd, depth_att,
																	VK_IMAGE_ASPECT_DEPTH_BIT);
		return IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, color_att);
	}
};

struct BlitPass {
	void Execute(const FrameRecorder& recorder,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor) const noexcept {
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

		auto swap_att = IssueBarrier<Vk::UndefinedState, Vk::ColorAttachmentState>(cmd, swap_u);

		ctx.blitPass.WriteNext(ctx.ctx.Device(), inColor,
							   Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()});

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

		// Compile-time state translation
		[[maybe_unused]] auto end =
			IssueBarrier<Vk::ColorAttachmentState, Vk::PresentState>(cmd, swap_att);
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
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skinningPass.pipeline.Get());

	for (const auto& drawCmd : drawQueue) {
		if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
			auto* inMesh = std::bit_cast<NativeMesh*>(drawCmd.mesh);
			auto* outMesh = meshPool.Resolve(static_cast<uint64_t>(drawCmd.skinnedVertexBuffer));

			if (inMesh == nullptr || outMesh == nullptr) {
				continue;
			}

			SkinningConstants pcs{};
			pcs.inputVerticesAddr = inMesh->vboAddress;
			pcs.outputVerticesAddr = outMesh->vboAddress;
			pcs.jointsAddr =
				Vk::GetBufferDeviceAddress(ctx.Device(), jointBuffers[frame_index].Handle());
			pcs.vertexCount = inMesh->vertexCount;
			pcs.jointOffset = drawCmd.jointOffset;

			vkCmdPushConstants(cmd, skinningPass.pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT,
							   0, sizeof(SkinningConstants), &pcs);

			uint32_t groupCount = (inMesh->vertexCount + 63) / 64;
			vkCmdDispatch(cmd, groupCount, 1, 1);
		}
	}

	// Memory barrier to synchronize Compute Writes -> Vertex Shader Reads
	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
							.src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
							.dst_stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
							.dst_access = VK_ACCESS_2_SHADER_READ_BIT});
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
			return std::unexpected(RenderFrameResult::OutOfDate); // <-- FIXED: Monadic return
		}

		VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

		if (!_impl->presentation.Rebuild(ext.width, ext.height)) {
			return std::unexpected(RenderFrameResult::Error); // <-- FIXED: Monadic return
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

		_impl->smaaEdgeTarget = Vk::RenderTarget<VK_FORMAT_R8G8_UNORM>::Create(
			_impl->allocator, _impl->ctx, ext,
			{.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
		_impl->smaaWeightTarget = Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>::Create(
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
		auto sColor_att = Vk::Transition(cmd, _impl->sceneColor, Vk::AsColorAttachment);
		auto sVel_att = Vk::Transition(cmd, _impl->velocityBuffer, Vk::AsColorAttachment);
		auto sAcc0_att = Vk::Transition(cmd, _impl->accumBuffers[0], Vk::AsColorAttachment);
		auto sAcc1_att = Vk::Transition(cmd, _impl->accumBuffers[1], Vk::AsColorAttachment);
		auto sNorm_att = Vk::Transition(cmd, _impl->normalRoughnessBuffer, Vk::AsColorAttachment);
		auto depth_att =
			Vk::Transition(cmd, _impl->presentation.depthTarget, Vk::AsDepthAttachment);
		auto sPost_att = Vk::Transition(cmd, _impl->postProcessTarget, Vk::AsColorAttachment);
		auto sAmb_att = Vk::Transition(cmd, _impl->ambientTarget, Vk::AsColorAttachment);
		auto sLight_att = Vk::Transition(cmd, _impl->lightingTarget, Vk::AsColorAttachment);
		auto sEdge_att = Vk::Transition(cmd, _impl->smaaEdgeTarget, Vk::AsColorAttachment);
		auto sWeight_att = Vk::Transition(cmd, _impl->smaaWeightTarget, Vk::AsColorAttachment);

		// Pass 1: Clear G-Buffer and TAA history
		Vk::DynamicPass(_impl->presentation.swapchain.Get().extent)
			.AddColor(sColor_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sVel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorVelocity)
			.AddColor(sAcc0_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sAcc1_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sNorm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorNormalRoughness)
			.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearDepthValue)
			.Execute(cmd, []() {});

		// Pass 2: Clear Post-Process intermediate targets
		Vk::DynamicPass(_impl->presentation.swapchain.Get().extent)
			.AddColor(sPost_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sAmb_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sLight_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sEdge_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.AddColor(sWeight_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  kClearColorBlack)
			.Execute(cmd, []() {});

		auto sColor_ro = Vk::Transition(cmd, sColor_att, Vk::AsReadOnly);
		auto sVel_ro = Vk::Transition(cmd, sVel_att, Vk::AsReadOnly);
		auto sAcc0_ro = Vk::Transition(cmd, sAcc0_att, Vk::AsReadOnly);
		auto sAcc1_ro = Vk::Transition(cmd, sAcc1_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sNorm_ro = Vk::Transition(cmd, sNorm_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sDepth_ro = Vk::Transition(cmd, depth_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sPost_ro = Vk::Transition(cmd, sPost_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sAmb_ro = Vk::Transition(cmd, sAmb_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sLight_ro = Vk::Transition(cmd, sLight_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sEdge_ro = Vk::Transition(cmd, sEdge_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sWeight_ro = Vk::Transition(cmd, sWeight_att, Vk::AsReadOnly);

		for (int i = 0; i < 2; ++i) {
			_impl->taaPass.WriteIndex(
				_impl->ctx.Device(), i, sColor_ro, i == 0 ? sAcc1_ro : sAcc0_ro, sVel_ro,
				Vk::SamplerWrite{.sampler = _impl->defaultSampler.Get()},
				Vk::BufferWrite{.buffer = _impl->frameUniformBuffers[i].Handle()});
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
		&ambientPass, &lightingPass, &reflectionPass, &blitPass, &frameUniformBuffers,
		&lightStorageBuffers, &instanceDataBuffers, &indirectCommandsBuffers, &jointBuffers,
		&bindlessSets, &tlas, &tlasBuffer, &tlasScratchBuffer, &clusterGridBuffers,
		&lightIndexListBuffers, &globalCounterBuffers, &clusterCullingSets);
	manager.FlipAll();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
	return res; // <-- FIXED: Return the actual result instead of empty list {}
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

			// --- CORRECTED TO SAFELY RESOLVE THE HANDLE ---
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
				.vboAddress = vboMesh->vboAddress, // BDA now safely points to the scratch output
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

		// TODO(Evilpasture): This still pulls animated screen space meshes. There is something
		// wrong with the GLB we're using. What do we do? Nothing. Too much work.
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

	// 1. Inside RenderContext::EndFrame()'s TLAS build block:
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

		// FIX: Comprehensive Sync Barrier for double-buffered AS/Scratch re-use AND Buffer Copy
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

		// Build or resize the TLAS buffer only if needed
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

		// --- NEW: Persistent Scratch Buffer Re-use ---
		// Reallocate/resize the scratch buffer only when needed
		bool needScratchRebuild =
			!_impl->tlasScratchBuffer.Current().Valid() ||
			_impl->tlasScratchBuffer.Current().Size() < sizes.build_scratch_size;

		if (needScratchRebuild) {
			_impl->tlasScratchBuffer.Current() = Vk::Buffer::Create(
				_impl->allocator.Get(), sizes.build_scratch_size,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				VMA_MEMORY_USAGE_GPU_ONLY);
		}

		// Execute TLAS build with zero runtime allocations
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
		// DO NOT push tlasScratchBuffer to tlasCleanupBuffers (we keep it alive!)
	}

	// Encapsulate recording state
	FrameRecorder recorder(cmd, *_impl);

	{
		VkCommandBuffer c = cmd;
		uint32_t fIdx = _impl->frame_index;

		// Recalculate cluster bounds every frame to match the moving camera frustum
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE,
						  _impl->clusterBoundsPass.pipeline.Get());
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
								_impl->clusterBoundsPass.pipelineLayout.Get(), 0, 1,
								&_impl->clusterCullingSets[fIdx], 0, nullptr);
		vkCmdDispatch(c, 1, 1, 24);

		vkCmdFillBuffer(c, _impl->globalCounterBuffers[fIdx].Handle(), 0, sizeof(uint32_t), 0);

		Vk::MemoryBarrier(
			c,
			{.src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			 .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
			 .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			 .dst_access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT});

		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE,
						  _impl->clusterCullingPass.pipeline.Get());
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
								_impl->clusterCullingPass.pipelineLayout.Get(), 0, 1,
								&_impl->clusterCullingSets[fIdx], 0, nullptr);
		vkCmdDispatch(c, 1, 1, 24);

		Vk::MemoryBarrier(c, {.src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
							  .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
							  .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
							  .dst_access = VK_ACCESS_2_SHADER_READ_BIT});
	}

	// Concepts-constrained pass execution
	RunPass(Passes::ShadowPass{}, recorder);

	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		initialState = {
			.sceneColor = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->sceneColor),
			.velocity =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->velocityBuffer),
			.normRough = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				_impl->normalRoughnessBuffer),
			.depth = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				_impl->presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)};

	// 1. Render the G-Buffer (Geometry Pass)
	auto stateAfterMain = Passes::MainPass{}.Execute(recorder, initialState);

	// 2. Transition G-Buffer outputs to Shader Read layouts before computing lighting
	auto color_ro =
		IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, stateAfterMain.sceneColor);
	auto vel_ro =
		IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, stateAfterMain.velocity);
	auto norm_ro =
		IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, stateAfterMain.normRough);
	auto depth_ro = IssueBarrier<Vk::DepthAttachmentState, Vk::ShaderReadState>(
		cmd, stateAfterMain.depth, VK_IMAGE_ASPECT_DEPTH_BIT);

	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		stateAfterMain_ro = {
			.sceneColor = color_ro, .velocity = vel_ro, .normRough = norm_ro, .depth = depth_ro};

	// 3. Compute Deferred Lighting
	auto stateAfterPP = Passes::DeferredLightingPass{}.Execute(recorder, stateAfterMain_ro);

	// 3.5. Forward Pass (Transparent geometry mapped ON TOP of Deferred Lighting)
	auto stateAfterForward =
		Passes::ForwardPass{}.Execute(recorder, stateAfterPP, stateAfterMain_ro.depth);

	// 4. Assemble the TAA inputs
	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		resourcesForAA = {.sceneColor = stateAfterForward, // Output of Forward Pass!
						  .velocity = stateAfterMain_ro.velocity,
						  .normRough = stateAfterMain_ro.normRough,
						  .depth = stateAfterMain_ro.depth};

	// 5. Run TAA to resolve all high-frequency dithering and geometry aliasing
	auto stateAfterAA = Passes::AAPass{}.Execute(recorder, resourcesForAA);

	// 6. Blit and present the clean, smoothed image
	Passes::BlitPass{}.Execute(recorder, stateAfterAA.sceneColor);

	ZHLN_EndCommandBuffer(cmd);
	// SubmitFrame now returns ZHLN_FrameResult
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

	// Resolve Mesh Handle with Generational Safety checks
	auto* nativeMesh = impl->meshPool.Resolve(static_cast<uint64_t>(mesh.vertexBuffer));
	if (nativeMesh == nullptr) [[unlikely]] {
		return;
	}

	impl->uiDrawQueue.push_back({.mesh = nativeMesh, .fontIndex = fontIndex});
}

} // namespace Renderer

} // namespace ZHLN
