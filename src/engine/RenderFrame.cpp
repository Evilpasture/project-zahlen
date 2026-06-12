// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// --- src/engine/RenderFrame.cpp ---

#include "ParallelDraw.hpp"
#include "RenderInternal.hpp"
#include "Zahlen/GUI.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "detail/RadixSort.hpp"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <bit>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

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
	NativeMesh* mesh;
	NativeMesh* indexMesh;
	uint32_t indexCount;
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
		};
		FrustumPlanes planes{};

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
				const VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
				for (const auto& group : groups) {
					if (!group.material->pipeline.Valid()) {
						continue;
					}

					Vk::DrawIndirect(
						cmd,
						{.pipeline = group.material->pipeline.Get(),
						 .layout = group.material->layout.Get(),
						 .set = recorder.bindlessSet,
						 .vbo = group.mesh->buffer.Handle(),
						 .ibo = group.indexMesh ? group.indexMesh->buffer.Handle() : VK_NULL_HANDLE,
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
	Record(const FrameRecorder& recorder, const JPH::Array<GroupRange>& groups, uint32_t drawCount,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> color_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> vel_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> norm_att,
		   Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att) noexcept {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		std::array<VkFormat, 3> colorFormats = {
			VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};

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
							 .vbo = std::bit_cast<NativeMesh*>(drawCmd.mesh)->buffer.Handle(),
							 .ibo = drawCmd.indexMesh
										? std::bit_cast<NativeMesh*>(drawCmd.indexMesh)
											  ->buffer.Handle()
										: VK_NULL_HANDLE,
							 .vertexCount = std::bit_cast<NativeMesh*>(drawCmd.mesh)->vertexCount,
							 .indexCount = drawCmd.indexCount},
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

// ============================================================================
// Render Passes
// ============================================================================

namespace Passes {

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
					auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh);

					ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 1};

					Vk::DrawInstanced(
						cmd,
						{.pipeline = ctx.shadowPipeline.Get(),
						 .layout = ctx.shadowPipelineLayout.Get(),
						 .set = recorder.bindlessSet,
						 .vbo = mesh->buffer.Handle(),
						 .ibo = draw.indexMesh
									? std::bit_cast<NativeMesh*>(draw.indexMesh)->buffer.Handle()
									: VK_NULL_HANDLE,
						 .vertexCount = mesh->vertexCount,
						 .indexCount = draw.indexCount,
						 .instanceCount = 1},
						pushConstants, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
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
		NativeMesh* currentMesh = nullptr;
		NativeMesh* currentIndexMesh = nullptr;

		for (uint32_t i = 0; i < drawCount; ++i) {
			const auto& drawCmd = ctx.drawQueue[i];

			auto* drawMat = std::bit_cast<NativeMaterial*>(drawCmd.material);
			auto* drawMesh = std::bit_cast<NativeMesh*>(drawCmd.mesh);
			auto* drawIndexMesh = std::bit_cast<NativeMesh*>(drawCmd.indexMesh);

			if (i == 0 || drawMat != currentMaterial || drawMesh != currentMesh ||
				drawIndexMesh != currentIndexMesh) {
				groups.push_back(GroupRange{.material = drawMat,
											.mesh = drawMesh,
											.indexMesh = drawIndexMesh,
											.indexCount = drawCmd.indexCount,
											.start = i,
											.count = 1});
				currentMaterial = drawMat;
				currentMesh = drawMesh;
				currentIndexMesh = drawIndexMesh;
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

struct TAAPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> in) const noexcept
		-> SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		Profiler::ScopedGpuProfile<Stages::TaaPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																		 ctx.gpuProfiler);

		// Compile-time state translation
		auto color_ro =
			IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, in.sceneColor);
		auto vel_ro = IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, in.velocity);
		auto norm_ro =
			IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, in.normRough);
		auto depth_ro = IssueBarrier<Vk::DepthAttachmentState, Vk::ShaderReadState>(
			cmd, in.depth, VK_IMAGE_ASPECT_DEPTH_BIT);

		if (ctx.taaState.enabled && ctx.taaPass.pipeline.Valid()) {
			auto accumCurr_ro =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Current());
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());

			// Compile-time state translation
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

					ctx.taaPass.Execute(cmd, TAAPushConstants{.feedback = ctx.taaState.feedback});
				});

			[[maybe_unused]] auto end =
				IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, accumNext_att);
		}

		return {
			.sceneColor = color_ro, .velocity = vel_ro, .normRough = norm_ro, .depth = depth_ro};
	}
};

struct BlitPass {
	void Execute(const FrameRecorder& recorder,
				 SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
					 in) const noexcept {
		VkCommandBuffer cmd = recorder.cmd;
		auto& ctx = recorder.ctx;

		Profiler::ScopedGpuProfile<Stages::BlitPass, FrameProfiler> timer(cmd, recorder.frameIndex,
																		  ctx.gpuProfiler);

		uint32_t imageIdx = ctx.current_image_index;
		VkImage swapImg = ctx.presentation.swapchain.Get().images[imageIdx];
		VkImageView swapView = ctx.presentation.swapchain.Get().views[imageIdx];

		Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_u = {.handle = swapImg,
															.view = swapView,
															.extent = in.sceneColor.extent,
															.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

		// Compile-time state translation
		auto swap_att = IssueBarrier<Vk::UndefinedState, Vk::ColorAttachmentState>(cmd, swap_u);

		bool useTAA = ctx.taaState.enabled && ctx.taaPass.pipeline.Valid();

		Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> blitSource_ro = {
			.handle = useTAA ? ctx.accumBuffers.Next().image.Handle() : in.sceneColor.handle,
			.view = useTAA ? ctx.accumBuffers.Next().view.Get() : in.sceneColor.view,
			.extent = in.sceneColor.extent,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

		ctx.blitPass.WriteNext(ctx.ctx.Device(), blitSource_ro,
							   Vk::SamplerWrite{.sampler = ctx.defaultSampler.Get()}, in.depth,
							   in.normRough, Vk::SamplerWrite{.sampler = ctx.pointSampler.Get()},
							   Vk::ImageWrite{.view = ctx.iblPayload.prefilteredView.Get(),
											  .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

		struct BlitPushConstants {
			JPH::Mat44 invViewProj;
			JPH::Mat44 viewProj;
			alignas(
				16) std::array<float, 4> camPos; // camPos.w now contains the Temporal frameIndex!
			int giMode;
			float aoRadius;
			float aoBias;
			float aoPower;
			float giIntensity;
			int giSamples;
			float vignetteIntensity;
			float vignettePower;
			int enableSSR;
		} pc = {.invViewProj = ctx.current_view_proj.Inversed(), // Use jittered inverse matrix
				.viewProj = ctx.current_view_proj, // Use active jittered view-projection matrix
				.camPos = {ctx.currentUniforms.camPos[0], ctx.currentUniforms.camPos[1],
						   ctx.currentUniforms.camPos[2], ctx.currentUniforms.camPos[3]},
				.giMode = ctx.giSettings.mode,
				.aoRadius = ctx.giSettings.aoRadius,
				.aoBias = ctx.giSettings.aoBias,
				.aoPower = ctx.giSettings.aoPower,
				.giIntensity = ctx.giSettings.giIntensity,
				.giSamples = ctx.giSettings.giSamples,
				.vignetteIntensity = ctx.giSettings.vignetteIntensity,
				.vignettePower = ctx.giSettings.vignettePower,
				.enableSSR = ctx.giSettings.enableSSR};

		if (ctx.blitPass.pipeline.Valid()) {
			Vk::DynamicPass(in.sceneColor.extent)
				.AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
				.Execute(cmd, [&]() {
					ctx.blitPass.Execute(cmd, pc);

					if (!ctx.uiDrawQueue.empty()) {
						struct UIObjectConstants {
							JPH::Mat44 orthoMatrix;
							JPH::Mat44 unused;
							uint32_t albedoIdx;
						} uipc{};

						uipc.orthoMatrix = GUI::CreateOrthoMatrix(
							(float)in.sceneColor.extent.width, (float)in.sceneColor.extent.height);

						for (const auto& draw : ctx.uiDrawQueue) {
							uipc.albedoIdx = draw.fontIndex;

							Vk::DrawInstanced(
								cmd,
								{.pipeline = ctx.uiPipeline.Get(),
								 .layout = ctx.uiPipelineLayout.Get(),
								 .set = recorder.bindlessSet,
								 .vbo = std::bit_cast<NativeMesh*>(draw.mesh)->buffer.Handle(),
								 .vertexCount = std::bit_cast<NativeMesh*>(draw.mesh)->vertexCount},
								uipc);
						}
						ctx.uiDrawQueue.clear();
					}

					ImGui::Render();
					ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
				});
		}

		// Compile-time state translation
		[[maybe_unused]] auto end =
			IssueBarrier<Vk::ColorAttachmentState, Vk::PresentState>(cmd, swap_att);
	}
};

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

void RenderContext::BeginFrame() {
	if (_impl->stagingContext) {
		_impl->stagingContext->Wait();
		_impl->stagingContext.reset(); // Wait, cleanup, and destroy the fence automatically
	}

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, _impl->pools[_impl->frame_index]);

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
			return;
		}

		VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

		if (!_impl->presentation.Rebuild(ext.width, ext.height)) {
			return;
		}

		_impl->sceneColor = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
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
		_impl->normalRoughnessBuffer = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
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
		return;
	}
	if (acq_res == ZHLN_FrameResult_Error) {
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
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

		auto sColor_ro = Vk::Transition(cmd, sColor_att, Vk::AsReadOnly);
		auto sVel_ro = Vk::Transition(cmd, sVel_att, Vk::AsReadOnly);
		auto sAcc0_ro = Vk::Transition(cmd, sAcc0_att, Vk::AsReadOnly);
		auto sAcc1_ro = Vk::Transition(cmd, sAcc1_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sNorm_ro = Vk::Transition(cmd, sNorm_att, Vk::AsReadOnly);
		[[maybe_unused]] auto sDepth_ro = Vk::Transition(cmd, depth_att, Vk::AsReadOnly);

		for (int i = 0; i < 2; ++i) {
			_impl->taaPass.WriteIndex(
				_impl->ctx.Device(), i, sColor_ro, i == 0 ? sAcc1_ro : sAcc0_ro, sVel_ro,
				Vk::SamplerWrite{.sampler = _impl->defaultSampler.Get()},
				Vk::BufferWrite{.buffer = _impl->frameUniformBuffers[i].Handle()});
		}

		_impl->needsInitialClear = false;
	}
}

void RenderContext::Impl::SubmitFrame() {
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

	if (Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok) {
		resized = true;
	}

	accumBuffers.Flip();
	taaPass.sets.Flip();
	blitPass.sets.Flip();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
}

void RenderContext::EndFrame() {
	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE) {
		_impl->drawQueue.clear();
		_impl->uiDrawQueue.clear();
		return;
	}

	VkCommandBuffer cmd = _impl->current_cmd;

	if (_impl->drawQueue.size() > kGpuCullingMaxInstances) {
		ZHLN::Log("WARNING: Draw queue exceeded max instances ({} / {}). Truncating.",
				  _impl->drawQueue.size(), kGpuCullingMaxInstances);
		_impl->drawQueue.resize(kGpuCullingMaxInstances);
	}

	_impl->SortDrawQueue();

	{
		auto mapRegion = _impl->instanceDataBuffers[_impl->frame_index].Map();
		auto* mapped = reinterpret_cast<InstanceData*>(mapRegion.data);
		auto drawCount = static_cast<uint32_t>(_impl->drawQueue.size());

		for (uint32_t i = 0; i < drawCount; ++i) {
			const auto& drawCmd = _impl->drawQueue[i];

			mapped[i] = InstanceData{
				.world = drawCmd.transform,
				.prevWorld = drawCmd.prevTransform,
				.albedoIndex = drawCmd.albedoIndex,
				.normalIndex = drawCmd.normalIndex,
				.pbrIndex = drawCmd.pbrIndex,
				.emissiveIndex = drawCmd.emissiveIndex,
				.vertexCount = drawCmd.mesh->vertexCount,
				.cullRadius = drawCmd.cullRadius,
				.metallicFactor = drawCmd.metallicFactor,
				.roughnessFactor = drawCmd.roughnessFactor,
				.alphaCutoff = drawCmd.alphaCutoff,
				.alphaMode = drawCmd.alphaMode,
				.jointOffset = drawCmd.jointOffset,
				.isSkinned = drawCmd.isSkinned,
				.morphOffset = drawCmd.morphOffset,
				.activeMorphCount = drawCmd.activeMorphCount,
				.indexCount = drawCmd.indexCount,
				._pad = 0,
				.morphWeights = {drawCmd.morphWeights[0], drawCmd.morphWeights[1],
								 drawCmd.morphWeights[2], drawCmd.morphWeights[3]},
				.baseColorFactor = {drawCmd.baseColorFactor[0], drawCmd.baseColorFactor[1],
									drawCmd.baseColorFactor[2], drawCmd.baseColorFactor[3]}};
		}
	}

	// Encapsulate recording state
	FrameRecorder recorder(cmd, *_impl);

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

	auto stateAfterMain = Passes::MainPass{}.Execute(recorder, initialState);
	auto stateAfterTAA = Passes::TAAPass{}.Execute(recorder, stateAfterMain);
	Passes::BlitPass{}.Execute(recorder, stateAfterTAA);

	ZHLN_EndCommandBuffer(cmd);
	_impl->SubmitFrame();

	_impl->drawQueue.clear();
	_impl->uiDrawQueue.clear();
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

	impl->currentUniforms.camPos[3] = static_cast<float>(impl->taaState.frameIndex);

	std::memcpy(impl->currentUniforms.sh, impl->iblPayload.shCoeffs.data(), sizeof(JPH::Vec4) * 9);

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
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius,
		  uint32_t jointOffset, bool isSkinned, uint32_t morphOffset, uint32_t activeMorphCount,
		  const float* morphWeights) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	if (mesh.vertexBuffer == BufferHandle::Invalid) {
		return;
	}

	// 1. Resolve Mesh Handles with Generational Safety checks
	auto* nativeMesh = impl->meshPool.Resolve(static_cast<uint64_t>(mesh.vertexBuffer));
	if (nativeMesh == nullptr) [[unlikely]] {
		return; // Stale or invalid handle; safely ignore
	}

	auto* nativeIndexMesh = mesh.indexBuffer != BufferHandle::Invalid
								? impl->meshPool.Resolve(static_cast<uint64_t>(mesh.indexBuffer))
								: nullptr;

	// 2. Resolve Material Handle with Generational Safety checks
	auto* nativeMaterial = impl->materialPool.Resolve(static_cast<uint64_t>(material.pipeline));
	if (nativeMaterial == nullptr) [[unlikely]] {
		return; // Stale or invalid material; safely ignore
	}

	// 3. Record Draw Command using safe resolved raw pointers
	impl->drawQueue.push_back(
		DrawCommand{.material = nativeMaterial,
					.mesh = nativeMesh,
					.indexMesh = nativeIndexMesh,
					.transform = transform,
					.prevTransform = prevTransform,
					.albedoIndex = material.albedoIndex,
					.normalIndex = material.normalIndex,
					.pbrIndex = material.pbrIndex,
					.emissiveIndex = material.emissiveIndex,
					.cullRadius = cullRadius,
					.metallicFactor = material.metallicFactor,
					.roughnessFactor = material.roughnessFactor,
					.alphaCutoff = material.alphaCutoff,
					.alphaMode = material.alphaMode,
					.jointOffset = jointOffset,
					.isSkinned = isSkinned ? 1u : 0u,
					.baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1],
										material.baseColorFactor[2], material.baseColorFactor[3]},
					.morphOffset = morphOffset,
					.activeMorphCount = activeMorphCount,
					.morphWeights = {(morphWeights != nullptr) ? morphWeights[0] : 0.0f,
									 (morphWeights != nullptr) ? morphWeights[1] : 0.0f,
									 (morphWeights != nullptr) ? morphWeights[2] : 0.0f,
									 (morphWeights != nullptr) ? morphWeights[3] : 0.0f},
					.indexCount = mesh.indexCount});
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
