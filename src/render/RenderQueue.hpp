#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif
namespace ZHLN::Vk {
// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierStage : VkPipelineStageFlags2 {
	None = 0,
	Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	Fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
	Vertex = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
	Indirect = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
	Transfer = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	Host = VK_PIPELINE_STAGE_2_HOST_BIT
};

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierAccess : VkAccessFlags2 {
	None = 0,
	ShaderRead = VK_ACCESS_2_SHADER_READ_BIT,
	ShaderWrite = VK_ACCESS_2_SHADER_WRITE_BIT,
	IndirectRead = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
	TransferRead = VK_ACCESS_2_TRANSFER_READ_BIT,
	TransferWrite = VK_ACCESS_2_TRANSFER_WRITE_BIT,
	HostRead = VK_ACCESS_2_HOST_READ_BIT,
	HostWrite = VK_ACCESS_2_HOST_WRITE_BIT,
	ColorRead = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
	ColorWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	DepthRead = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
	DepthWrite = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
};

// Enable bitwise OR operations on the scoped enums
[[nodiscard]] constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage {
	return static_cast<BarrierStage>(static_cast<std::underlying_type_t<BarrierStage>>(a) |
									 static_cast<std::underlying_type_t<BarrierStage>>(b));
}

[[nodiscard]] constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess {
	return static_cast<BarrierAccess>(static_cast<std::underlying_type_t<BarrierAccess>>(a) |
									  static_cast<std::underlying_type_t<BarrierAccess>>(b));
}

struct BarrierBuilder {
	VkPipelineStageFlags2 src_stage = 0;
	VkAccessFlags2 src_access = 0;

	constexpr BarrierBuilder() noexcept = default;

	constexpr auto From(BarrierStage stage, BarrierAccess access) noexcept -> BarrierBuilder& {
		src_stage |= static_cast<VkPipelineStageFlags2>(stage);
		src_access |= static_cast<VkAccessFlags2>(access);
		return *this;
	}

	void To(VkCommandBuffer cmd, BarrierStage dst_stage, BarrierAccess dst_access) const noexcept {
		const ZHLN_MemoryBarrierDesc desc = {.src_stage = src_stage,
											 .src_access = src_access,
											 .dst_stage =
												 static_cast<VkPipelineStageFlags2>(dst_stage),
											 .dst_access = static_cast<VkAccessFlags2>(dst_access)};
		ZHLN_CmdMemoryBarrier(cmd, &desc);
	}
};
enum class QueueType : uint8_t { Graphics, Compute, Transfer };

// Primary templates (default to invalid/false)
template <QueueType Queue, BarrierStage Stage> struct IsStageValid : std::false_type {};

template <QueueType Queue, BarrierAccess Access> struct IsAccessValid : std::false_type {};

// --- 1. GRAPHICS QUEUE: Supports all stages and accesses ---
template <BarrierStage Stage> struct IsStageValid<QueueType::Graphics, Stage> : std::true_type {};

template <BarrierAccess Access>
struct IsAccessValid<QueueType::Graphics, Access> : std::true_type {};

// --- 2. COMPUTE QUEUE: Supports Compute, Transfer, and Host ---
template <> struct IsStageValid<QueueType::Compute, BarrierStage::None> : std::true_type {};
template <> struct IsStageValid<QueueType::Compute, BarrierStage::Compute> : std::true_type {};
template <> struct IsStageValid<QueueType::Compute, BarrierStage::Transfer> : std::true_type {};
template <> struct IsStageValid<QueueType::Compute, BarrierStage::Host> : std::true_type {};

template <BarrierAccess Access>
struct IsAccessValid<QueueType::Compute, Access>
	: std::bool_constant<(static_cast<VkAccessFlags2>(Access) &
						  ~(static_cast<VkAccessFlags2>(BarrierAccess::ShaderRead) |
							static_cast<VkAccessFlags2>(BarrierAccess::ShaderWrite) |
							static_cast<VkAccessFlags2>(BarrierAccess::TransferRead) |
							static_cast<VkAccessFlags2>(BarrierAccess::TransferWrite) |
							static_cast<VkAccessFlags2>(BarrierAccess::HostRead) |
							static_cast<VkAccessFlags2>(BarrierAccess::HostWrite))) == 0> {};

// --- 3. TRANSFER QUEUE: Only supports Transfer (Copy/Clear) ---
template <> struct IsStageValid<QueueType::Transfer, BarrierStage::None> : std::true_type {};
template <> struct IsStageValid<QueueType::Transfer, BarrierStage::Transfer> : std::true_type {};

template <BarrierAccess Access>
struct IsAccessValid<QueueType::Transfer, Access>
	: std::bool_constant<(static_cast<VkAccessFlags2>(Access) &
						  ~(static_cast<VkAccessFlags2>(BarrierAccess::TransferRead) |
							static_cast<VkAccessFlags2>(BarrierAccess::TransferWrite))) == 0> {};

template <QueueType Queue, BarrierStage Stage, BarrierAccess Access>
concept ValidQueueOperation =
	IsStageValid<Queue, Stage>::value && IsAccessValid<Queue, Access>::value;

template <QueueType QType> struct CommandBuffer {
	VkCommandBuffer handle = VK_NULL_HANDLE;
	static constexpr QueueType queue_type = QType;

	// Implicit conversion to raw handle for driver API calls
	operator VkCommandBuffer() const noexcept { return handle; }
	[[nodiscard]] bool Valid() const noexcept { return handle != VK_NULL_HANDLE; }
};

template <QueueType QType, BarrierStage SrcStage, BarrierAccess SrcAccess>
	requires ValidQueueOperation<QType, SrcStage, SrcAccess>
struct ConstrainedBarrier {
	CommandBuffer<QType> cmd;

	template <BarrierStage DstStage, BarrierAccess DstAccess>
		requires ValidQueueOperation<QType, DstStage, DstAccess>
	void TransitionTo() const noexcept {
		const ZHLN_MemoryBarrierDesc desc = {
			.src_stage = static_cast<VkPipelineStageFlags2>(SrcStage),
			.src_access = static_cast<VkAccessFlags2>(SrcAccess),
			.dst_stage = static_cast<VkPipelineStageFlags2>(DstStage),
			.dst_access = static_cast<VkAccessFlags2>(DstAccess)};
		ZHLN_CmdMemoryBarrier(cmd.handle, &desc);
	}
};

// Fluent helper function to start a barrier
template <BarrierStage SrcStage, BarrierAccess SrcAccess, QueueType QType>
[[nodiscard]] constexpr auto BeginBarrier(CommandBuffer<QType> cmd) noexcept {
	return ConstrainedBarrier<QType, SrcStage, SrcAccess>{cmd};
}
} // namespace ZHLN::Vk
