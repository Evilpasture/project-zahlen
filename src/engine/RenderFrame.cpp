// File: src/engine/RenderFrame.cpp
#include "ParallelDraw.hpp"
#include "RenderInternal.hpp"
#include "Zahlen/GUI.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "detail/RadixSort.hpp"
#include "imgui.h"

#include <algorithm>
#include <bit>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

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

// Declared outside to prevent template compilation errors [3]
struct GroupRange {
	NativeMaterial* material;
	NativeMesh* mesh;
	NativeMesh* indexMesh;
	uint32_t indexCount;
	uint32_t start;
	uint32_t count;
};

// ============================================================================
// Static Render Graph Nodes
// ============================================================================
namespace Passes {

struct ShadowPass {
	void Execute(VkCommandBuffer cmd, RenderContext::Impl& ctx) const noexcept {
		Profiler::ScopedGpuProfile<Stages::ShadowPass, FrameProfiler> timer(cmd, ctx.frame_index,
																			ctx.gpuProfiler);

		auto shadow_att = Vk::Transition(cmd, ctx.shadowMap, Vk::AsDepthAttachment);

		Vk::DynamicPass({.width = ctx.SHADOW_RES, .height = ctx.SHADOW_RES})
			.AddDepth(shadow_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
			.Execute(cmd, [&]() {
				for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
					const auto& draw = ctx.drawQueue[i];
					auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh); // Fixed namespace [3]

					ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 1};

					Vk::DrawInstanced(
						cmd,
						{.pipeline = ctx.shadowPipeline.Get(),
						 .layout = ctx.shadowPipelineLayout.Get(),
						 .set = ctx.bindlessSets[ctx.frame_index],
						 .vbo = mesh->buffer.Handle(),
						 .ibo = draw.indexMesh
									? std::bit_cast<NativeMesh*>(draw.indexMesh)->buffer.Handle()
									: VK_NULL_HANDLE, // Fixed namespace [3]
						 .vertexCount = mesh->vertexCount,
						 .indexCount = draw.indexCount,
						 .instanceCount = 1},
						pushConstants, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				}
			});

		Vk::Transition(cmd, shadow_att, Vk::AsReadOnly);
	}
};

struct MainPass {
	auto Execute(VkCommandBuffer cmd, RenderContext::Impl& ctx,
				 SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
					 in) const noexcept
		-> SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> {
		Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, ctx.frame_index,
																		  ctx.gpuProfiler);

		auto color_att = Vk::Transition(cmd, in.sceneColor, Vk::AsColorAttachment);
		auto vel_att = Vk::Transition(cmd, in.velocity, Vk::AsColorAttachment);
		auto norm_att = Vk::Transition(cmd, in.normRough, Vk::AsColorAttachment);
		auto depth_att = Vk::Transition(cmd, in.depth, Vk::AsDepthAttachment);

		auto drawCount = static_cast<uint32_t>(ctx.drawQueue.size());
		if (drawCount == 0) {
			return {color_att, vel_att, norm_att, depth_att};
		}

		ctx.SortDrawQueue();

		JPH::Array<GroupRange> groups;
		groups.reserve((drawCount + 15) / 16);

		auto mapRegion = ctx.instanceDataBuffers[ctx.frame_index].Map();
		auto* mapped = reinterpret_cast<InstanceData*>(mapRegion.data);

		NativeMaterial* currentMaterial = nullptr;
		NativeMesh* currentMesh = nullptr;
		NativeMesh* currentIndexMesh = nullptr;

		for (uint32_t i = 0; i < drawCount; ++i) {
			const auto& drawCmd = ctx.drawQueue[i];

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
							 ctx.indirectCommandsBuffers[ctx.frame_index].Valid() &&
							 (drawCount <= kGpuCullingMaxInstances);

		if (useGpuCulling) {
			struct FrustumPlanes {
				JPH::Vec4 planes[6];
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

			ctx.cullingPass.Dispatch(cmd, ctx.cullingSets[ctx.frame_index], (drawCount + 63) / 64,
									 1, 1, planes);
			Vk::CmdBarrierComputeToIndirect(cmd);

			Vk::DynamicPass(in.sceneColor.extent)
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
						if (!group.material->pipeline.Valid())
							continue;

						Vk::DrawIndirect(
							cmd,
							{.pipeline = group.material->pipeline.Get(),
							 .layout = group.material->layout.Get(),
							 .set = ctx.bindlessSets[ctx.frame_index],
							 .vbo = group.mesh->buffer.Handle(),
							 .ibo = group.indexMesh ? group.indexMesh->buffer.Handle()
													: VK_NULL_HANDLE,
							 .argumentBuffer =
								 ctx.indirectCommandsBuffers[ctx.frame_index].Handle(),
							 .offset = group.start * stride,
							 .drawCount = group.count,
							 .stride = static_cast<uint32_t>(stride)},
							ObjectConstants{.instanceId = kGpuCullingSentinel, .isShadowPass = 0});
					}
				});
		} else {
			[[maybe_unused]] static auto loggedOnce = []() -> bool {
				ZHLN::Log("RENDERING DIAGNOSTIC: Falling back to CPU Traditional Path!");
				return true;
			}();

			std::array<VkFormat, 3> colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT,
													VK_FORMAT_R16G16_SFLOAT,
													VK_FORMAT_R16G16B16A16_SFLOAT};

			Vk::DynamicPass(in.sceneColor.extent)
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
						in.sceneColor.extent, drawCount, kParallelChunkSize, ctx.frame_index,
						std::span<WorkerCmdContext>(ctx.workerCmds.data(),
													ctx.workerCmds.size()), // Fixed namespace [3]
						[&](VkCommandBuffer sec_cmd, uint32_t i) {
							const auto& drawCmd = ctx.drawQueue[i];
							if (!drawCmd.material->pipeline.Valid())
								return;

							ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};

							Vk::DrawInstanced(
								sec_cmd,
								{.pipeline = std::bit_cast<NativeMaterial*>(drawCmd.material)
												 ->pipeline.Get(), // Fixed namespace [3]
								 .layout = std::bit_cast<NativeMaterial*>(drawCmd.material)
											   ->layout.Get(), // Fixed namespace [3]
								 .set = ctx.bindlessSets[ctx.frame_index],
								 .vbo = std::bit_cast<NativeMesh*>(drawCmd.mesh)
											->buffer.Handle(), // Fixed namespace [3]
								 .ibo = drawCmd.indexMesh
											? std::bit_cast<NativeMesh*>(drawCmd.indexMesh)
												  ->buffer.Handle()
											: VK_NULL_HANDLE, // Fixed namespace [3]
								 .vertexCount = std::bit_cast<NativeMesh*>(drawCmd.mesh)
													->vertexCount, // Fixed namespace [3]
								 .indexCount = drawCmd.indexCount},
								pushConstants);
						});
				});
		}

		ctx.drawQueue.clear();
		return {color_att, vel_att, norm_att, depth_att};
	}
};

struct TAAPass {
	auto Execute(VkCommandBuffer cmd, RenderContext::Impl& ctx,
				 SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>
					 in) const noexcept
		-> SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
		Profiler::ScopedGpuProfile<Stages::TaaPass, FrameProfiler> timer(cmd, ctx.frame_index,
																		 ctx.gpuProfiler);

		auto color_ro = Vk::Transition(cmd, in.sceneColor, Vk::AsReadOnly);
		auto vel_ro = Vk::Transition(cmd, in.velocity, Vk::AsReadOnly);
		auto norm_ro = Vk::Transition(cmd, in.normRough, Vk::AsReadOnly);
		auto depth_ro = Vk::Transition(cmd, in.depth, Vk::AsReadOnly);

		if (ctx.taaState.enabled && ctx.taaPass.pipeline.Valid()) {
			auto accumCurr_ro =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Current());
			auto accumNext_u =
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(ctx.accumBuffers.Next());
			auto accumNext_att = Vk::Transition(cmd, accumNext_u, Vk::AsColorAttachment);

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
											ctx.frameUniformBuffers[ctx.frame_index].Handle()});

					ctx.taaPass.Execute(cmd, TAAPushConstants{.feedback = ctx.taaState.feedback});
				});

			Vk::Transition(cmd, accumNext_att, Vk::AsReadOnly);
		}

		return {color_ro, vel_ro, norm_ro, depth_ro};
	}
};

struct BlitPass {
	void Execute(VkCommandBuffer cmd, RenderContext::Impl& ctx,
				 SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
					 in) const noexcept {
		Profiler::ScopedGpuProfile<Stages::BlitPass, FrameProfiler> timer(cmd, ctx.frame_index,
																		  ctx.gpuProfiler);

		uint32_t imageIdx = ctx.current_image_index;
		VkImage swapImg = ctx.presentation.swapchain.Get().images[imageIdx];
		VkImageView swapView = ctx.presentation.swapchain.Get().views[imageIdx];

		Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_u = {.handle = swapImg,
															.view = swapView,
															.extent = in.sceneColor.extent,
															.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

		auto swap_att = Vk::Transition(cmd, swap_u, Vk::AsColorAttachment);

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
			alignas(16) float camPos[4];
			int giMode;
			float aoRadius;
			float aoBias;
			float aoPower;
			float giIntensity;
			int giSamples;
			float vignetteIntensity;
			float vignettePower;
			int enableSSR;
		} pc = {.invViewProj = ctx.currentUniforms.invViewProj,
				.viewProj = ctx.currentUniforms.unjitteredViewProj,
				.camPos = {ctx.currentUniforms.camPos[0], ctx.currentUniforms.camPos[1],
						   ctx.currentUniforms.camPos[2], ctx.currentUniforms.camPos[3]},
				.giMode = ctx.giSettings.mode,
				.aoRadius = ctx.giSettings.aoRadius,
				.aoBias = ctx.giSettings.aoBias, // Fixed typo [1]
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
								 .set = ctx.bindlessSets[ctx.frame_index],
								 .vbo = std::bit_cast<NativeMesh*>(draw.mesh)
											->buffer.Handle(), // Fixed namespace [3]
								 .vertexCount = std::bit_cast<NativeMesh*>(draw.mesh)
													->vertexCount}, // Fixed namespace [3]
								uipc);
						}
						ctx.uiDrawQueue.clear();
					}

					ImGui::Render();
					ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
				});
		}

		Vk::Transition(cmd, swap_att, Vk::AsPresent);
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
		return std::nullopt; // Invalid state (minimized)
	}
	return size; // Valid state
}

void RenderContext::BeginFrame() {
	if (_impl->initFence != VK_NULL_HANDLE) {
		vkWaitForFences(_impl->ctx.Device(), 1, &_impl->initFence, VK_TRUE, UINT64_MAX);
		vkDestroyFence(_impl->ctx.Device(), _impl->initFence, nullptr);
		_impl->initFence = VK_NULL_HANDLE;
		delete _impl->stagingContext;
		_impl->stagingContext = nullptr;
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
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->velocityBuffer = Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumBuffers[0] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumBuffers[1] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->normalRoughnessBuffer = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

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
		auto cmd = _impl->current_cmd;
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
		Vk::Transition(cmd, sNorm_att, Vk::AsReadOnly);
		Vk::Transition(cmd, depth_att, Vk::AsReadOnly);

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
		return;
	}

	VkCommandBuffer cmd = _impl->current_cmd;

	if (_impl->drawQueue.empty()) {
		ZHLN_EndCommandBuffer(cmd);
		_impl->SubmitFrame();
		return;
	}

	Passes::ShadowPass{}.Execute(cmd, *_impl);

	// Start graph with guaranteed state from InitialClear (or previous frame)
	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		initialState = {
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->sceneColor),
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->velocityBuffer),
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->normalRoughnessBuffer),
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->presentation.depthTarget,
																   VK_IMAGE_ASPECT_DEPTH_BIT)};

	auto stateAfterMain = Passes::MainPass{}.Execute(cmd, *_impl, initialState);
	auto stateAfterTAA = Passes::TAAPass{}.Execute(cmd, *_impl, stateAfterMain);
	Passes::BlitPass{}.Execute(cmd, *_impl, stateAfterTAA);

	ZHLN_EndCommandBuffer(cmd);
	_impl->SubmitFrame();
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

	impl->drawQueue.push_back(
		DrawCommand{.material = std::bit_cast<NativeMaterial*>(material.pipeline),
					.mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer),
					.indexMesh = std::bit_cast<NativeMesh*>(mesh.indexBuffer),
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

	impl->uiDrawQueue.push_back(
		{.mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer), .fontIndex = fontIndex});
}
} // namespace Renderer
} // namespace ZHLN
