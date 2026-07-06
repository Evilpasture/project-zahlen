// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Profiler.hpp"
#include "detail/RadixSort.hpp"
#include "engine/Scheduler.hpp"

#include <array>
#include <cstring>
#include <threading/TaskSystem.hpp>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN {

// ============================================================================
// Hoisted Uniform Structures
// ============================================================================

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

[[nodiscard]] inline std::array<float, 4> UnpackMorphWeights(const float* weights) noexcept {
	if (weights == nullptr) {
		return {0.0f, 0.0f, 0.0f, 0.0f};
	}
	return {weights[0], weights[1], weights[2], weights[3]};
}

template <typename... Ptrs> [[nodiscard]] constexpr bool AnyNull(Ptrs... ptrs) noexcept {
	return (... || (ptrs == nullptr));
}

inline void BarrierComputeWriteToVertexRead(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
	Vk::BeginBarrier<Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite>(cmd)
		.TransitionTo<Vk::BarrierStage::Vertex, Vk::BarrierAccess::ShaderRead>();
}

inline void BarrierComputeWriteToFragmentRead(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
	Vk::BeginBarrier<Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite>(cmd)
		.TransitionTo<Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead>();
}

inline void BarrierCounterReset(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
	Vk::BeginBarrier<Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite>(cmd)
		.TransitionTo<Vk::BarrierStage::Compute,
					  Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite>();
}

template <typename TargetImageT, typename RecordFn>
inline void DispatchPostProcessPass(VkCommandBuffer cmd, TargetImageT& targetImage,
									VkAttachmentLoadOp loadOp, RecordFn&& recordFn,
									VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
									const Color4& clearColor = kClearColorBlack) noexcept {
	// Revert from ReadToColor (which issues redundant barriers) to AssumeLayout
	auto attachment = Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(targetImage);
	Vk::DynamicPass(targetImage.extent)
		.AddColor(attachment, loadOp, storeOp, clearColor)
		.Execute(cmd, std::forward<RecordFn>(recordFn));
}

struct TaskSystemSchedulerAdapter {
	void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
		TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
	}
};
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

// ----------------------------------------------------------------------------
// Frame Graph Resource Tags
// ----------------------------------------------------------------------------

using Res_SceneColor =
	Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Velocity = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_NormRough =
	Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Depth = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowMap = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowAtlas =
	Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_Ambient =
	Vk::GraphImage<"Ambient", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Lighting =
	Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_PostProcess =
	Vk::GraphImage<"PostProcess", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomThresh =
	Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomBlur =
	Vk::GraphImage<"BloomBlur", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomFinal =
	Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_AccumCurr =
	Vk::GraphImage<"AccumCurr", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_AccumNext =
	Vk::GraphImage<"AccumNext", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaEdge = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaWeight =
	Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Swapchain =
	Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;

} // namespace

// ============================================================================
// RenderContext Infrastructure & Lifecycles
// ============================================================================

void RenderContext::Impl::SortDrawQueue() {
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	ZHLN::Array<SortItem> items(drawCount);
	ZHLN::Array<SortItem> temp(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		items[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].posMesh),
					.payload = i};
	}

	RadixSort64(items.data(), temp.data(), drawCount);

	ZHLN::Array<DrawCommand> sortedDrawQueue(drawCount);
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

void RenderContext::Impl::InitialClearTargets([[maybe_unused]] VkCommandBuffer cmd) noexcept {}

void RenderContext::Impl::DispatchSkinningPasses() {
	if (!hasSkinnedThisFrame) {
		return;
	}

	ZHLN_PROFILE_SCOPE("GPU Compute Skinning");
	VkCommandBuffer cmd = current_cmd;
	skinningPass.Bind(cmd);

	for (const auto& drawCmd : drawQueue) {
		if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
			auto* posMesh = drawCmd.posMesh;
			auto* attrMesh = drawCmd.attrMesh;
			auto* skinMesh = drawCmd.skinMesh;
			auto* scratchMesh = meshPool.Resolve(drawCmd.skinnedVertexBuffer);

			if (AnyNull(posMesh, attrMesh, skinMesh, scratchMesh)) {
				continue;
			}

			SkinningConstants pcs = {
				.inPosAddr = posMesh->vboAddress,
				.inAttrAddr = attrMesh->vboAddress,
				.inSkinAddr = skinMesh->vboAddress,
				.outPosAddr = scratchMesh->vboAddress,
				.outAttrAddr =
					scratchMesh->vboAddress + (scratchMesh->vertexCount * sizeof(VertexPosition)),
				.jointsAddr =
					Vk::GetBufferDeviceAddress(ctx.Device(), jointBuffers[frame_index].Handle()),
				.morphDeltasAddr =
					Vk::GetBufferDeviceAddress(ctx.Device(), morphDeltasBuffer.Handle()),
				.vertexCount = posMesh->vertexCount,
				.jointOffset = drawCmd.jointOffset,
				.morphOffset = drawCmd.morphOffset,
				.activeMorphCount = drawCmd.activeMorphCount,
				.morphWeights = {drawCmd.morphWeights[0], drawCmd.morphWeights[1],
								 drawCmd.morphWeights[2], drawCmd.morphWeights[3]}};

			skinningPass.PushConstants(cmd, pcs);
			skinningPass.Dispatch(cmd, (posMesh->vertexCount + 63) / 64, 1, 1);
		}
	}

	BarrierComputeWriteToVertexRead(Vk::CommandBuffer<Vk::QueueType::Graphics>{cmd});
}

RenderResult RenderContext::BeginFrame() noexcept {
	if (_impl->stagingContext) {
		_impl->stagingContext->Wait();
		_impl->stagingContext.reset();
	}

	const auto& s = _impl->sync[_impl->frame_index];

	auto wait_res =
		ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, _impl->pools[_impl->frame_index]);
	if (wait_res == ZHLN_FrameResult_DeviceLost) {
		return std::unexpected(RenderFrameResult::DeviceLost);
	}
	_impl->deletionQueue.BeginFrame(_impl->frame_index);
	_impl->activeQueueGuard.emplace(_impl->deletionQueue);

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

		if (!_impl->RecreateTargets(ext)) {
			return std::unexpected(RenderFrameResult::Error);
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

	_impl->pendingAcquires.Drain(_impl->current_cmd);

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
									   .imageIndex = current_image_index,
									   .stagingSemaphore = transferRingBuffer.GetSemaphore(),
									   .stagingWaitValue = transferRingBuffer.GetCurrentValue()};

	ZHLN_FrameResult res = Vk::SubmitAndPresent(submitDesc);
	if (res != ZHLN_FrameResult_Ok) {
		resized = true;
	}

	StaticResourceManager(
		&accumBuffers, &taaPass, &fxaaPass, &smaaEdgePass, &smaaWeightPass, &smaaBlendPass,
		&ambientPass, &lightingPass, &reflectionPass, &blitPass, &bloomThresholdPass,
		&bloomBlurHPass, &bloomBlurVPass, &frameUniformBuffers, &lightStorageBuffers,
		&instanceDataBuffers, &indirectCommandsBuffers, &shadowIndirectBuffers, &jointBuffers,
		&bindlessSets, &tlas, &tlasBuffer, &tlasScratchBuffer, &clusterGridBuffers,
		&lightIndexListBuffers, &globalCounterBuffers, &clusterCullingSets, &parallelRecorder)
		.FlipAll();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
	hasSkinnedThisFrame = false;
	return res;
}

void RenderContext::Impl::BuildTLAS(VkCommandBuffer cmd) noexcept {
	if (!rtCtx.Valid() || drawQueue.empty()) {
		return;
	}

	tlasInstancesScratch.clear();
	tlasInstancesScratch.reserve(drawQueue.size());

	for (uint32_t i = 0; i < drawQueue.size(); ++i) {
		const auto& drawCmd = drawQueue[i];
		auto* mesh = drawCmd.posMesh;

		if (mesh == nullptr || mesh->blasAddress == 0 ||
			((drawCmd.flags & DrawFlags::Skinned) != DrawFlags::None) ||
			((drawCmd.flags & DrawFlags::ExcludeFromTLAS) != DrawFlags::None)) {
			continue;
		}

		VkAccelerationStructureInstanceKHR inst{};
		const auto& t = drawCmd.instanceData.world;

		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 4; ++col) {
				inst.transform.matrix[row][col] = t(row, col);
			}
		}

		inst.instanceCustomIndex = i;
		inst.mask = 0xFF;
		inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		inst.accelerationStructureReference = mesh->blasAddress;
		tlasInstancesScratch.push_back(inst);
	}

	if (tlasInstancesScratch.empty()) {
		return;
	}

	auto& stagingBuf = tlasStagingBuffers[frame_index];
	auto& instanceBuf = tlasInstanceBuffers[frame_index];

	std::memcpy(stagingBuf.Map().data, tlasInstancesScratch.data(),
				tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

	Vk::CopyBuffer(cmd, stagingBuf, instanceBuf,
				   tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
							.src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
							.dst_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
							.dst_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
										  VK_ACCESS_2_SHADER_READ_BIT});

	ZHLN_TlasGeometryDesc geom = {
		.instance_data = Vk::GetBufferDeviceAddress(ctx.Device(), instanceBuf.Handle())};

	rtCtx.CmdBuildTlas(
		cmd, geom, tlas[frame_index],
		Vk::GetBufferDeviceAddress(ctx.Device(), tlasScratchBuffer[frame_index].Handle()),
		static_cast<uint32_t>(tlasInstancesScratch.size()));

	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
							.src_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
							.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
							.dst_access = VK_ACCESS_2_SHADER_READ_BIT});
}

// ============================================================================
// Zero-Copy Pass Construction Factory
// ============================================================================

struct PassFactory {
	RenderContext::Impl& self;
	VkCommandBuffer cmd;
	uint32_t fIdx;
	VkDevice device;
	FrameRecorder& recorder;
	const RenderContext::Impl::PPPushConstants& pc;
	uint32_t lightVariant;
	uint32_t reflVariant;

	[[nodiscard]] auto GetTLAS() const noexcept {
		if constexpr (isMac) {
			return Vk::SkipWrite{};
		} else {
			return self.rtCtx.Valid() ? (const VkAccelerationStructureKHR*)&self.tlas.Current()
									  : (const VkAccelerationStructureKHR*)nullptr;
		}
	}

	[[nodiscard]] auto MakeMainPass() const noexcept {
		return Vk::MakePassUnsafe<"Main", Vk::ColorWrite<Res_SceneColor>,
								  Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>,
								  Vk::DepthWrite<Res_Depth>>([this](VkCommandBuffer c) noexcept {
			FrameRecorder mainRec(c, self);
			Passes::MainPass{}.Execute(
				mainRec,
				SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>{
					.sceneColor =
						Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(self.sceneColor),
					.velocity = Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
						self.velocityBuffer),
					.normRough = Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
						self.normalRoughnessBuffer),
					.depth = Vk::AssumeLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)});
		});
	}

	[[nodiscard]] auto MakeShadowPass() const noexcept {
		return Vk::MakePassUnsafe<"MainShadow", Vk::ColorWrite<Res_SceneColor>,
								  Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>,
								  Vk::DepthWrite<Res_Depth>, Vk::DepthWrite<Res_ShadowMap>,
								  Vk::DepthWrite<Res_ShadowAtlas>>([this](
																	   VkCommandBuffer c) noexcept {
			auto& rec = self.parallelRecorder.Current();
			rec.Reset();
			TaskSystemScheduler scheduler;
			rec.Record(
				scheduler,
				[&](Vk::RecordingSlot slot) noexcept {
					FrameRecorder shadowRec(slot.cmd, self);
					Passes::ShadowPass{}.Execute(shadowRec);
				},
				[&](Vk::RecordingSlot slot) noexcept {
					FrameRecorder mainRec(slot.cmd, self);
					Passes::MainPass{}.Execute(
						mainRec,
						SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>{
							.sceneColor =
								Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
									self.sceneColor),
							.velocity = Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
								self.velocityBuffer),
							.normRough = Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
								self.normalRoughnessBuffer),
							.depth = Vk::AssumeLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
								self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)});
				});
			Vk::ExecuteCommands(c, rec.GetCommandBuffers());
		});
	}

	[[nodiscard]] auto MakeAmbientPass() const noexcept {
		return Vk::MakePass<"Ambient", Vk::ShaderRead<Res_SceneColor>,
							Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>,
							Vk::ColorWrite<Res_Ambient>>([this](auto& ctx) noexcept {
			self.ambientPass.WriteNext(
				device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(self.sceneColor),
				self.defaultSampler.Get(),
				Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
					self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT),
				Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
					self.normalRoughnessBuffer),
				self.pointSampler.Get(), self.iblPayload.prefilteredView.Get(),
				self.iblPayload.brdfLutView.Get(), self.clampSampler.Get(),
				self.frameUniformBuffers[fIdx].Handle());
			self.ambientPass.Execute(ctx.Cmd(), pc);
		});
	}

	[[nodiscard]] auto MakeLightingPass() const noexcept {
		return Vk::MakePass<"Lighting", Vk::ShaderRead<Res_SceneColor>,
							Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>,
							Vk::ShaderRead<Res_Ambient>, Vk::ShaderRead<Res_ShadowMap>,
							Vk::ShaderRead<Res_ShadowAtlas>, Vk::ColorWrite<Res_Lighting>>(
			[this](auto& ctx) noexcept {
				self.lightingPass.WriteNext(
					device,
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(self.sceneColor),
					self.defaultSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.normalRoughnessBuffer),
					self.lightStorageBuffers[fIdx].Handle(),
					self.frameUniformBuffers[fIdx].Handle(), self.shadowMap.view.Get(),
					self.shadowSampler.Get(), self.ltcMatView.Get(), self.ltcAmpView.Get(),
					self.clampSampler.Get(), self.clusterGridBuffers[fIdx].Handle(),
					self.lightIndexListBuffers[fIdx].Handle(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(self.ambientTarget),
					self.pointSampler.Get(), GetTLAS(), self.shadowAtlasCubeView.Get(),
					self.shadowAtlas2DView.Get());
				self.lightingPass.ExecuteVariant(ctx.Cmd(), lightVariant, pc);
			});
	}

	[[nodiscard]] auto MakeReflectionPass() const noexcept {
		return Vk::MakePass<"Reflection", Vk::ShaderRead<Res_SceneColor>,
							Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>,
							Vk::ShaderRead<Res_Lighting>, Vk::ShaderRead<Res_ShadowMap>,
							Vk::ShaderRead<Res_ShadowAtlas>, Vk::ColorWrite<Res_PostProcess>>(
			[this](auto& ctx) noexcept {
				Profiler::ScopedGpuProfile<Stages::PostProcessPass, FrameProfiler> timer(
					ctx.Cmd(), fIdx, self.gpuProfiler);
				self.reflectionPass.WriteNext(
					device,
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(self.sceneColor),
					self.defaultSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.normalRoughnessBuffer),
					self.pointSampler.Get(), self.iblPayload.prefilteredView.Get(), GetTLAS(),
					self.frameUniformBuffers[fIdx].Handle(), self.iblPayload.brdfLutView.Get(),
					self.clampSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.lightingTarget));
				self.reflectionPass.ExecuteVariant(ctx.Cmd(), reflVariant, pc);
			});
	}

	template <bool FullBright> [[nodiscard]] auto MakeForwardPass() const noexcept {
		using ColorTargetRes = std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>;
		auto& targetImage = [&]() -> auto& {
			if constexpr (FullBright) {
				return self.sceneColor;
			} else {
				return self.postProcessTarget;
			}
		}();

		return Vk::MakePassUnsafe<"Forward", Vk::ColorWrite<ColorTargetRes>,
								  Vk::DepthWrite<Res_Depth>>(
			[this, &targetImage](VkCommandBuffer c) noexcept {
				FrameRecorder fwdRecorder(c, self);
				Passes::ForwardPass{}.Execute(
					fwdRecorder,
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(targetImage),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT));
			});
	}

	template <bool FullBright> [[nodiscard]] auto MakeBloomThresholdPass() const noexcept {
		using BloomInputRes = std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>;
		const auto& inputColor = [&]() -> auto& {
			if constexpr (FullBright) {
				return self.sceneColor;
			} else {
				return self.postProcessTarget;
			}
		}();
		return Vk::MakePass<"BloomThreshold", Vk::ShaderRead<BloomInputRes>,
							Vk::ColorWrite<Res_BloomThresh>>(
			[this, &inputColor](auto& ctx) noexcept {
				Profiler::ScopedGpuProfile<Stages::BloomThreshPass, FrameProfiler> timer(
					ctx.Cmd(), fIdx, self.gpuProfiler);
				self.bloomThresholdPass.WriteNext(
					device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
					self.defaultSampler.Get());
				self.bloomThresholdPass.Execute(ctx.Cmd());
			});
	}

	[[nodiscard]] auto MakeBloomBlurHPass() const noexcept {
		struct BlurPushConstants {
			int horizontal;
			float texelSize;
		};
		return Vk::MakePass<"BloomBlurH", Vk::ShaderRead<Res_BloomThresh>,
							Vk::ColorWrite<Res_BloomBlur>>([this](auto& ctx) noexcept {
			Profiler::ScopedGpuProfile<Stages::BloomBlurHPass, FrameProfiler> timer(
				ctx.Cmd(), fIdx, self.gpuProfiler);
			self.bloomBlurHPass.WriteNext(
				device,
				Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
					self.bloomThresholdTarget),
				self.defaultSampler.Get());
			self.bloomBlurHPass.Execute(
				ctx.Cmd(), BlurPushConstants{
							   .horizontal = 1,
							   .texelSize = 1.0f / (float)self.bloomThresholdTarget.extent.width});
		});
	}

	[[nodiscard]] auto MakeBloomBlurVPass() const noexcept {
		struct BlurPushConstants {
			int horizontal;
			float texelSize;
		};
		return Vk::MakePass<"BloomBlurV", Vk::ShaderRead<Res_BloomBlur>,
							Vk::ColorWrite<Res_BloomFinal>>([this](auto& ctx) noexcept {
			Profiler::ScopedGpuProfile<Stages::BloomBlurVPass, FrameProfiler> timer(
				ctx.Cmd(), fIdx, self.gpuProfiler);
			self.bloomBlurVPass.WriteNext(
				device,
				Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(self.bloomBlurTarget),
				self.defaultSampler.Get());
			self.bloomBlurVPass.Execute(
				ctx.Cmd(),
				BlurPushConstants{.horizontal = 0,
								  .texelSize = 1.0f / (float)self.bloomBlurTarget.extent.height});
		});
	}

	template <bool FullBright, AAMode Mode, typename AALambdaT>
	auto MakeAAPass(AALambdaT&& aaLambda) const noexcept {
		using AAColorInputRes = std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>;
		auto& aaColorInputImage = [&]() -> auto& {
			if constexpr (FullBright) {
				return self.sceneColor;
			} else {
				return self.postProcessTarget;
			}
		}();

		return Vk::MakePassUnsafe<"AA", Vk::ShaderRead<AAColorInputRes>,
								  Vk::ShaderRead<Res_Velocity>, Vk::ShaderRead<Res_NormRough>,
								  Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_AccumNext>,
								  Vk::ColorWrite<Res_SmaaEdge>, Vk::ColorWrite<Res_SmaaWeight>,
								  Vk::ShaderRead<Res_AccumCurr>>(
			[aaLambda = std::forward<AALambdaT>(aaLambda),
			 &aaColorInputImage](VkCommandBuffer c) noexcept { aaLambda(c, aaColorInputImage); });
	}

	template <bool FullBright, AAMode Mode, typename GetSwapchainImageT>
	auto MakeBlitPass(GetSwapchainImageT&& getSwapchainImage) const noexcept {
		using BlitInputRes =
			std::conditional_t<Mode != AAMode::None, Res_AccumNext,
							   std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>>;
		const auto& blitInputImage = [&]() -> auto& {
			if constexpr (Mode != AAMode::None) {
				return self.accumBuffers.Next();
			} else {
				if constexpr (FullBright) {
					return self.sceneColor;
				} else {
					return self.postProcessTarget;
				}
			}
		}();

		return Vk::MakePassUnsafe<"Blit", Vk::ShaderRead<BlitInputRes>,
								  Vk::ShaderRead<Res_BloomFinal>, Vk::ColorWrite<Res_Swapchain>>(
			[this, &blitInputImage,
			 getSwapchainImage =
				 std::forward<GetSwapchainImageT>(getSwapchainImage)](VkCommandBuffer c) noexcept {
				FrameRecorder blitRecorder(c, self);
				Passes::BlitPass{}.Execute(
					blitRecorder,
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(blitInputImage),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.bloomFinalTarget),
					getSwapchainImage(), FullBright ? 1 : 0);
			});
	}
};

// ============================================================================
// Multi-Axis Frame Graph Generator
// ============================================================================
namespace {
template <bool FullBright, AAMode Mode, typename AALambdaT, typename GetSwapchainImageT>
auto BuildFrameGraph(const PassFactory& factory, AALambdaT&& aaLambda,
					 GetSwapchainImageT&& getSwapchainImage) {
	auto corePasses = [&] {
		if constexpr (FullBright) {
			return std::tuple{factory.MakeMainPass(),
							  factory.template MakeForwardPass<FullBright>()};
		} else {
			return std::tuple{factory.MakeShadowPass(), factory.MakeAmbientPass(),
							  factory.MakeLightingPass(), factory.MakeReflectionPass(),
							  factory.template MakeForwardPass<FullBright>()};
		}
	}();

	// Sequence the individual bloom sub-passes
	auto bloomPasses = std::tuple{factory.template MakeBloomThresholdPass<FullBright>(),
								  factory.MakeBloomBlurHPass(), factory.MakeBloomBlurVPass()};

	auto tailPasses = [&] {
		if constexpr (Mode != AAMode::None) {
			return std::tuple{
				factory.template MakeAAPass<FullBright, Mode>(std::forward<AALambdaT>(aaLambda)),
				factory.template MakeBlitPass<FullBright, Mode>(
					std::forward<GetSwapchainImageT>(getSwapchainImage))};
		} else {
			return std::tuple{factory.template MakeBlitPass<FullBright, Mode>(
				std::forward<GetSwapchainImageT>(getSwapchainImage))};
		}
	}();

	return std::apply(
		[](auto&&... passes) { return Vk::CompileTimeFrameGraph(std::move(passes)...); },
		std::tuple_cat(std::move(corePasses), std::move(bloomPasses), std::move(tailPasses)));
}

template <bool FullBright, AAMode Mode, typename AALambdaT, typename GetSwapchainImageT>
void ExecuteFrameGraph(RenderContext::Impl& self, VkCommandBuffer cmd, const PassFactory& factory,
					   AALambdaT&& aaLambda, GetSwapchainImageT&& getSwapchainImage) {
	auto graph =
		BuildFrameGraph<FullBright, Mode>(factory, std::forward<AALambdaT>(aaLambda),
										  std::forward<GetSwapchainImageT>(getSwapchainImage));
	typename decltype(graph)::Binder binder;

	// Bind common resources
	auto sceneColorRef = MakeRef<Res_SceneColor>(self.sceneColor);
	auto velocityRef = MakeRef<Res_Velocity>(self.velocityBuffer);
	auto normRoughRef = MakeRef<Res_NormRough>(self.normalRoughnessBuffer);
	auto depthRef = MakeRef<Res_Depth>(self.presentation.depthTarget);
	auto bloomThreshRef = MakeRef<Res_BloomThresh>(self.bloomThresholdTarget);
	auto bloomBlurRef = MakeRef<Res_BloomBlur>(self.bloomBlurTarget);
	auto bloomFinalRef = MakeRef<Res_BloomFinal>(self.bloomFinalTarget);
	auto swapchainRef = Vk::MakeRef<Res_Swapchain>(
		self.presentation.swapchain.Get().images[self.current_image_index],
		self.presentation.swapchain.Get().views[self.current_image_index], self.sceneColor.extent);

	AutoBind(binder, sceneColorRef, velocityRef, normRoughRef, depthRef, bloomThreshRef,
			 bloomBlurRef, bloomFinalRef, swapchainRef);

	if constexpr (Mode != AAMode::None) {
		auto accumCurrRef = MakeRef<Res_AccumCurr>(self.accumBuffers.Current());
		auto accumNextRef = MakeRef<Res_AccumNext>(self.accumBuffers.Next());
		auto smaaEdgeRef = MakeRef<Res_SmaaEdge>(self.smaaEdgeTarget);
		auto smaaWeightRef = MakeRef<Res_SmaaWeight>(self.smaaWeightTarget);

		AutoBind(binder, accumCurrRef, accumNextRef, smaaEdgeRef, smaaWeightRef);
	}

	if constexpr (!FullBright) {
		auto shadowMapRef = MakeRef<Res_ShadowMap>(self.shadowMap);
		auto shadowAtlasRef = MakeRef<Res_ShadowAtlas>(self.shadowAtlas);
		auto ambientRef = MakeRef<Res_Ambient>(self.ambientTarget);
		auto lightingRef = MakeRef<Res_Lighting>(self.lightingTarget);
		auto postProcessRef = MakeRef<Res_PostProcess>(self.postProcessTarget);

		AutoBind(binder, shadowMapRef, shadowAtlasRef, ambientRef, lightingRef, postProcessRef);
	}

	graph.Execute(cmd, binder);
}
} // namespace

template <bool FullBright>
void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
	uint32_t imageIdx = current_image_index;
	VkDevice device = ctx.Device();
	uint32_t fIdx = frame_index;

	using namespace ZHLN::Vk;
	FrameRecorder recorder(cmd, *this);

	// Safe builder for explicitly typed swapchain images
	auto getSwapchainImage = [&]() -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
		return {.handle = presentation.swapchain.Get().images[imageIdx],
				.view = presentation.swapchain.Get().views[imageIdx],
				.extent = sceneColor.extent,
				.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
				.format = presentation.swapchain.Get().format};
	};

	auto aaLambda = [&]([[maybe_unused]] VkCommandBuffer c, const auto& inputColor) noexcept {
		Profiler::ScopedGpuProfile<Stages::AAPass, FrameProfiler> timer(c, fIdx, gpuProfiler);
		const auto smaaEdge_ro =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(smaaEdgeTarget);
		const auto smaaWeight_ro =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(smaaWeightTarget);

		if (aaState.mode == AAMode::TAA && taaPass.pipeline.Valid()) {
			struct TAAPushConstants {
				float feedback;
			};
			DispatchPostProcessPass(c, accumBuffers.Next(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
				taaPass.WriteNext(
					device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						accumBuffers.Current()),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(velocityBuffer),
					defaultSampler.Get(), frameUniformBuffers[fIdx].Handle());
				taaPass.Execute(c, TAAPushConstants{.feedback = aaState.taaFeedback});
			});
		} else if (aaState.mode == AAMode::FXAA && fxaaPass.pipeline.Valid()) {
			struct FXAAPushConstants {
				float rcpFrameX;
				float rcpFrameY;
				float subpix;
				float edgeThreshold;
				float edgeThresholdMin;
				float _pad;
			} pc = {.rcpFrameX = 1.0f / (float)inputColor.extent.width,
					.rcpFrameY = 1.0f / (float)inputColor.extent.height,
					.subpix = aaState.fxaaSubpix,
					.edgeThreshold = aaState.fxaaEdgeThreshold,
					.edgeThresholdMin = aaState.fxaaEdgeThresholdMin,
					._pad = 0.0f};
			DispatchPostProcessPass(c, accumBuffers.Next(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
				fxaaPass.WriteNext(
					device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
					defaultSampler.Get());
				fxaaPass.Execute(c, pc);
			});
		} else if (aaState.mode == AAMode::SMAA && smaaEdgePass.pipeline.Valid()) {
			struct SMAAMetrics {
				float rcpWidth;
				float rcpHeight;
				float width;
				float height;
			} metrics = {.rcpWidth = 1.0f / (float)inputColor.extent.width,
						 .rcpHeight = 1.0f / (float)inputColor.extent.height,
						 .width = (float)inputColor.extent.width,
						 .height = (float)inputColor.extent.height};
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				c, smaaEdgeTarget.image.Handle());
			DispatchPostProcessPass(c, smaaEdgeTarget, VK_ATTACHMENT_LOAD_OP_CLEAR, [&]() {
				smaaEdgePass.WriteNext(
					device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
					defaultSampler.Get(), pointSampler.Get());
				smaaEdgePass.Execute(c, metrics,
									 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			});
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				c, smaaEdgeTarget.image.Handle());
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				c, smaaWeightTarget.image.Handle());

			DispatchPostProcessPass(c, smaaWeightTarget, VK_ATTACHMENT_LOAD_OP_CLEAR, [&]() {
				const auto& [areaView, searchView] =
					std::tie(textureViews[smaaAreaTexIdx], textureViews[smaaSearchTexIdx]);

				smaaWeightPass.WriteNext(device, smaaEdge_ro, areaView, searchView,
										 defaultSampler.Get(), pointSampler.Get());
				smaaWeightPass.Execute(c, metrics,
									   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			});
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				c, smaaWeightTarget.image.Handle());

			DispatchPostProcessPass(c, accumBuffers.Next(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
				smaaBlendPass.WriteNext(device, inputColor, smaaWeight_ro, defaultSampler.Get(),
										pointSampler.Get());
				smaaBlendPass.Execute(c, metrics,
									  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			});
		} else {
			DispatchPostProcessPass(c, accumBuffers.Next(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
				blitPass.WriteNext(
					device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
					defaultSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor));
				BlitPushConstants pc = {
					.vignetteIntensity = 0.0f, .vignettePower = 0.0f, .fullBright = 0};
				blitPass.Execute(c, pc);
			});
		}
	};

	uint32_t lightVariant = (giSettings.enableRTR && rtCtx.Valid()) ? 1 : 0;
	uint32_t reflVariant =
		(giSettings.enableSSR ? 1 : 0) | ((giSettings.enableRTR && rtCtx.Valid()) ? 2 : 0);

	PassFactory factory{
		.self = *this,
		.cmd = cmd,
		.fIdx = fIdx,
		.device = device,
		.recorder = recorder,
		.pc = {.invViewProj = current_view_proj.Inversed(),
			   .viewProj = current_view_proj,
			   .camPos = {currentUniforms.camPos[0], currentUniforms.camPos[1],
						  currentUniforms.camPos[2], currentUniforms.camPos[3]},
			   .giMode = giSettings.mode,
			   .aoRadius = giSettings.aoRadius,
			   .aoBias = giSettings.aoBias,
			   .aoPower = giSettings.aoPower,
			   .giIntensity = giSettings.giIntensity,
			   .giSamples = giSettings.giSamples,
			   .enableSSR = giSettings.enableSSR,
			   .enableRTR = (tlas.Current() != VK_NULL_HANDLE) ? giSettings.enableRTR : 0,
			   ._pad = {}},
		.lightVariant = lightVariant,
		.reflVariant = reflVariant};

	switch (aaState.mode) {
		case AAMode::None:
			ExecuteFrameGraph<FullBright, AAMode::None>(*this, cmd, factory, aaLambda,
														getSwapchainImage);
			break;
		case AAMode::FXAA:
			ExecuteFrameGraph<FullBright, AAMode::FXAA>(*this, cmd, factory, aaLambda,
														getSwapchainImage);
			break;
		case AAMode::TAA:
			ExecuteFrameGraph<FullBright, AAMode::TAA>(*this, cmd, factory, aaLambda,
													   getSwapchainImage);
			break;
		case AAMode::SMAA:
			ExecuteFrameGraph<FullBright, AAMode::SMAA>(*this, cmd, factory, aaLambda,
														getSwapchainImage);
			break;
	}
}

template void
	RenderContext::Impl::RecordSceneFrame<true>(Vk::CommandBuffer<Vk::QueueType::Graphics>);
template void
	RenderContext::Impl::RecordSceneFrame<false>(Vk::CommandBuffer<Vk::QueueType::Graphics>);

RenderResult RenderContext::EndFrame() noexcept {
	struct EndFrameGuard {
		RenderContext::Impl* impl;
		EndFrameGuard(RenderContext::Impl* i) : impl(i) {}
		~EndFrameGuard() { impl->activeQueueGuard.reset(); }
		EndFrameGuard(const EndFrameGuard&) = delete;
		EndFrameGuard& operator=(const EndFrameGuard&) = delete;
		EndFrameGuard(EndFrameGuard&&) = delete;
		EndFrameGuard& operator=(EndFrameGuard&&) = delete;
	} frameGuard{_impl.get()};

	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE) {
		_impl->drawQueue.clear();
		_impl->uiDrawQueue.clear();
		return std::unexpected(RenderFrameResult::Error);
	}

	_impl->DispatchSkinningPasses();

	VkCommandBuffer cmd = _impl->current_cmd;

	if (_impl->drawQueue.size() > kGpuCullingMaxInstances) {
		_impl->drawQueue.resize(kGpuCullingMaxInstances);
	}

	_impl->SortDrawQueue();

	auto drawCount = static_cast<uint32_t>(_impl->drawQueue.size());
	if (drawCount > 0) {
		auto mapped = _impl->instanceDataBuffers[_impl->frame_index].Map();
		auto* dst = static_cast<InstanceData*>(mapped.data);
		for (uint32_t i = 0; i < drawCount; ++i) {
			dst[i] = _impl->drawQueue[i].instanceData;
		}
	}
	_impl->BuildTLAS(cmd);

	if (_impl->currentUniforms.fullBright != 0) {
		_impl->RecordSceneFrame<true>(Vk::CommandBuffer<Vk::QueueType::Graphics>{cmd});
	} else {
		_impl->RecordSceneFrame<false>(Vk::CommandBuffer<Vk::QueueType::Graphics>{cmd});
	}

	ZHLN_EndCommandBuffer(cmd);

	auto res = _impl->SubmitFrame();
	if (res != ZHLN_FrameResult_Ok) {
		return std::unexpected(MapFrameResult(res));
	}

	_impl->drawQueue.clear();
	_impl->uiDrawQueue.clear();

	return {};
}

void RenderContext::Impl::ProvokeDeviceLostInternal() const {
	if (!hangGpuPass.pipeline.Valid()) {
		return;
	}

	VkCommandBuffer cmd = current_cmd;
	bool submitQueue = false;
	Vk::CommandPool tempPool;

	if (cmd == VK_NULL_HANDLE) {
		tempPool = Vk::CommandPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		if (!tempPool.Allocate(1)) {
			return;
		}
		cmd = tempPool[0];
		ZHLN_BeginCommandBuffer(cmd);
		submitQueue = true;
	}

	hangGpuPass.Bind(cmd);
	hangGpuPass.Dispatch(cmd, 512, 512, 1);

	if (submitQueue) {
		ZHLN_EndCommandBuffer(cmd);
		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .pNext = nullptr,
											 .commandBuffer = cmd,
											 .deviceMask = 0};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.pNext = nullptr,
								.flags = 0,
								.waitSemaphoreInfoCount = 0,
								.pWaitSemaphoreInfos = nullptr,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo,
								.signalSemaphoreInfoCount = 0,
								.pSignalSemaphoreInfos = nullptr};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(ctx.GraphicsQueue());
	}
}

namespace Renderer {

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const DrawParams& params) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	auto* posMesh = impl->meshPool.Resolve(mesh.posBuffer);
	auto* attrMesh = impl->meshPool.Resolve(mesh.attrBuffer);
	auto* nativeMaterial = impl->materialPool.Resolve(material.pipeline);
	if (AnyNull(attrMesh, posMesh, nativeMaterial)) [[unlikely]] {
		ZHLN::Assert(false);
		return;
	}

	auto* skinMesh = mesh.skinBuffer != BufferHandle::Invalid
						 ? impl->meshPool.Resolve(mesh.skinBuffer)
						 : nullptr;
	auto* nativeIndexMesh = mesh.indexBuffer != BufferHandle::Invalid
								? impl->meshPool.Resolve(mesh.indexBuffer)
								: nullptr;

	if (params.skinnedVertexBuffer != BufferHandle::Invalid) {
		impl->hasSkinnedThisFrame = true;
	}

	auto* finalPosMesh = (params.skinnedVertexBuffer != BufferHandle::Invalid)
							 ? impl->meshPool.Resolve(params.skinnedVertexBuffer)
							 : posMesh;

	uint64_t posAddr = (finalPosMesh != nullptr) ? finalPosMesh->vboAddress : 0;
	uint64_t attrAddr = (attrMesh != nullptr) ? attrMesh->vboAddress : 0;

	if (posMesh == attrMesh && posMesh != nullptr) {
		attrAddr = posMesh->vboAddress + (500000 * sizeof(VertexPosition));
	} else if (params.skinnedVertexBuffer != BufferHandle::Invalid && posMesh != nullptr) {
		attrAddr = finalPosMesh->vboAddress + (posMesh->vertexCount * sizeof(VertexPosition));
	}

	uint32_t isSkinned = (params.skinnedVertexBuffer == BufferHandle::Invalid &&
						  (params.flags & DrawFlags::Skinned) != DrawFlags::None)
							 ? 1u
							 : 0u;
	uint32_t activeMorphCount =
		(params.skinnedVertexBuffer != BufferHandle::Invalid) ? 0 : params.activeMorphCount;

	InstanceData inst = {
		.world = params.transform,
		.prevWorld = params.prevTransform,
		.posAddress = posAddr,
		.attrAddress = attrAddr,
		.skinAddress = (skinMesh != nullptr) ? skinMesh->vboAddress : 0,
		.iboAddress = (nativeIndexMesh != nullptr) ? nativeIndexMesh->vboAddress : 0,
		.vertexCount = (posMesh != nullptr) ? posMesh->vertexCount : 0,
		.indexCount = mesh.indexCount,
		.texIndices0 = (material.normalIndex << 16) | (material.albedoIndex & 0xFFFF),
		.texIndices1 = (material.emissiveIndex << 16) | (material.pbrIndex & 0xFFFF),
		.cullRadius = params.cullRadius,
		.metallicFactor = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
		.roughnessFactor = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
		.alphaCutoff = material.alphaCutoff,
		.flags = (isSkinned << 8) | (material.alphaMode & 0xFF),
		.jointOffset = params.jointOffset,
		.morphOffset = params.morphOffset,
		.activeMorphCount = activeMorphCount,
		.localCenter = {params.localCenter[0], params.localCenter[1], params.localCenter[2]},
		._paddingCenter = {},
		.morphWeights = UnpackMorphWeights(params.morphWeights),
		.baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1],
							material.baseColorFactor[2], material.baseColorFactor[3]},
		.emissiveFactor = {material.emissiveFactor[0], material.emissiveFactor[1],
						   material.emissiveFactor[2], material.emissiveFactor[3]},
	};

	impl->drawQueue.push_back(DrawCommand{.instanceData = inst,
										  .material = const_cast<NativeMaterial*>(nativeMaterial),
										  .posMesh = posMesh,
										  .attrMesh = attrMesh,
										  .skinMesh = skinMesh,
										  .skinnedVertexBuffer = params.skinnedVertexBuffer,
										  .jointOffset = params.jointOffset,
										  .morphOffset = params.morphOffset,
										  .activeMorphCount = params.activeMorphCount,
										  .morphWeights = UnpackMorphWeights(params.morphWeights),
										  .flags = params.flags});
}

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex, bool useScissor,
			ScissorRect scissorRect) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	auto* posMesh = impl->meshPool.Resolve(mesh.posBuffer);
	auto* attrMesh = impl->meshPool.Resolve(mesh.attrBuffer);
	if (AnyNull(posMesh, attrMesh)) [[unlikely]] {
		return;
	}

	impl->uiDrawQueue.push_back({.posMesh = posMesh,
								 .attrMesh = attrMesh,
								 .fontIndex = fontIndex,
								 .useScissor = useScissor,
								 .scissorRect = scissorRect});
}

} // namespace Renderer
} // namespace ZHLN
