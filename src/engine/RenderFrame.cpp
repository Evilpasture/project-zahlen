// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Profiler.hpp"
#include "detail/RadixSort.hpp"
#include "detail/Reflection.hpp"
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
	using enum RenderFrameResult;
	switch (res) {
		case ZHLN_FrameResult_Ok:
			return Success;
		case ZHLN_FrameResult_Suboptimal:
			return Suboptimal;
		case ZHLN_FrameResult_OutOfDate:
			return OutOfDate;
		case ZHLN_FrameResult_DeviceLost:
			return DeviceLost;
		default:
			return Error;
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

template <typename TargetImageT, typename RecordFn>
inline void DispatchPostProcessPass(VkCommandBuffer cmd, TargetImageT& targetImage,
									VkAttachmentLoadOp loadOp, RecordFn&& recordFn,
									VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
									const Color4& clearColor = kClearColorBlack) noexcept {
	auto attachment = Vk::AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(targetImage);
	Vk::DynamicPass(targetImage.extent)
		.AddColor(attachment, loadOp, storeOp, clearColor)
		.Execute(cmd, std::forward<RecordFn>(recordFn));
}

template <typename PassT, typename PC, typename... WriteArgs>
void RunAAWritePass(VkCommandBuffer c, VkDevice device, RenderContext::Impl& self, PassT& pass,
					PC&& pushConstants, WriteArgs&&... writeArgs) noexcept {
	DispatchPostProcessPass(c, self.accumBuffers.Next(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
		pass.WriteNext(device, std::forward<WriteArgs>(writeArgs)...);
		pass.Execute(c, std::forward<PC>(pushConstants));
	});
}

struct TaskSystemSchedulerAdapter {
	void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
		TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
	}
};
enum class RenderPassType : uint8_t { Main, Shadow };

template <typename T>
inline void SubmitDrawInstanced(VkCommandBuffer cmd, const DrawCommand& drawCmd,
								uint32_t instanceIdx, VkDescriptorSet bindlessSet,
								const T& pushConstants,
								VkPipeline pipelineOverride = VK_NULL_HANDLE,
								VkPipelineLayout layoutOverride = VK_NULL_HANDLE,
								VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
															VK_SHADER_STAGE_FRAGMENT_BIT) noexcept {
	const auto* const nativeMat = drawCmd.material;
	const auto* const pipeline =
		(pipelineOverride != VK_NULL_HANDLE) ? pipelineOverride : nativeMat->pipeline.Get();
	const auto* const layout =
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
// RenderContext Infrastructure & Lifecycles
// ============================================================================

void RenderContext::Impl::SortDrawQueue() {
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	if (drawCount == 0) {
		return;
	}

	sortItemsScratch.resize(drawCount);
	sortTempScratch.resize(drawCount);
	sortDrawQueueScratch.resize(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		sortItemsScratch[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].posMesh),
							   .payload = i};
	}

	RadixSort64(sortItemsScratch.data(), sortTempScratch.data(), drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		sortDrawQueueScratch[i] = drawQueue[sortItemsScratch[i].payload];
	}

	// Copy elements back. Because drawQueue already has the capacity, this performs a raw data copy
	// with zero allocations.
	drawQueue = sortDrawQueueScratch;
}

std::optional<Extent2D> RenderContext::GetFramebufferSize() const {
	Extent2D size = _impl->window.GetSize();
	if (size.width == 0 || size.height == 0) {
		return std::nullopt;
	}
	return size;
}

void RenderContext::Impl::DispatchSkinningPasses() {
	if (!hasSkinnedThisFrame) {
		return;
	}

	ZHLN_PROFILE_SCOPE("GPU Compute Skinning");
	auto* const cmd = current_cmd;
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
				.jointsAddr = Vk::GetBufferDeviceAddress(ctx.Device(), jointBuffers->Handle()),
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

void RenderContext::Impl::BuildTLAS(VkCommandBuffer cmd) noexcept {
	if (!rtCtx.Valid() || drawQueue.empty()) {
		return;
	}

	tlasInstancesScratch.clear();
	tlasInstancesScratch.reserve(drawQueue.size());

	using enum DrawFlags;

	for (uint32_t i = 0; i < drawQueue.size(); ++i) {
		const auto& drawCmd = drawQueue[i];
		auto* mesh = drawCmd.posMesh;

		if (mesh == nullptr || mesh->blasAddress == 0 || ((drawCmd.flags & Skinned) != None) ||
			((drawCmd.flags & ExcludeFromTLAS) != None)) {
			continue;
		}

		const auto& t = drawCmd.instanceData.world;

		VkAccelerationStructureInstanceKHR inst{
			.transform =
				[&]() {
					VkTransformMatrixKHR m;
					for (int row = 0; row < 3; ++row) {
						for (int col = 0; col < 4; ++col) {
							m.matrix[row][col] = t(row, col);
						}
					}
					return m;
				}(),
			.instanceCustomIndex = i,
			.mask = 0xFF,
			.instanceShaderBindingTableRecordOffset = 0,
			.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
			.accelerationStructureReference = mesh->blasAddress};

		tlasInstancesScratch.push_back(inst);
	}

	if (tlasInstancesScratch.empty()) {
		return;
	}

	auto& stagingBuf = tlasStagingBuffers[];
	auto& instanceBuf = tlasInstanceBuffers[];

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

	rtCtx.CmdBuildTlas(cmd, geom, tlas[],
					   Vk::GetBufferDeviceAddress(ctx.Device(), tlasScratchBuffer->Handle()),
					   tlasInstancesScratch.size());

	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
							.src_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
							.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
							.dst_access = VK_ACCESS_2_SHADER_READ_BIT});
}

// ============================================================================
// Zero-Copy Pass Construction Factory (Refactored)
// ============================================================================
namespace {
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
			return self.rtCtx.Valid() ? &self.tlas.Current() : VK_NULL_HANDLE;
		}
	}

	// Helper to resolve the correct color target based on fullbright configuration
	template <bool FullBright> [[nodiscard]] auto& ColorTarget() const noexcept {
		if constexpr (FullBright) {
			return self.graphResources.sceneColor;
		} else {
			return self.graphResources.postProcessTarget;
		}
	}

	// Consolidated helper to build scene resource descriptions
	[[nodiscard]] auto BuildSceneResources() const noexcept {
		return SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>{
			.sceneColor = AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				self.graphResources.sceneColor),
			.velocity = AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				self.graphResources.velocityBuffer),
			.normRough = AssumeLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				self.graphResources.normalRoughnessBuffer),
			.depth = AssumeLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
				self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)};
	}

	[[nodiscard]] auto MakeMainPass() const noexcept {
		return Vk::Passieren<"Main", Vk::ColorWrite<Res_SceneColor>, Vk::ColorWrite<Res_Velocity>,
							 Vk::ColorWrite<Res_NormRough>, Vk::DepthWrite<Res_Depth>>(
			[this](VkCommandBuffer c) noexcept {
				FrameRecorder mainRec(c, self);
				Passes::MainPass{}.Execute(mainRec, BuildSceneResources());
			});
	}

	[[nodiscard]] auto MakeShadowPass() const noexcept {
		return Vk::Passieren<"MainShadow", Vk::ColorWrite<Res_SceneColor>,
							 Vk::ColorWrite<Res_Velocity>, Vk::ColorWrite<Res_NormRough>,
							 Vk::DepthWrite<Res_Depth>, Vk::DepthWrite<Res_ShadowMap>,
							 Vk::DepthWrite<Res_ShadowAtlas>>([this](VkCommandBuffer c) noexcept {
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
					Passes::MainPass{}.Execute(mainRec, BuildSceneResources());
				});
			Vk::ExecuteCommands(c, rec.GetCommandBuffers());
		});
	}

	[[nodiscard]] auto MakeAmbientPass() const noexcept {
		return Vk::MakePass<"Ambient", Vk::ShaderRead<Res_SceneColor>, Vk::ShaderRead<Res_NormRough>,
							Vk::ShaderRead<Res_Depth>, Vk::ColorWrite<Res_Ambient>>(
			[this](auto& ctx) noexcept {
				self.ambientPass.WriteNext(
					device,
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.sceneColor),
					self.defaultSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.normalRoughnessBuffer),
					self.pointSampler.Get(), self.iblPayload.prefilteredView.Get(),
					self.iblPayload.brdfLutView.Get(), self.clampSampler.Get(),
					self.frameUniformBuffers->Handle());
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
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.sceneColor),
					self.defaultSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.normalRoughnessBuffer),
					self.lightStorageBuffers->Handle(), self.frameUniformBuffers->Handle(),
					self.graphResources.shadowMap.view.Get(), self.shadowSampler.Get(),
					self.ltcMatView.Get(), self.ltcAmpView.Get(), self.clampSampler.Get(),
					self.clusterGridBuffers->Handle(), self.lightIndexListBuffers->Handle(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.ambientTarget),
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
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.sceneColor),
					self.defaultSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.normalRoughnessBuffer),
					self.pointSampler.Get(), self.iblPayload.prefilteredView.Get(), GetTLAS(),
					self.frameUniformBuffers->Handle(), self.iblPayload.brdfLutView.Get(),
					self.clampSampler.Get(),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.lightingTarget));
				self.reflectionPass.ExecuteVariant(ctx.Cmd(), reflVariant, pc);
			});
	}

	template <bool FullBright> [[nodiscard]] auto MakeForwardPass() const noexcept {
		using ColorTargetRes = std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>;
		auto& targetImage = ColorTarget<FullBright>();

		return Vk::Passieren<"Forward", Vk::ColorWrite<ColorTargetRes>, Vk::DepthWrite<Res_Depth>>(
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
		const auto& inputColor = ColorTarget<FullBright>();
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

	// Downsample version (single source)
	template <typename SrcImgT, typename PassT>
	static void RunKawasePass(VkDevice device, VkCommandBuffer cmd, PassT& pass, const SrcImgT& src,
							  const Vk::Sampler& defaultSampler) noexcept {
		pass.WriteNext(device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(src),
					   defaultSampler.Get(), Vk::SkipWrite{});
		pass.Execute(cmd, RenderContext::Impl::KawasePushConstants{
							  .mode = 0,
							  .rcpWidth = 1.0f / (float)src.extent.width,
							  .rcpHeight = 1.0f / (float)src.extent.height,
							  .padding = 0.0f});
	}

	// Upsample version (dual sources)
	template <typename SrcImgT, typename SrcImg2T, typename PassT>
	static void RunKawasePass(VkDevice device, VkCommandBuffer cmd, PassT& pass, const SrcImgT& src,
							  const Vk::Sampler& defaultSampler, const SrcImg2T& src2) noexcept {
		pass.WriteNext(device, Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(src),
					   defaultSampler.Get(),
					   Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(src2));
		pass.Execute(cmd, RenderContext::Impl::KawasePushConstants{
							  .mode = 1,
							  .rcpWidth = 1.0f / (float)src.extent.width,
							  .rcpHeight = 1.0f / (float)src.extent.height,
							  .padding = 0.0f});
	}

	template <size_t Index> [[nodiscard]] auto MakeBloomDownPass() const noexcept {
		if constexpr (Index == 0) {
			return Vk::MakePass<"BloomDown0", Vk::ShaderRead<Res_BloomThresh>,
								Vk::ColorWrite<Res_BloomDown1>>([this](auto& ctx) noexcept {
				RunKawasePass(device, ctx.Cmd(), self.bloomDownPass[0],
							  self.graphResources.bloomThresholdTarget, self.defaultSampler);
			});
		} else if constexpr (Index == 1) {
			return Vk::MakePass<"BloomDown1", Vk::ShaderRead<Res_BloomDown1>,
								Vk::ColorWrite<Res_BloomDown2>>([this](auto& ctx) noexcept {
				RunKawasePass(device, ctx.Cmd(), self.bloomDownPass[1],
							  self.graphResources.bloomDown1, self.defaultSampler);
			});
		} else {
			return Vk::MakePass<"BloomDown2", Vk::ShaderRead<Res_BloomDown2>,
								Vk::ColorWrite<Res_BloomDown3>>([this](auto& ctx) noexcept {
				RunKawasePass(device, ctx.Cmd(), self.bloomDownPass[2],
							  self.graphResources.bloomDown2, self.defaultSampler);
			});
		}
	}

	template <size_t Index> [[nodiscard]] auto MakeBloomUpPass() const noexcept {
		if constexpr (Index == 2) {
			return Vk::MakePass<"BloomUp2", Vk::ShaderRead<Res_BloomDown3>,
								Vk::ShaderRead<Res_BloomDown2>, Vk::ColorWrite<Res_BloomUp2>>(
				[this](auto& ctx) noexcept {
					RunKawasePass(device, ctx.Cmd(), self.bloomUpPass[2],
								  self.graphResources.bloomDown3, self.defaultSampler,
								  self.graphResources.bloomDown2);
				});
		} else if constexpr (Index == 1) {
			return Vk::MakePass<"BloomUp1", Vk::ShaderRead<Res_BloomUp2>,
								Vk::ShaderRead<Res_BloomDown1>, Vk::ColorWrite<Res_BloomUp1>>(
				[this](auto& ctx) noexcept {
					RunKawasePass(device, ctx.Cmd(), self.bloomUpPass[1],
								  self.graphResources.bloomUp2, self.defaultSampler,
								  self.graphResources.bloomDown1);
				});
		} else {
			return Vk::MakePass<"BloomUp0", Vk::ShaderRead<Res_BloomUp1>,
								Vk::ShaderRead<Res_BloomThresh>, Vk::ColorWrite<Res_BloomFinal>>(
				[this](auto& ctx) noexcept {
					RunKawasePass(device, ctx.Cmd(), self.bloomUpPass[0],
								  self.graphResources.bloomUp1, self.defaultSampler,
								  self.graphResources.bloomThresholdTarget);
				});
		}
	}

	template <bool FullBright, AAMode Mode, typename AALambdaT>
	auto MakeAAPass(AALambdaT&& aaLambda) const noexcept {
		using AAColorInputRes = std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>;
		auto& aaColorInputImage = ColorTarget<FullBright>();

		return Vk::Passieren<"AA", Vk::ShaderRead<AAColorInputRes>, Vk::ShaderRead<Res_Velocity>,
							 Vk::ShaderRead<Res_NormRough>, Vk::ShaderRead<Res_Depth>,
							 Vk::ColorWrite<Res_AccumNext>, Vk::ShaderRead<Res_SmaaEdge>,
							 Vk::ShaderRead<Res_SmaaWeight>, Vk::ShaderRead<Res_AccumCurr>>(
			[aaLambda = std::forward<AALambdaT>(aaLambda),
			 &aaColorInputImage](VkCommandBuffer c) noexcept { aaLambda(c, aaColorInputImage); });
	}

	template <bool FullBright, AAMode Mode, typename GetSwapchainImageT>
	auto MakeBlitPass(GetSwapchainImageT&& getSwapchainImage) const noexcept {
		using enum AAMode;
		using BlitInputRes =
			std::conditional_t<Mode != None, Res_AccumNext,
							   std::conditional_t<FullBright, Res_SceneColor, Res_PostProcess>>;
		const auto& blitInputImage = [&]() -> auto& {
			if constexpr (Mode != None) {
				return self.accumBuffers.Next();
			} else {
				return ColorTarget<FullBright>();
			}
		}();

		return Vk::Passieren<"Blit", Vk::ShaderRead<BlitInputRes>, Vk::ShaderRead<Res_BloomFinal>,
							 Vk::ColorWrite<Res_Swapchain>>(
			[this, &blitInputImage,
			 getSwapchainImage =
				 std::forward<GetSwapchainImageT>(getSwapchainImage)](VkCommandBuffer c) noexcept {
				FrameRecorder blitRecorder(c, self);
				Passes::BlitPass{}.Execute(
					blitRecorder,
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(blitInputImage),
					Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
						self.graphResources.bloomFinalTarget),
					getSwapchainImage(), FullBright ? 1 : 0);
			});
	}
};

// ============================================================================
// Multi-Axis Frame Graph Generator
// ============================================================================
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

	// Sequence the multi-scale Dual Kawase Downsample / Upsample chain
	auto bloomPasses = std::tuple{factory.template MakeBloomThresholdPass<FullBright>(),
								  factory.template MakeBloomDownPass<0>(),
								  factory.template MakeBloomDownPass<1>(),
								  factory.template MakeBloomDownPass<2>(),
								  factory.template MakeBloomUpPass<2>(),
								  factory.template MakeBloomUpPass<1>(),
								  factory.template MakeBloomUpPass<0>()};

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
	using Resources = typename decltype(graph)::Resources;
	using GraphRes = RenderContext::Impl::GraphResources;
	using Meta = GraphRes::ReflectMetadata;

	// Automatically discover, match, and bind matching graph resources from self.graphResources
	Reflect::ForEachReflectedField<Meta>(self.graphResources, [&]<typename Tag>(auto& image) {
		if constexpr (Vk::IsInList<Resources, Tag>::value) {
			auto ref = Vk::MakeRef<Tag>(image);
			binder.template Bind<Tag>(ref.handle, ref.view, ref.extent);
		}
	});

	// Genuine exceptions — not representable as a plain stored field, handled explicitly:
	if constexpr (Vk::IsInList<Resources, Res_Depth>::value) {
		auto ref = Vk::MakeRef<Res_Depth>(self.presentation.depthTarget);
		binder.template Bind<Res_Depth>(ref.handle, ref.view, ref.extent);
	}
	if constexpr (Vk::IsInList<Resources, Res_AccumCurr>::value) {
		auto ref = Vk::MakeRef<Res_AccumCurr>(self.accumBuffers.Current());
		binder.template Bind<Res_AccumCurr>(ref.handle, ref.view, ref.extent);
	}
	if constexpr (Vk::IsInList<Resources, Res_AccumNext>::value) {
		auto ref = Vk::MakeRef<Res_AccumNext>(self.accumBuffers.Next());
		binder.template Bind<Res_AccumNext>(ref.handle, ref.view, ref.extent);
	}
	if constexpr (Vk::IsInList<Resources, Res_Swapchain>::value) {
		const auto& sc = self.presentation.swapchain.Get();
		auto ref = Vk::MakeRef<Res_Swapchain>(sc.images[self.current_image_index],
											  sc.views[self.current_image_index],
											  self.graphResources.sceneColor.extent);
		binder.template Bind<Res_Swapchain>(ref.handle, ref.view, ref.extent);
	}

	graph.Execute(cmd, binder);
}

template <bool FullBright, typename Self, typename... Args>
void DispatchAAMode(Self& self, VkCommandBuffer cmd, AAMode mode, Args&&... args) {
	Reflect::DispatchEnum(mode, [&]<AAMode Val>() {
		ExecuteFrameGraph<FullBright, Val>(self, cmd, std::forward<Args>(args)...);
	});
}
} // namespace

consteval std::string_view GetRenderGraphDump() noexcept {
	auto dummyAA = [](VkCommandBuffer, const auto&) noexcept {};
	auto dummySwap = []() noexcept -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
		return {};
	};

	// Deduce the graph structure via template instantiation without running any code
	using DummyGraphT = decltype(BuildFrameGraph<false, AAMode::TAA>(std::declval<PassFactory>(),
																	 dummyAA, dummySwap));

	// Bake the resulting string directly into the executable's read-only memory
	static constexpr auto viz = Vk::Debug::GraphVisualizer<DummyGraphT>::Visualize();
	return viz.string_view();
}

template <bool FullBright>
void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd) {
	uint32_t imageIdx = current_image_index;
	VkDevice device = ctx.Device();
	uint32_t fIdx = frame_index;

	using namespace ZHLN::Vk;
	using enum AAMode;
	FrameRecorder recorder(cmd, *this);

	// Safe builder for explicitly typed swapchain images
	auto getSwapchainImage = [&]() -> Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
		return {.handle = presentation.swapchain.Get().images[imageIdx],
				.view = presentation.swapchain.Get().views[imageIdx],
				.extent = graphResources.sceneColor.extent,
				.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
				.format = presentation.swapchain.Get().format};
	};

	auto RcpExtent = [] [[nodiscard]] (VkExtent2D e) noexcept -> std::pair<float, float> {
		return {1.0f / (float)e.width, 1.0f / (float)e.height};
	};

	auto aaLambda = [&]([[maybe_unused]] VkCommandBuffer c, const auto& inputColor) noexcept {
		Profiler::ScopedGpuProfile<Stages::AAPass, FrameProfiler> timer(c, fIdx, gpuProfiler);
		const auto smaaEdge_ro =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(graphResources.smaaEdgeTarget);
		const auto smaaWeight_ro =
			AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(graphResources.smaaWeightTarget);

		if (aaState.mode == TAA && taaPass.pipeline.Valid()) {
			struct TAAPushConstants {
				float feedback;
			};
			RunAAWritePass(
				c, device, *this, taaPass, TAAPushConstants{.feedback = aaState.taaFeedback},
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(accumBuffers.Current()),
				AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
					graphResources.velocityBuffer),
				defaultSampler.Get(), frameUniformBuffers[fIdx].Handle());

		} else if (aaState.mode == FXAA && fxaaPass.pipeline.Valid()) {
			auto [rcpW, rcpH] = RcpExtent(inputColor.extent);
			struct FXAAPushConstants {
				float rcpFrameX;
				float rcpFrameY;
				float subpix;
				float edgeThreshold;
				float edgeThresholdMin;
				float _pad;
			};
			RunAAWritePass(c, device, *this, fxaaPass,
						   FXAAPushConstants{rcpW, rcpH, aaState.fxaaSubpix,
											 aaState.fxaaEdgeThreshold,
											 aaState.fxaaEdgeThresholdMin, 0.0f},
						   AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
						   defaultSampler.Get());

		} else if (aaState.mode == SMAA && smaaEdgePass.pipeline.Valid()) {
			auto [rcpW, rcpH] = RcpExtent(inputColor.extent);
			struct SMAAMetrics {
				float rcpWidth;
				float rcpHeight;
				float width;
				float height;
			} metrics = {rcpW, rcpH, (float)inputColor.extent.width,
						 (float)inputColor.extent.height};

			Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				c, graphResources.smaaEdgeTarget.image.Handle());
			DispatchPostProcessPass(
				c, graphResources.smaaEdgeTarget, VK_ATTACHMENT_LOAD_OP_CLEAR, [&]() {
					smaaEdgePass.WriteNext(
						device,
						Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
						defaultSampler.Get(), pointSampler.Get());
					smaaEdgePass.Execute(c, metrics,
										 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				});
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				c, graphResources.smaaEdgeTarget.image.Handle());
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				c, graphResources.smaaWeightTarget.image.Handle());

			DispatchPostProcessPass(
				c, graphResources.smaaWeightTarget, VK_ATTACHMENT_LOAD_OP_CLEAR, [&]() {
					const auto& [areaView, searchView] =
						std::tie(textureViews[smaaAreaTexIdx], textureViews[smaaSearchTexIdx]);

					smaaWeightPass.WriteNext(device, smaaEdge_ro, areaView, searchView,
											 defaultSampler.Get(), pointSampler.Get());
					smaaWeightPass.Execute(
						c, metrics, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
				});
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				c, graphResources.smaaWeightTarget.image.Handle());

			DispatchPostProcessPass(c, accumBuffers.Next(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, [&]() {
				smaaBlendPass.WriteNext(device, inputColor, smaaWeight_ro, defaultSampler.Get(),
										pointSampler.Get());
				smaaBlendPass.Execute(c, metrics,
									  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			});
		} else {
			RunAAWritePass(c, device, *this, blitPass,
						   BlitPushConstants{
							   .vignetteIntensity = 0.0f, .vignettePower = 0.0f, .fullBright = 0},
						   AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor),
						   defaultSampler.Get(),
						   AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(inputColor));
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

	// Runtime enum mapped to compile-time template parameter dispatch
	DispatchAAMode<FullBright>(*this, cmd, aaState.mode, factory, aaLambda, getSwapchainImage);
}

template void
	RenderContext::Impl::RecordSceneFrame<true>(Vk::CommandBuffer<Vk::QueueType::Graphics>);
template void
	RenderContext::Impl::RecordSceneFrame<false>(Vk::CommandBuffer<Vk::QueueType::Graphics>);

RenderResult RenderContext::BeginFrame() noexcept {
	using enum RenderFrameResult;
	if (_impl->stagingContext) {
		_impl->stagingContext->Wait();
		_impl->stagingContext.reset();
	}

	_impl->deletionQueue.BeginFrame(_impl->frame_index);
	_impl->activeQueueGuard.emplace(_impl->deletionQueue);

	for (auto& worker : _impl->workerCmds) {
		worker.cmdCount[_impl->frame_index].store(0, std::memory_order::relaxed);
		worker.pools[_impl->frame_index].Reset();
	}

	if (_impl->resized) {
		auto fbSize = GetFramebufferSize();
		if (!fbSize.has_value()) {
			return std::unexpected(OutOfDate);
		}

		VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

		if (!_impl->RecreateTargets(ext)) {
			return std::unexpected(Error);
		}

		_impl->needsInitialClear = true;
		_impl->resized = false;
	}

	// Set a non-null placeholder value to indicate that frame tracking is active
	_impl->current_cmd = reinterpret_cast<VkCommandBuffer>(1);

	return {};
}

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

	using enum RenderFrameResult;

	ZHLN_FrameResult res = ZHLN_FrameResult_Ok;

	// 1. Isolate the active CPU recording work in an inner block to exclude GPU/VSync blocking times
	{
		ZHLN_PROFILE_SCOPE("Render (CPU Record)");
		if (_impl->current_cmd == VK_NULL_HANDLE) {
			_impl->drawQueue.clear();
			_impl->uiDrawQueue.clear();
			return std::unexpected(Error);
		}

		res = Vk::DrawFrame<2, false>(
			{.ctx = _impl->ctx,
			 .swapchain = _impl->presentation.swapchain,
			 .sync = _impl->sync,
			 .pools = _impl->pools,
			 .presentSemaphores = _impl->presentation.presentSemaphores,
			 .stagingSemaphore = _impl->transferRingBuffer.GetSemaphore(),
			 .stagingWaitValue = _impl->transferRingBuffer.GetCurrentValue()},
			_impl->frame_index,
			[this](VkCommandBuffer cmd, uint32_t image_index) {
				_impl->current_cmd = cmd;
				_impl->current_image_index = image_index;

				// Safe Synchronized Query Readback & Reset
				float timestampPeriod =
					_impl->ctx.PhysicalInfo().properties.properties.limits.timestampPeriod;
				_impl->gpuProfiler.RetrieveResults(_impl->frame_index, timestampPeriod,
												   [](std::string_view name, float durationMS) {
													   CPUProfiler::Record(name, durationMS);
												   });
				_impl->gpuProfiler.Reset(_impl->frame_index);

				_impl->pendingAcquires.Drain(cmd);

				_impl->DispatchSkinningPasses();

				if (_impl->drawQueue.size() > kGpuCullingMaxInstances) {
					_impl->drawQueue.resize(kGpuCullingMaxInstances);
				}

				_impl->SortDrawQueue();

				auto drawCount = _impl->drawQueue.size();
				if (drawCount > 0) {
					auto mapped = _impl->instanceDataBuffers->Map();
					auto* dst = static_cast<InstanceData*>(mapped.data);
					for (size_t i = 0; i < drawCount; ++i) {
						dst[i] = _impl->drawQueue[i].instanceData;
					}
				}
				_impl->BuildTLAS(cmd);

				if (_impl->currentUniforms.fullBright != 0) {
					_impl->RecordSceneFrame<true>({cmd});
				} else {
					_impl->RecordSceneFrame<false>({cmd});
				}
			},
			[this]() { _impl->resized = true; });

		if (res != ZHLN_FrameResult_Ok && res != ZHLN_FrameResult_Suboptimal) {
			_impl->drawQueue.clear();
			_impl->uiDrawQueue.clear();
			_impl->current_cmd = VK_NULL_HANDLE;
			_impl->hasSkinnedThisFrame = false;
			return std::unexpected(MapFrameResult(res));
		}

		// Flip double-buffered resources to prepare for the next frame
		ZHLN::Reflect::ForEachField(*_impl, [](auto& field) { FlipObject(field); });

		_impl->drawQueue.clear();
		_impl->uiDrawQueue.clear();
		_impl->current_cmd = VK_NULL_HANDLE;
		_impl->hasSkinnedThisFrame = false;
	} // <-- The profile timer is destroyed here, committing only the actual recording work

	// 2. Perform the blocking GPU synchronization outside of the CPU Record timer
	auto wait_res = Vk::SyncFrameBoundary(_impl->ctx, _impl->sync, _impl->frame_index);
	if (wait_res == VK_ERROR_DEVICE_LOST) {
		return std::unexpected(DeviceLost);
	}

	if (res == ZHLN_FrameResult_Suboptimal) {
		return std::unexpected(Suboptimal);
	}

	return {};
}

void RenderContext::Impl::ProvokeDeviceLostInternal() const {
	if (!hangGpuPass.pipeline.Valid()) {
		return;
	}

	if (current_cmd != VK_NULL_HANDLE) {
		hangGpuPass.Bind(current_cmd);
		hangGpuPass.Dispatch(current_cmd, 512, 512, 1);
	} else {
		// Grab from pre-allocated ring and execute blocking CPU stall until GPU page faults
		Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](auto cmd) {
			hangGpuPass.Bind(cmd);
			hangGpuPass.Dispatch(cmd, 512, 512, 1);
		});
	}
}

namespace Renderer {

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh, const DrawParams& params) {
	using enum DrawFlags;
	using enum BufferHandle;
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

	auto* skinMesh = mesh.skinBuffer != Invalid ? impl->meshPool.Resolve(mesh.skinBuffer) : nullptr;
	auto* nativeIndexMesh =
		mesh.indexBuffer != Invalid ? impl->meshPool.Resolve(mesh.indexBuffer) : nullptr;

	if (params.skinnedVertexBuffer != Invalid) {
		impl->hasSkinnedThisFrame = true;
	}

	auto* finalPosMesh = (params.skinnedVertexBuffer != Invalid)
							 ? impl->meshPool.Resolve(params.skinnedVertexBuffer)
							 : posMesh;

	uint64_t posAddr = (finalPosMesh != nullptr) ? finalPosMesh->vboAddress : 0;
	uint64_t attrAddr = (attrMesh != nullptr) ? attrMesh->vboAddress : 0;

	if (posMesh == attrMesh && posMesh != nullptr) {
		attrAddr = posMesh->vboAddress + (500000 * sizeof(VertexPosition));
	} else if (params.skinnedVertexBuffer != Invalid && posMesh != nullptr) {
		attrAddr = finalPosMesh->vboAddress + (posMesh->vertexCount * sizeof(VertexPosition));
	}

	uint32_t isSkinned =
		(params.skinnedVertexBuffer == Invalid && (params.flags & Skinned) != None) ? 1u : 0u;
	uint32_t activeMorphCount =
		(params.skinnedVertexBuffer != Invalid) ? 0 : params.activeMorphCount;

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
