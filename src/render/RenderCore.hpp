// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN {

struct Color4 {
	float r, g, b, a;
};

// NOLINTBEGIN(misc-misplaced-const, readability-avoid-const-params-in-decls)

template <typename T> struct PerFrame {
	std::array<T, 2> data{};
	uint32_t idx = 0;

	PerFrame() = default;

	constexpr PerFrame(T first, T second) noexcept : data{{std::move(first), std::move(second)}} {}

	// C++23 Zero-Argument Subscript Overload for []
	[[nodiscard]] constexpr T& operator[]() noexcept { return data[idx]; }
	[[nodiscard]] constexpr const T& operator[]() const noexcept { return data[idx]; }

	// Standard Single-Argument Subscript Overload for [i]
	[[nodiscard]] constexpr T& operator[](uint32_t i) noexcept { return data[i % 2]; }
	[[nodiscard]] constexpr const T& operator[](uint32_t i) const noexcept { return data[i % 2]; }

	// Keep existing pointer and helper APIs
	[[nodiscard]] constexpr T& operator*() noexcept { return data[idx]; }
	[[nodiscard]] constexpr const T& operator*() const noexcept { return data[idx]; }
	[[nodiscard]] constexpr T* operator->() noexcept { return &data[idx]; }
	[[nodiscard]] constexpr const T* operator->() const noexcept { return &data[idx]; }
	[[nodiscard]] constexpr T& Current() noexcept { return data[idx]; }
	[[nodiscard]] constexpr const T& Current() const noexcept { return data[idx]; }
	[[nodiscard]] constexpr T& Next() noexcept { return data[idx ^ 1]; }
	[[nodiscard]] constexpr const T& Next() const noexcept { return data[idx ^ 1]; }

	void Advance() noexcept { idx ^= 1; }
	void Flip() noexcept { idx ^= 1; }
};

template <typename T> using DoubleBuffered = PerFrame<T>;

// C++20/C++23 Concepts to evaluate layout capabilities at compile-time
template <typename T>
concept CanFlipDirect = requires(T& t) { t.Flip(); };

template <typename T>
concept CanFlipIterable = requires(T& t) {
	requires !CanFlipDirect<T>;
	t.begin();
	t.end();
	requires requires(typename T::value_type& item) { item.Flip(); };
};

inline void FlipObject(auto& obj) noexcept {
	if constexpr (CanFlipDirect<decltype(obj)>) {
		obj.Flip();
	} else if constexpr (CanFlipIterable<decltype(obj)>) {
		for (auto& item : obj) {
			item.Flip();
		}
	}
}

} // namespace ZHLN

namespace ZHLN::Vk {

// ============================================================================
// TMP / Concepts
// ============================================================================

template <typename T>
concept GpuTriviallyCopyable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template <typename T>
concept RecordFn = std::invocable<T, VkCommandBuffer, uint32_t>;

template <typename T>
concept RebuildFn = std::invocable<T>;

// ============================================================================
// Type safe Pipeline
// ============================================================================

template <size_t ColorCount, bool HasDepth> class TypedPipeline {
  public:
	Pipeline handle;

	TypedPipeline() = default;
	explicit TypedPipeline(Pipeline&& p) noexcept : handle(std::move(p)) {}

	// Allow move assignment from raw legacy Pipeline
	TypedPipeline& operator=(Pipeline&& p) noexcept {
		handle = std::move(p);
		return *this;
	}

	[[nodiscard]] VkPipeline Get() const noexcept { return handle.Get(); }
	[[nodiscard]] bool Valid() const noexcept { return handle.Valid(); }
	explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] Pipeline Release() noexcept { return std::move(handle); }
};

inline constexpr auto& GetBufferDeviceAddress = ZHLN_GetBufferDeviceAddress;

std::expected<VkResult, std::string> WaitIdle(VkDevice device) noexcept;

// ============================================================================
// Scoped RAII Scissor State Guard
// ============================================================================

struct ScopedScissor {
	VkCommandBuffer commandRect;
	VkRect2D resetScissor;

	struct ScissorDesc {
		VkRect2D target;
		VkRect2D fallback;
	};
	ScopedScissor(VkCommandBuffer cmd, const ScissorDesc& desc) noexcept;
	~ScopedScissor() noexcept;

	ScopedScissor(const ScopedScissor&) = delete;
	auto operator=(const ScopedScissor&) -> ScopedScissor& = delete;
	ScopedScissor(ScopedScissor&&) = delete;
	auto operator=(ScopedScissor&&) -> ScopedScissor& = delete;
};

// ============================================================================
// Command & Rendering Helpers
// ============================================================================

class ScopedRendering {
  public:
	ScopedRendering(const VkCommandBuffer cmd, const ZHLN_RenderPassDesc& desc) noexcept;
	~ScopedRendering() noexcept;

	ScopedRendering(ScopedRendering&&) = delete;
	auto operator=(ScopedRendering&&) -> ScopedRendering& = delete;
	ScopedRendering(const ScopedRendering&) = delete;
	auto operator=(const ScopedRendering&) -> ScopedRendering& = delete;

  private:
	VkCommandBuffer _cmd;
};

void ImageBarrier(const VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept;
void MemoryBarrier(const VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc& desc) noexcept;

void CopyBufferToImage(const VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc& desc) noexcept;

template <size_t RegionCount>
[[nodiscard]] constexpr auto
CreateCopyRegions(VkDeviceSize baseOffset, VkDeviceSize regionSize, VkExtent3D extent,
				  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mipLevel = 0,
				  uint32_t baseArrayLayer = 0) noexcept
	-> std::array<VkBufferImageCopy2, RegionCount>;

template <size_t RegionCount>
inline void CopyBufferToImage(VkCommandBuffer cmd, VkBuffer srcBuffer, VkImage dstImage,
							  const std::array<VkBufferImageCopy2, RegionCount>& regions,
							  VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) noexcept;

template <GpuTriviallyCopyable T>
void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout, const VkShaderStageFlags stages,
		  const T& value) noexcept;

// ============================================================================
// Frame Execution
// ============================================================================
class SemaphorePool;

template <uint32_t N> struct DrawFrameDesc {
	const Context& ctx;
	const Swapchain& swapchain;
	const FrameSync<N>& sync;
	const CommandPools<N, QueueType::Graphics>& pools;
	const SemaphorePool& presentSemaphores;
	VkSemaphore stagingSemaphore = VK_NULL_HANDLE;
	uint64_t stagingWaitValue = 0;
};

template <uint32_t N, bool WaitOnFence = true, typename Record, typename Rebuild>
	requires RecordFn<Record> && RebuildFn<Rebuild>
auto DrawFrame(const DrawFrameDesc<N>& desc, uint32_t& frame_index, Record&& record,
			   Rebuild&& rebuild) noexcept -> ZHLN_FrameResult;

[[nodiscard]] auto SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept -> ZHLN_FrameResult;

void SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, VkSemaphore waitSemaphore = VK_NULL_HANDLE,
				   uint64_t waitValue = 0,
				   VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) noexcept;

void ExecuteCommands(const VkCommandBuffer primary,
					 const std::span<const VkCommandBuffer> secondaries) noexcept;

// ============================================================================
// Consolidated Graphics State & Instanced Draw Dispatcher
// ============================================================================

struct DrawState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t instanceCount = 1;
	uint32_t firstVertex = 0;
	uint32_t firstIndex = 0;
	uint32_t firstInstance = 0;
};

template <GpuTriviallyCopyable T>
void DrawInstanced(VkCommandBuffer cmd, const DrawState& state, const T& pushConstants,
				   VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
											   VK_SHADER_STAGE_FRAGMENT_BIT);

struct DrawIndirectState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer argumentBuffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	uint32_t drawCount = 0;
	uint32_t stride = 0;
	VkBuffer countBuffer = VK_NULL_HANDLE; // GPU-driven count buffer
	VkDeviceSize countBufferOffset = 0;	   // Offset inside count buffer
};

struct DrawIndirectCountState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer argumentBuffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkBuffer countBuffer = VK_NULL_HANDLE;
	VkDeviceSize countBufferOffset = 0;
	uint32_t maxDrawCount = 0;
	uint32_t stride = 0;
};

template <GpuTriviallyCopyable T>
void DrawIndirect(VkCommandBuffer cmd, const DrawIndirectState& state, const T& pushConstants,
				  VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
											  VK_SHADER_STAGE_FRAGMENT_BIT);

template <GpuTriviallyCopyable T>
void DrawIndirectCount(VkCommandBuffer cmd, const DrawIndirectCountState& state,
					   const T& pushConstants,
					   VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
												   VK_SHADER_STAGE_FRAGMENT_BIT) noexcept;

// ============================================================================
// Explicit GPU-Driven Draw Indirect Utilities
// ============================================================================

struct DrawIndexedIndirectState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer argumentBuffer =
		VK_NULL_HANDLE;		 // Buffer containing VkDrawIndexedIndirectCommand structs
	VkDeviceSize offset = 0; // Byte offset into argumentBuffer
	uint32_t drawCount = 0;	 // Number of draw commands to execute
	uint32_t stride = 0;	 // Stride (sizeof(VkDrawIndexedIndirectCommand))
};

struct DrawIndexedIndirectCountState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer argumentBuffer =
		VK_NULL_HANDLE;					   // Buffer containing VkDrawIndexedIndirectCommand structs
	VkDeviceSize offset = 0;			   // Byte offset into argumentBuffer
	VkBuffer countBuffer = VK_NULL_HANDLE; // Buffer containing a single uint32_t draw count
	VkDeviceSize countBufferOffset = 0;	   // Byte offset into countBuffer
	uint32_t maxDrawCount = 0;			   // Upper limit to prevent out-of-bound draws
	uint32_t stride = 0;				   // Stride (sizeof(VkDrawIndexedIndirectCommand))
};

template <GpuTriviallyCopyable T>
void DrawIndexedIndirect(VkCommandBuffer cmd, const DrawIndexedIndirectState& state,
						 const T& pushConstants,
						 VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
													 VK_SHADER_STAGE_FRAGMENT_BIT) noexcept;

template <GpuTriviallyCopyable T>
void DrawIndexedIndirectCount(VkCommandBuffer cmd, const DrawIndexedIndirectCountState& state,
							  const T& pushConstants,
							  VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
														  VK_SHADER_STAGE_FRAGMENT_BIT) noexcept;

// ============================================================================
// Error Helpers
// ============================================================================

std::string ReportVkError(VkResult result, const char* context,
						  const std::source_location& location);
[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept;

[[nodiscard]] auto ResultString(const VkResult result) noexcept -> std::string;
std::expected<VkResult, std::string>
CheckResult(const VkResult result, const char* context = "",
			const std::source_location location = std::source_location::current());

// ============================================================================
// Extension Query Utilities
// ============================================================================

[[nodiscard]] auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool;
[[nodiscard]] auto IsDeviceExtensionSupported(VkPhysicalDevice physical,
											  std::string_view extension) noexcept -> bool;

void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ,
			  uint32_t localX, uint32_t localY, uint32_t localZ) noexcept;
void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept;

// ============================================================================
// Mipmapping
// ============================================================================

template <uint32_t Width, uint32_t Height> consteval auto GetMipLevels() noexcept -> uint32_t;

void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width,
					 const uint32_t height);

// NOLINTEND(misc-misplaced-const, readability-avoid-const-params-in-decls)

} // namespace ZHLN::Vk

#include "DynamicRendering.hpp" // Ensure dynamic rendering is layered correctly
#include "RenderCore.inl"
