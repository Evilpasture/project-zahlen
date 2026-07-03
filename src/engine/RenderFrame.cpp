// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "RenderInternal.hpp"
#include "Zahlen/Profiler.hpp"
#include "detail/RadixSort.hpp"

#include <array>
#include <cstring>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

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

/**
 * @brief Checks if any of the provided pointers are null using compile-time fold expressions.
 */
template <typename... Ptrs> [[nodiscard]] constexpr bool AnyNull(Ptrs... ptrs) noexcept {
	return (... || (ptrs == nullptr));
}

/**
 * @brief Inline barrier wrapper for the compute-to-vertex synchronization boundary.
 */
inline void BarrierComputeWriteToVertexRead(VkCommandBuffer cmd) {
	Vk::BarrierBuilder()
		.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
		.To(cmd, Vk::BarrierStage::Vertex, Vk::BarrierAccess::ShaderRead);
}

/**
 * @brief Inline barrier wrapper for the cluster-culling internal sync.
 */
inline void BarrierClusterCullingSync(VkCommandBuffer cmd) {
	Vk::BarrierBuilder()
		.From(Vk::BarrierStage::Transfer | Vk::BarrierStage::Compute,
			  Vk::BarrierAccess::TransferWrite | Vk::BarrierAccess::ShaderWrite)
		.To(cmd, Vk::BarrierStage::Compute,
			Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite);
}

/**
 * @brief Inline barrier wrapper for compute-write to fragment-read synchronization.
 */
inline void BarrierComputeWriteToFragmentRead(VkCommandBuffer cmd) {
	Vk::BarrierBuilder()
		.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
		.To(cmd, Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead);
}

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

void RenderContext::Impl::InitialClearTargets(VkCommandBuffer cmd) noexcept {
	// Scope-guarded transitions for depth/stencil buffers (reverts to AsReadOnly in destructor)
	Vk::ScopedTransition sShadowMap(cmd, shadowMap, Vk::AsDepthAttachment);
	Vk::ScopedTransition sShadowAtlas(cmd, shadowAtlas, Vk::AsDepthAttachment);
	Vk::ScopedTransition depth(cmd, presentation.depthTarget, Vk::AsDepthAttachment);

	// Re-map main G-Buffer color attachments
	auto mainBundle = Vk::TieTargets(sceneColor, velocityBuffer, accumBuffers[0], accumBuffers[1],
									 normalRoughnessBuffer);
	auto mainAtts = mainBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);

	Vk::DynamicPass(presentation.swapchain.Get().extent)
		.AddColorGroup(mainAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					   {.r = 0, .g = 0, .b = 0, .a = 0})
		.AddDepth(depth.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
				  kClearDepthValue)
		.Execute(cmd, []() {});

	auto mainRo = TransitionAllTo<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, mainAtts);

	// Modernized clear pipelines for pure color groups
	[[maybe_unused]] auto ppRo = ClearAndPrepareGroup(
		cmd, presentation.swapchain.Get().extent, kClearColorBlack, postProcessTarget,
		ambientTarget, lightingTarget, smaaEdgeTarget, smaaWeightTarget);

	[[maybe_unused]] auto bloomRo =
		ClearAndPrepareGroup(cmd, bloomThresholdTarget.extent, kClearColorBlack,
							 bloomThresholdTarget, bloomBlurTarget, bloomFinalTarget);

	for (int i = 0; i < 2; ++i) {
		taaPass.WriteIndex(ctx.Device(), i, std::get<0>(mainRo),
						   i == 0 ? std::get<3>(mainRo) : std::get<2>(mainRo), std::get<1>(mainRo),
						   defaultSampler.Get(), frameUniformBuffers[i].Handle());
	}
}

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

	BarrierComputeWriteToVertexRead(cmd);
}

RenderResult RenderContext::BeginFrame() noexcept {
	if (_impl->stagingContext) {
		_impl->stagingContext->Wait();
		_impl->stagingContext.reset();
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

	if (_impl->needsInitialClear) {
		_impl->InitialClearTargets(_impl->current_cmd);
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

	StaticResourceManager(
		&accumBuffers, &taaPass, &fxaaPass, &smaaEdgePass, &smaaWeightPass, &smaaBlendPass,
		&ambientPass, &lightingPass, &reflectionPass, &blitPass, &bloomThresholdPass,
		&bloomBlurHPass, &bloomBlurVPass, &frameUniformBuffers, &lightStorageBuffers,
		&instanceDataBuffers, &indirectCommandsBuffers, &shadowIndirectBuffers, &jointBuffers,
		&bindlessSets, &tlas, &tlasBuffer, &tlasScratchBuffer, &clusterGridBuffers,
		&lightIndexListBuffers, &globalCounterBuffers, &clusterCullingSets)
		.FlipAll();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
	hasSkinnedThisFrame = false;
	return res;
}

// ============================================================================
// InstanceData & TLAS Assembly Operations
// ============================================================================

void RenderContext::Impl::BuildTLAS(VkCommandBuffer cmd) noexcept {
	if (!rtCtx.Valid() || drawQueue.empty()) {
		return;
	}

	// Persistent, reusable memory block to prevent dynamic heap reallocations
	tlasInstancesScratch.clear();
	tlasInstancesScratch.reserve(drawQueue.size());

	for (uint32_t i = 0; i < drawQueue.size(); ++i) {
		const auto& drawCmd = drawQueue[i];
		auto* mesh = drawCmd.posMesh;

		if (mesh == nullptr) {
			continue;
		}

		bool isSkinned = (drawCmd.flags & DrawFlags::Skinned) != DrawFlags::None;
		bool isExcluded = (drawCmd.flags & DrawFlags::ExcludeFromTLAS) != DrawFlags::None;

		if (mesh->blasAddress == 0 || isSkinned || isExcluded) {
			continue;
		}

		VkAccelerationStructureInstanceKHR inst{};

		// Unpacked fields: Read directly from pre-assembled InstanceData to omit duplicate matrices
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

	ZHLN::Assert(tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR) <=
				 stagingBuf.Size());
	std::memcpy(stagingBuf.Map().data, tlasInstancesScratch.data(),
				tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

	ZHLN_BufferCopyDesc copy = {.src = stagingBuf.Handle(),
								.dst = instanceBuf.Handle(),
								.size = tlasInstancesScratch.size() *
										sizeof(VkAccelerationStructureInstanceKHR),
								.src_offset = 0,
								.dst_offset = 0};
	ZHLN_CmdCopyBuffer(cmd, &copy);

	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
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
// Compile-Time Scene Recording Dispatches
// ============================================================================
namespace {
struct TLASBuildStep {
	void Execute(RenderContext::Impl* impl, VkCommandBuffer cmd, const FrameRecorder& /*unused*/,
				 RenderContext::Impl::RenderState& /*unused*/) const noexcept {
		impl->BuildTLAS(cmd);
	}
};

struct ClusterCullingStep {
	void Execute(RenderContext::Impl* impl, VkCommandBuffer cmd, const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& /*unused*/) const noexcept {
		uint32_t fIdx = impl->frame_index;
		impl->clusterBoundsPass.Dispatch(cmd, impl->clusterCullingSets[fIdx], 1, 1, 24);
		Vk::FillBuffer(cmd, impl->globalCounterBuffers[fIdx]);
		BarrierClusterCullingSync(cmd);
		impl->clusterCullingPass.Dispatch(cmd, impl->clusterCullingSets[fIdx], 1, 1, 24);
		BarrierComputeWriteToFragmentRead(cmd);
		RunPass(Passes::ShadowPass{}, recorder);
	}
};

struct SetupInitialStateStep {
	void Execute(RenderContext::Impl* impl, VkCommandBuffer /*unused*/,
				 const FrameRecorder& /*unused*/,
				 RenderContext::Impl::RenderState& state) const noexcept {
		state.initialState = {
			.sceneColor =
				Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(impl->sceneColor),
			.velocity =
				Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(impl->velocityBuffer),
			.normRough = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				impl->normalRoughnessBuffer),
			.depth = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				impl->presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)};
	}
};

struct MainPassStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& state) const noexcept {
		Passes::MainPass{}.Execute(recorder, state.initialState);
	}
};

struct LightingPassStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& state) const noexcept {
		state.finalColor = Passes::DeferredLightingPass{}.Execute(recorder, state.initialState);
	}
};

struct FullBrightSetupStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& /*unused*/,
				 RenderContext::Impl::RenderState& state) const noexcept {
		state.finalColor = state.initialState.sceneColor;
	}
};

struct ForwardPassStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& state) const noexcept {
		Passes::ForwardPass{}.Execute(recorder, state.finalColor, state.initialState.depth);
	}
};

struct BloomPassStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& state) const noexcept {
		state.bloomFinal = Passes::BloomPass{}.Execute(recorder, state.finalColor);
	}
};

struct FullBrightBloomStep {
	void Execute(RenderContext::Impl* impl, VkCommandBuffer /*unused*/,
				 const FrameRecorder& /*unused*/,
				 RenderContext::Impl::RenderState& state) const noexcept {
		state.bloomFinal =
			Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(impl->bloomFinalTarget);
	}
};

struct AAPassStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& state) const noexcept {
		state.resourcesForAA = {.sceneColor = state.finalColor,
								.velocity = state.initialState.velocity,
								.normRough = state.initialState.normRough,
								.depth = state.initialState.depth};
		state.aaResult = Passes::AAPass{}.Execute(recorder, state.resourcesForAA);
	}
};

struct BlitPassStep {
	void Execute(RenderContext::Impl* /*unused*/, VkCommandBuffer /*unused*/,
				 const FrameRecorder& recorder,
				 RenderContext::Impl::RenderState& state) const noexcept {
		Passes::BlitPass{}.Execute(recorder, state.aaResult.sceneColor, state.bloomFinal);
	}
};
} // namespace

template <bool FullBright> [[nodiscard]] consteval auto GetPassSteps() noexcept {
	if constexpr (FullBright) {
		return std::make_tuple(SetupInitialStateStep{}, MainPassStep{}, FullBrightSetupStep{},
							   ForwardPassStep{}, FullBrightBloomStep{}, AAPassStep{},
							   BlitPassStep{});
	} else {
		return std::make_tuple(TLASBuildStep{}, ClusterCullingStep{}, SetupInitialStateStep{},
							   MainPassStep{}, LightingPassStep{}, ForwardPassStep{},
							   BloomPassStep{}, AAPassStep{}, BlitPassStep{});
	}
}

template <bool FullBright> void RenderContext::Impl::RecordSceneFrame(VkCommandBuffer cmd) {
	FrameRecorder recorder(cmd, *this);
	RenderState state{};

	constexpr auto steps = GetPassSteps<FullBright>();

	// Unpack and execute the flat rendering timeline sequentially
	std::apply([&](auto&&... step) { (step.Execute(this, cmd, recorder, state), ...); }, steps);
}

// Explicit instantiations to prevent translation unit linkage issues
template void RenderContext::Impl::RecordSceneFrame<true>(VkCommandBuffer);
template void RenderContext::Impl::RecordSceneFrame<false>(VkCommandBuffer);

RenderResult RenderContext::EndFrame() noexcept {
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

		// Zero-overhead linear copy of pre-assembled GPU structures (pointer-chasing eliminated)
		for (uint32_t i = 0; i < drawCount; ++i) {
			dst[i] = _impl->drawQueue[i].instanceData;
		}
	}

	// Hot-path branch optimization: dispatch using compile-time constants
	if (_impl->currentUniforms.fullBright != 0) {
		_impl->RecordSceneFrame<true>(cmd);
	} else {
		_impl->RecordSceneFrame<false>(cmd);
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
		ZHLN::Log("ERROR: Cannot provoke device lost because the Hang GPU pipeline is invalid. "
				  "Check your build.");
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

	// Resolve the correct positioning target for skinned versus non-skinned draw streams
	auto* finalPosMesh = (params.skinnedVertexBuffer != BufferHandle::Invalid)
							 ? impl->meshPool.Resolve(params.skinnedVertexBuffer)
							 : posMesh;

	uint64_t posAddr = (finalPosMesh != nullptr) ? finalPosMesh->vboAddress : 0;
	uint64_t attrAddr = (attrMesh != nullptr) ? attrMesh->vboAddress : 0;

	// Safely offset the attribute layout bounds using static array increments
	if (posMesh == attrMesh && posMesh != nullptr) {
		attrAddr = posMesh->vboAddress + (500000 * sizeof(VertexPosition));
	} else if (params.skinnedVertexBuffer != BufferHandle::Invalid && posMesh != nullptr) {
		attrAddr = finalPosMesh->vboAddress + (posMesh->vertexCount * sizeof(VertexPosition));
	}

	uint32_t isSkinned = (params.skinnedVertexBuffer == BufferHandle::Invalid &&
						  (params.flags & DrawFlags::Skinned) != DrawFlags::None)
							 ? 1u
							 : 0u;

	uint32_t activeMorphCount = params.activeMorphCount;
	if (params.skinnedVertexBuffer != BufferHandle::Invalid) {
		activeMorphCount = 0; // Displacement is evaluated on GPU pre-skinning
	}

	// Pre-assemble the GPU structure directly on the calling thread
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
										  .material = nativeMaterial,
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
