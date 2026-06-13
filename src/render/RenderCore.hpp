// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderCore.h"

#include <array>
#include <bit>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <source_location>
#include <span>
#include <type_traits>

namespace ZHLN {

struct Color4 {
	float r, g, b, a;
};

// NOLINTBEGIN(misc-misplaced-const, readability-avoid-const-params-in-decls)

/**
 * @brief Thread-safe-ish ping-pong buffer for temporal rendering state.
 */
template <typename T> class DoubleBuffered {
  public:
	DoubleBuffered() = default;

	[[nodiscard]] auto Current() noexcept -> T&;
	[[nodiscard]] auto Current() const noexcept -> const T&;

	[[nodiscard]] auto Next() noexcept -> T&;
	[[nodiscard]] auto Next() const noexcept -> const T&;

	[[nodiscard]] auto operator[](uint32_t idx) noexcept -> T&;
	[[nodiscard]] auto operator[](uint32_t idx) const noexcept -> const T&;

	void Flip() noexcept;

  private:
	std::array<T, 2> _data{};
	uint32_t _index = 0;
};

namespace Detail {
template <bool Condition> struct ResourceCheck {
	[[gnu::warning(
		"A static resource lived in the manager but lacks a Flip() method! It will be skipped.")]]
	static void emit() noexcept {}
};

template <> struct ResourceCheck<true> {
	static constexpr void emit() noexcept {}
};
} // namespace Detail

/**
 * @brief 100% Compile-Time Static Dispatch Resource Manager.
 * Zero heap allocations, zero indirection, and perfect inlining.
 */
template <typename... Resources> class StaticResourceManager {
  public:
	constexpr explicit StaticResourceManager(Resources*... resources) noexcept
		: _resources(resources...) {}

	void FlipAll() noexcept {
		std::apply(
			[](auto*... r) {
				// Define a helper lambda that checks for the Flip method at compile time
				auto flip_single = [](auto* resource) {
					// 1. Evaluate the constraint into a compile-time constant
					constexpr bool can_flip = requires { resource->Flip(); };

					// 2. Pass it to your custom warning diagnostic mechanism
					Detail::ResourceCheck<can_flip>::emit();

					// 3. Conditionally compile the execution code
					if constexpr (can_flip) {
						resource->Flip();
					}
				};

				// Expand the fold expression using our conditional helper
				(flip_single(r), ...);
			},
			_resources);
	}

  private:
	std::tuple<Resources*...> _resources;
};

} // namespace ZHLN

namespace ZHLN::Vk {

/**
 * @brief Zero-overhead, compile-time layout tracker.
 * Memory footprint is identical to passing raw handles, but the compiler enforces state.
 */
template <VkImageLayout Layout> struct TypedImage {
	static constexpr VkImageLayout layout = Layout;

	VkImage handle = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkExtent2D extent{};
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

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
// RAII Handles
// ============================================================================

template <typename T, auto DeleterFn> class Handle {
  public:
	Handle() noexcept = default;
	explicit Handle(T raw) noexcept;
	~Handle() noexcept;

	Handle(const Handle&) = delete;
	auto operator=(const Handle&) -> Handle& = delete;

	Handle(Handle&& other) noexcept;
	auto operator=(Handle&& other) noexcept -> Handle&;

	[[nodiscard]] auto Get() const noexcept -> T;
	[[nodiscard]] auto Valid() const noexcept -> bool;
	explicit operator bool() const noexcept;
	[[nodiscard]] auto Release() noexcept -> T;

  private:
	T _raw = VK_NULL_HANDLE;
};

template <typename T, auto DeleterFn> class DeviceHandle {
  public:
	DeviceHandle() noexcept = default;
	DeviceHandle(const VkDevice device, const T raw) noexcept;
	~DeviceHandle() noexcept;

	DeviceHandle(const DeviceHandle&) = delete;
	auto operator=(const DeviceHandle&) -> DeviceHandle& = delete;

	DeviceHandle(DeviceHandle&& other) noexcept;
	auto operator=(DeviceHandle&& other) noexcept -> DeviceHandle&;

	[[nodiscard]] constexpr auto Get() const noexcept -> T;
	[[nodiscard]] constexpr auto Valid() const noexcept -> bool;
	constexpr explicit operator bool() const noexcept;
	[[nodiscard]] constexpr auto Release() noexcept -> T;

  private:
	VkDevice _device = VK_NULL_HANDLE;
	T _raw = VK_NULL_HANDLE;
};

using ShaderModule = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using Semaphore = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;
using Sampler = DeviceHandle<VkSampler, ZHLN_DestroySampler>;
using DescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, ZHLN_DestroyDescriptorSetLayout>;
using DescriptorPool = DeviceHandle<VkDescriptorPool, ZHLN_DestroyDescriptorPool>;

// ============================================================================
// Context RAII
// ============================================================================

class Context {
  public:
	Context() noexcept = default;
	~Context() noexcept;

	Context(const Context&) = delete;
	auto operator=(const Context&) -> Context& = delete;

	Context(Context&& other) noexcept;
	auto operator=(Context&& other) noexcept -> Context&;

	[[nodiscard(
		"Vulkan context creation may fail; check validity with Valid() or explicit bool cast")]]
	static auto Create(const ZHLN_InstanceDesc& instance_desc,
					   const ZHLN_DeviceSelectDesc& select_desc,
					   const ZHLN_DeviceDesc& device_desc) noexcept -> Context;

	[[nodiscard]] auto Instance() const noexcept -> VkInstance { return _instance; }
	[[nodiscard]] auto Device() const noexcept -> VkDevice { return _device.handle; }
	[[nodiscard]] auto GraphicsQueue() const noexcept -> VkQueue { return _device.graphics_queue; }
	[[nodiscard]] auto PresentQueue() const noexcept -> VkQueue { return _device.present_queue; }
	[[nodiscard]] auto Physical() const noexcept -> VkPhysicalDevice { return _physical.handle; }
	[[nodiscard]] auto PhysicalInfo() const noexcept -> const ZHLN_PhysicalDeviceInfo& {
		return _physical;
	}

	[[nodiscard("Always verify context initialization; check Valid() before use")]]
	auto Valid() const noexcept -> bool {
		return _device.handle != VK_NULL_HANDLE;
	}
	explicit operator bool() const noexcept { return Valid(); }

  private:
	VkInstance _instance = VK_NULL_HANDLE;
	VkSurfaceKHR _surface = VK_NULL_HANDLE;
	ZHLN_PhysicalDeviceInfo _physical = {};
	ZHLN_Device _device = {};
};

// ============================================================================
// Swapchain RAII
// ============================================================================

struct SwapchainSupport {
	ZHLN_SwapchainSupport raw;
	[[nodiscard]] auto Formats() const noexcept -> std::span<const VkSurfaceFormatKHR>;
	[[nodiscard]] auto PresentModes() const noexcept -> std::span<const VkPresentModeKHR>;
};

[[nodiscard]] SwapchainSupport QuerySwapchainSupport(const VkPhysicalDevice physical,
													 const VkSurfaceKHR surface) noexcept;

class Swapchain {
  public:
	Swapchain() noexcept = default;
	Swapchain(const VkDevice device, const ZHLN_Swapchain raw) noexcept;
	~Swapchain() noexcept;

	Swapchain(const Swapchain&) = delete;
	auto operator=(const Swapchain&) -> Swapchain& = delete;

	Swapchain(Swapchain&& other) noexcept;
	auto operator=(Swapchain&& other) noexcept -> Swapchain&;

	[[nodiscard]] constexpr auto Get() const noexcept -> const ZHLN_Swapchain& { return _raw; }
	[[nodiscard("Verify swapchain validity before use")]]
	constexpr auto Valid() const noexcept -> bool {
		return _raw.handle != VK_NULL_HANDLE;
	}
	constexpr explicit operator bool() const noexcept { return Valid(); }

	auto Rebuild(const ZHLN_SwapchainDesc& desc) noexcept -> bool;

  private:
	void Destroy() noexcept;

	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_Swapchain _raw = {};
};

// ============================================================================
// Sync & Pools
// ============================================================================

template <uint32_t N>
	requires(N > 0 && N <= 8)
class FrameSync {
  public:
	FrameSync() noexcept = default;
	~FrameSync() noexcept;

	FrameSync(const FrameSync&) = delete;
	auto operator=(const FrameSync&) -> FrameSync& = delete;

	FrameSync(FrameSync&& other) noexcept;
	auto operator=(FrameSync&& other) noexcept -> FrameSync&;

	[[nodiscard("Frame sync creation may fail; verify validity before use in frame loop")]]
	static auto Create(const VkDevice device) noexcept -> FrameSync;

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) const noexcept
		-> const ZHLN_FrameSync& {
		return _frames[frame % N];
	}
	[[nodiscard]] static constexpr auto Count() noexcept -> uint32_t { return N; }
	[[nodiscard("Check FrameSync validity before use in frame loop")]]
	constexpr auto Valid() const noexcept -> bool {
		return _device != VK_NULL_HANDLE;
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<ZHLN_FrameSync, N> _frames = {};
};

class CommandPool {
  public:
	CommandPool() = default;
	CommandPool(const VkDevice device, const uint32_t queue_family);
	~CommandPool();

	CommandPool(const CommandPool&) = delete;
	auto operator=(const CommandPool&) -> CommandPool& = delete;

	constexpr CommandPool(CommandPool&& other) noexcept;
	auto operator=(CommandPool&& other) noexcept -> CommandPool&;

	[[nodiscard("Verify command pool validity before allocation")]]
	constexpr auto Valid() const noexcept -> bool {
		return _device != VK_NULL_HANDLE;
	}
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] constexpr operator const ZHLN_CommandPool&() const noexcept { return _raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool&() noexcept { return _raw; }

	[[nodiscard("Check allocation success before using command buffers")]]
	auto Allocate(const uint32_t count) -> bool;

	[[nodiscard]] constexpr auto operator[](const uint32_t idx) const noexcept -> VkCommandBuffer {
		return _raw.buffers[idx];
	}

	[[nodiscard("Check allocation success before using secondary command buffers")]]
	auto AllocateSecondary(const uint32_t count) -> bool;

	void Reset() noexcept;

	[[nodiscard]] constexpr operator const ZHLN_CommandPool*() const noexcept { return &_raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool*() noexcept { return &_raw; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_CommandPool _raw{};
};

template <uint32_t N>
	requires(N > 0 && N <= 8)
class CommandPools {
  public:
	struct Description {
		uint32_t queue_family = 0;
		uint32_t buffers_per_pool = 1;
	};

	CommandPools() noexcept = default;

	[[nodiscard("Command pools must be verified before use in command recording")]]
	static auto Create(const VkDevice device, const Description& desc) noexcept -> CommandPools;

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) noexcept -> CommandPool& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) const noexcept
		-> const CommandPool& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto Cmd(const uint32_t frame) const noexcept -> VkCommandBuffer {
		return _pools[frame % N][0];
	}

	[[nodiscard("Verify command pools are valid before frame recording")]]
	constexpr auto Valid() const noexcept -> bool {
		return _pools[0].Valid();
	}

  private:
	std::array<CommandPool, N> _pools = {};
};

// ============================================================================
// ShaderStages RAII
// ============================================================================

[[nodiscard]] constexpr auto CreateShaderDesc(const uint32_t* code, size_t size,
											  const char* entry = nullptr) noexcept
	-> ZHLN_ShaderDesc {
	return ZHLN_ShaderDesc{.code = code, .size = size, .entry_point = entry};
}

class ShaderStages {
  public:
	constexpr ShaderStages() noexcept = default;
	constexpr ShaderStages(const VkDevice device, const ZHLN_ShaderStages raw) noexcept
		: _device(device), _raw(raw) {}

	~ShaderStages() noexcept;
	ShaderStages(const ShaderStages&) = delete;
	auto operator=(const ShaderStages&) -> ShaderStages& = delete;
	ShaderStages(ShaderStages&& other) noexcept;
	auto operator=(ShaderStages&& other) noexcept -> ShaderStages&;

	[[nodiscard("Shader creation may fail; verify validity before binding")]]
	static auto Create(const VkDevice device, const ZHLN_ShaderDesc& vert,
					   const ZHLN_ShaderDesc& frag) noexcept -> ShaderStages;

	[[nodiscard("Shader loading from files may fail; verify validity before use")]]
	static auto FromFiles(VkDevice device, const std::filesystem::path& vert_path,
						  const std::filesystem::path& frag_path, const char* vert_entry = "main",
						  const char* frag_entry = "main") noexcept -> ShaderStages;

	[[nodiscard]] constexpr auto Get() const noexcept -> const ZHLN_ShaderStages* { return &_raw; }
	[[nodiscard("Always verify shader stages are valid before pipeline creation")]]
	constexpr auto Valid() const noexcept -> bool {
		return _raw.vert.handle != VK_NULL_HANDLE;
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_ShaderStages _raw{};
};

[[nodiscard]] constexpr auto AsSpirV(const void* data) noexcept -> const uint32_t* {
	return std::bit_cast<const uint32_t*>(data);
}

/**
 * @brief Holds fully automated, RAII-managed layout handles generated via shader reflection.
 * @note [UNSAFE] This struct is populated by parsing untrusted SPIR-V bytecode at runtime.
 * Incorrect layout assumptions here can lead to undefined behavior, driver hangs, or GPU crashes.
 */
struct UnsafeReflectedLayout {
	PipelineLayout pipelineLayout;
	std::array<DescriptorSetLayout, 4> descriptorSetLayouts;
	uint32_t setLayoutCount = 0;

	// Tracks the exact count of each descriptor type needed by all sets combined
	std::array<uint32_t, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1> descriptorTypeCounts{};

	/**
	 * @brief Unsafely fetches a raw layout handle.
	 * @warning Caller must guarantee setIndex is within shader layout bounds.
	 */
	[[nodiscard]] auto GetSetLayoutUnsafe(uint32_t setIndex = 0) const noexcept
		-> VkDescriptorSetLayout {
		return setIndex < setLayoutCount ? descriptorSetLayouts[setIndex].Get() : VK_NULL_HANDLE;
	}
};

/**
 * @brief Reflection builder that queries SPIR-V bytecode at runtime using SPIRV-Reflect.
 * @note [UNSAFE] Bypasses C++ compile-time type-safety guarantees. Relies entirely on
 * runtime binary parsing. Use only when static layouts cannot be predefined.
 */
class UnsafeReflectedLayoutBuilder {
  public:
	UnsafeReflectedLayoutBuilder() noexcept = default;

	// Non-movable, non-copyable
	UnsafeReflectedLayoutBuilder(UnsafeReflectedLayoutBuilder&&) = delete;
	UnsafeReflectedLayoutBuilder& operator=(UnsafeReflectedLayoutBuilder&&) = delete;
	UnsafeReflectedLayoutBuilder(const UnsafeReflectedLayoutBuilder&) = delete;
	auto operator=(const UnsafeReflectedLayoutBuilder&) -> UnsafeReflectedLayoutBuilder& = delete;
	~UnsafeReflectedLayoutBuilder() noexcept = default;

	/**
	 * @brief Adds a shader bytecode stage to the pipeline parsing queue.
	 * @warning It is undefined behavior if desc contains malformed SPIR-V or invalid bytecode size.
	 */
	void AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept;

	/**
	 * @brief Unsafely parses all registered stages and builds the Vulkan layouts.
	 * @throws Does not throw, but failure to match pipeline state object requirements
	 * later down the line will cause hard validation layer errors.
	 */
	[[nodiscard]] auto BuildUnsafe(VkDevice device) noexcept -> UnsafeReflectedLayout;

  private:
	struct StageData {
		const uint32_t* code = nullptr;
		size_t size = 0;
		VkShaderStageFlags stage = 0;
	};
	std::array<StageData, 5> _stages{};
	uint32_t _stageCount = 0;
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

template <VkImageLayout Layout> struct LayoutTraits;

// Specializations for LayoutTraits defined in the implementation
template <VkImageLayout OldLayout, VkImageLayout NewLayout>
void TransitionLayout(const VkCommandBuffer cmd, const VkImage image,
					  const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept;

// ============================================================================
// Compile-Time Layout State Contract
// ============================================================================

struct UndefinedState {};
struct ColorAttachmentState {};
struct DepthAttachmentState {};
struct ShaderReadState {};
struct PresentState {};

template <typename State> struct LayoutMap;

template <typename InState, typename OutState, typename T>
auto IssueBarrier(VkCommandBuffer cmd, const T& resource,
				  VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE);

template <VkImageLayout NewLayout, VkImageLayout OldLayout>
[[nodiscard]] auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img,
							  VkImageAspectFlags overrideAspect = VK_IMAGE_ASPECT_NONE) noexcept
	-> TypedImage<NewLayout>;

void CopyBufferToImage(const VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc& desc) noexcept;

template <GpuTriviallyCopyable T>
void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout, const VkShaderStageFlags stages,
		  const T& value) noexcept;

// ============================================================================
// Frame Execution
// ============================================================================

template <uint32_t N, typename Record, typename Rebuild>
	requires RecordFn<Record> && RebuildFn<Rebuild>
auto DrawFrame(const Context& ctx, const Swapchain& swapchain, const FrameSync<N>& sync,
			   const CommandPools<N>& pools, uint32_t& frame_index, Record&& record,
			   Rebuild&& rebuild) noexcept -> ZHLN_FrameResult;

[[nodiscard]] auto SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept -> ZHLN_FrameResult;

void ExecuteCommands(const VkCommandBuffer primary,
					 const std::span<const VkCommandBuffer> secondaries) noexcept;

// ============================================================================
// Dynamic Render Pass Builder
// ============================================================================

template <VkImageLayout Layout> struct Tag {};

inline constexpr Tag<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> AsColorAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> AsReadOnly;
inline constexpr Tag<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> AsDepthAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR> AsPresent;

template <VkImageLayout TargetLayout, VkImageLayout OldLayout>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img,
										Tag<TargetLayout> /*unused*/) noexcept;

template <size_t ColorCount = 0, bool HasDepth = false> class DynamicPass {
  public:
	constexpr explicit DynamicPass(VkExtent2D extent) noexcept : _extent(extent) {}

	template <size_t InsideCount, bool InsideDepth>
	constexpr DynamicPass(const DynamicPass<InsideCount, InsideDepth>&& other,
						  std::array<VkRenderingAttachmentInfo, ColorCount>&& colors,
						  VkRenderingAttachmentInfo depth) noexcept;

	template <VkImageLayout Layout>
	constexpr auto
	AddColor(const TypedImage<Layout>& img, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
			 VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			 const ZHLN::Color4& clearColor = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}) noexcept
		-> DynamicPass<ColorCount + 1, HasDepth>;

	template <VkImageLayout Layout>
	constexpr auto AddDepth(const TypedImage<Layout>& img,
							VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
							VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
							float clearVal = 1.0f) noexcept -> DynamicPass<ColorCount, true>;

	constexpr auto Flags(VkRenderingFlags flags) noexcept -> DynamicPass<ColorCount, HasDepth>&;

	template <typename Func> void Execute(VkCommandBuffer cmd, Func&& func) const;

  private:
	template <size_t C, bool D> friend class DynamicPass;

	[[nodiscard]] constexpr auto GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo*;

	VkExtent2D _extent;
	VkRenderingFlags _flags = 0;
	std::array<VkRenderingAttachmentInfo, ColorCount> _colors{};
	VkRenderingAttachmentInfo _depth{};
};

DynamicPass(VkExtent2D) -> DynamicPass<0, false>;

// ============================================================================
// Consolidated Graphics State & Instanced Draw Dispatcher
// ============================================================================

struct DrawState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer vbo = VK_NULL_HANDLE;
	VkBuffer ibo = VK_NULL_HANDLE;
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
	VkBuffer vbo = VK_NULL_HANDLE;
	VkBuffer ibo = VK_NULL_HANDLE;
	VkBuffer argumentBuffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	uint32_t drawCount = 0;
	uint32_t stride = 0;
};

template <GpuTriviallyCopyable T>
void DrawIndirect(VkCommandBuffer cmd, const DrawIndirectState& state, const T& pushConstants,
				  VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
											  VK_SHADER_STAGE_FRAGMENT_BIT);

// ============================================================================
// Surface helpers
// ============================================================================

class Surface {
  public:
	Surface() = default;
	Surface(VkInstance instance, VkSurfaceKHR surface);
	~Surface();

	Surface(const Surface&) = delete;
	auto operator=(const Surface&) -> Surface& = delete;

	Surface(Surface&& other) noexcept;
	auto operator=(Surface&& other) noexcept -> Surface&;

	[[nodiscard]] auto Get() const -> VkSurfaceKHR;

  private:
	VkInstance _instance = VK_NULL_HANDLE;
	VkSurfaceKHR _handle = VK_NULL_HANDLE;
};

// ============================================================================
// Error Helpers
// ============================================================================

void ReportVkError(VkResult result, const char* context, const std::source_location& location);
[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept;

[[nodiscard]] auto ResultString(const VkResult result) noexcept -> const char*;
void CheckResult(const VkResult result, const char* context = "",
				 const std::source_location location = std::source_location::current());

// ============================================================================
// Semaphore Helpers
// ============================================================================

class alignas(64) SemaphorePool {
  public:
	SemaphorePool() noexcept = default;
	~SemaphorePool() noexcept;

	SemaphorePool(const SemaphorePool&) = delete;
	auto operator=(const SemaphorePool&) -> SemaphorePool& = delete;

	SemaphorePool(SemaphorePool&& other) noexcept;
	auto operator=(SemaphorePool&& other) noexcept -> SemaphorePool&;

	void Rebuild(const VkDevice device, const uint32_t count) noexcept;
	[[nodiscard("Semaphore access must be checked for bounds; invalid indices will crash")]]
	auto operator[](const uint32_t index) const noexcept -> VkSemaphore;

	[[nodiscard]] auto Count() const noexcept -> uint32_t;
	[[nodiscard("Verify semaphore pool is initialized before use")]]
	auto Valid() const noexcept -> bool;

  private:
	void Cleanup() noexcept;

	VkDevice _device = VK_NULL_HANDLE;
	uint32_t _count = 0;
	[[maybe_unused]] uint32_t _padding = 0;
	std::array<VkSemaphore, 6> _semaphores = {};
};

// ============================================================================
// Image View Helpers
// ============================================================================

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags;

using ImageView = DeviceHandle<VkImageView, ZHLN_DestroyImageView>;

template <VkFormat F>
[[nodiscard]] auto CreateView(VkDevice device, VkImage image,
							  VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1)
	-> ImageView;

template <VkFormat F>
[[nodiscard]] auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips = 1) -> ImageView;

// ============================================================================
// Extension Query Utilities
// ============================================================================

[[nodiscard]] auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool;
[[nodiscard]] auto IsDeviceExtensionSupported(VkPhysicalDevice physical,
											  std::string_view extension) noexcept -> bool;

// ============================================================================
// Zero-Allocation Render Graph
// ============================================================================

struct PassResource {
	ZHLN_ImageBarrierDesc barrier;
};

using PassRecordFn = void (*)(VkCommandBuffer, const void* userData);

struct PassDesc {
	const char* name = "Unnamed Pass";
	std::span<const PassResource> transitions;
	PassRecordFn record = nullptr;
	const void* pUserData = nullptr;
};

template <size_t MaxStackBarriers = 16>
void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept;

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

#include "RenderCore.inl"
