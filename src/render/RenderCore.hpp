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

/**
 * @brief Thread-safe-ish ping-pong buffer for temporal rendering state.
 */
template <typename T> class DoubleBuffered {
  public:
	DoubleBuffered() = default;

	constexpr DoubleBuffered(T&& first, T&& second) noexcept
		: _data{std::move(first), std::move(second)} {}

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
 * Enforces single-use consumption at compile time via rvalue ref-qualifiers.
 */
template <typename... Resources> class StaticResourceManager {
  public:
	constexpr explicit StaticResourceManager(Resources*... resources) noexcept
		: _resources(resources...) {}
	~StaticResourceManager() = default;
	StaticResourceManager(const StaticResourceManager&) = delete;
	StaticResourceManager(StaticResourceManager&&) = delete;
	StaticResourceManager& operator=(const StaticResourceManager&) = delete;
	StaticResourceManager& operator=(StaticResourceManager&&) = delete;

	void FlipAll() & = delete; // because i'm dumb and i want the compiler to protect me

	void FlipAll() && noexcept {
		std::apply(
			[](auto*... r) {
				auto flip_single = [](auto* resource) {
					constexpr bool can_flip = requires { resource->Flip(); };
					Detail::ResourceCheck<can_flip>::emit();
					if constexpr (can_flip) {
						resource->Flip();
					}
				};
				(flip_single(r), ...);
			},
			_resources);
	}

  private:
	std::tuple<Resources*...> _resources;
};

} // namespace ZHLN

namespace ZHLN::Vk {

static constexpr VkCommandBufferInheritanceInfo NullInheritanceInfo = {
	.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
	.pNext = nullptr,
	.renderPass = VK_NULL_HANDLE, // Ignored in Dynamic Rendering
	.subpass = 0,
	.framebuffer = VK_NULL_HANDLE,
	.occlusionQueryEnable = VK_FALSE,
	.queryFlags = 0,
	.pipelineStatistics = 0};

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

	VkFormat format = VK_FORMAT_UNDEFINED; // Compatibility for 1.1
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

// NOTE: RAII handles which call destructors from C. Inlinable with LTO.
using ShaderModule = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using Semaphore = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;
using Sampler = DeviceHandle<VkSampler, ZHLN_DestroySampler>;
using DescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, ZHLN_DestroyDescriptorSetLayout>;
using DescriptorPool = DeviceHandle<VkDescriptorPool, ZHLN_DestroyDescriptorPool>;

// ============================================================================
// VMA RAII Handles
// ============================================================================

class DeletionQueue;
extern thread_local DeletionQueue* t_activeDeletionQueue;

// Overloaded C-helpers to decouple VmaHandle from DeletionQueue definition
void DeferVmaDestruction(VmaAllocator allocator, VkBuffer buffer,
						 VmaAllocation allocation) noexcept;
void DeferVmaDestruction(VmaAllocator allocator, VkImage image, VmaAllocation allocation) noexcept;

inline void VmaUnmapDeleter(VmaAllocator allocator, [[maybe_unused]] void* ptr,
							VmaAllocation allocation) noexcept {
	if (allocator != nullptr && allocation != nullptr) {
		vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
		vmaUnmapMemory(allocator, allocation);
	}
}

template <typename T, auto DeleterFn> class VmaHandle {
  public:
	VmaHandle() noexcept = default;
	VmaHandle(VmaAllocator allocator, T handle, VmaAllocation allocation) noexcept
		: _allocator(allocator), _handle(handle), _allocation(allocation) {}
	~VmaHandle() noexcept { Cleanup(); }

	VmaHandle(const VmaHandle&) = delete;
	auto operator=(const VmaHandle&) -> VmaHandle& = delete;

	VmaHandle(VmaHandle&& other) noexcept
		: _allocator(std::exchange(other._allocator, nullptr)),
		  _handle(std::exchange(other._handle, T{})),
		  _allocation(std::exchange(other._allocation, nullptr)) {}

	auto operator=(VmaHandle&& other) noexcept -> VmaHandle& {
		if (this != &other) {
			Cleanup();
			_allocator = std::exchange(other._allocator, nullptr);
			_handle = std::exchange(other._handle, T{});
			_allocation = std::exchange(other._allocation, nullptr);
		}
		return *this;
	}

	[[nodiscard]] auto Get() const noexcept -> T { return _handle; }
	[[nodiscard]] auto Allocation() const noexcept -> VmaAllocation { return _allocation; }
	[[nodiscard]] auto Allocator() const noexcept -> VmaAllocator { return _allocator; }
	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle != T{}; }
	explicit operator bool() const noexcept { return Valid(); }

	void Cleanup() noexcept {
		if (_handle != T{}) {
			if (ZHLN::Vk::t_activeDeletionQueue != nullptr) {
				if constexpr (std::is_same_v<T, VkBuffer>) {
					DeferVmaDestruction(_allocator, _handle, _allocation);
				} else if constexpr (std::is_same_v<T, VkImage>) {
					DeferVmaDestruction(_allocator, _handle, _allocation);
				} else {
					DeleterFn(_allocator, _handle, _allocation);
				}
			} else {
				DeleterFn(_allocator, _handle, _allocation);
			}
			_handle = T{};
			_allocation = nullptr;
			_allocator = nullptr;
		}
	}

  private:
	VmaAllocator _allocator = nullptr;
	T _handle = T{};
	VmaAllocation _allocation = nullptr;
};

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
	[[nodiscard(
		"Vulkan context creation may fail; check validity with Valid() or explicit bool cast")]]
	static auto Create(VkInstance instance, VkSurfaceKHR surface,
					   const ZHLN_PhysicalDeviceInfo& physical,
					   const ZHLN_DeviceDesc& device_desc) noexcept -> Context;

	[[nodiscard]] auto Instance() const noexcept -> VkInstance { return _instance; }
	[[nodiscard]] auto Device() const noexcept -> VkDevice { return _device.handle; }
	[[nodiscard]] auto GraphicsQueue() const noexcept -> VkQueue { return _device.graphics_queue; }
	[[nodiscard]] auto PresentQueue() const noexcept -> VkQueue { return _device.present_queue; }
	[[nodiscard]] auto TransferQueue() const noexcept -> VkQueue { return _device.transfer_queue; }
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

template <Vk::QueueType QType> class CommandPool {
  public:
	CommandPool() = default;
	CommandPool(const VkDevice device, const uint32_t queue_family);
	~CommandPool();

	CommandPool(const CommandPool&) = delete;
	auto operator=(const CommandPool&) -> CommandPool& = delete;

	constexpr CommandPool(CommandPool&& other) noexcept;
	auto operator=(CommandPool&& other) noexcept -> CommandPool&;

	[[nodiscard]] constexpr auto Valid() const noexcept -> bool {
		return _device != VK_NULL_HANDLE;
	}
	constexpr explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] constexpr operator const ZHLN_CommandPool&() const noexcept { return _raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool&() noexcept { return _raw; }

	[[nodiscard]] auto Allocate(const uint32_t count) -> bool;
	[[nodiscard]] auto AllocateSecondary(const uint32_t count) -> bool;
	void Reset() noexcept;

	// This is where the compiler-enforced safety is introduced!
	[[nodiscard]] constexpr auto operator[](const uint32_t idx) const noexcept
		-> Vk::CommandBuffer<QType> {
		return Vk::CommandBuffer<QType>{_raw.buffers[idx]};
	}

	[[nodiscard]] constexpr operator const ZHLN_CommandPool*() const noexcept { return &_raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool*() noexcept { return &_raw; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_CommandPool _raw{};
};

template <typename... Args> CommandPool(Args&&...) -> CommandPool<QueueType::Graphics>;

template <uint32_t N, Vk::QueueType QType = Vk::QueueType::Graphics>
	requires(N > 0 && N <= 8)
class CommandPools {
  public:
	struct Description {
		uint32_t queue_family = 0;
		uint32_t buffers_per_pool = 1;
	};

	CommandPools() noexcept = default;

	[[nodiscard]] static auto Create(const VkDevice device, const Description& desc) noexcept
		-> CommandPools;

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) noexcept -> CommandPool<QType>& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) const noexcept
		-> const CommandPool<QType>& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto Cmd(const uint32_t frame) const noexcept
		-> Vk::CommandBuffer<QType> {
		return _pools[frame % N][0];
	}

	[[nodiscard]] constexpr auto Valid() const noexcept -> bool { return _pools[0].Valid(); }

  private:
	std::array<CommandPool<QType>, N> _pools = {};
};

// ============================================================================
// ShaderStages RAII
// ============================================================================

[[nodiscard]] constexpr auto CreateShaderDesc(const uint32_t* code, size_t size,
											  const char* entry = nullptr) noexcept
	-> ZHLN_ShaderDesc {
	return ZHLN_ShaderDesc{.code = code, .size = size, .entry_point = entry};
}

template <typename T, size_t Extent>
[[nodiscard]] constexpr auto CreateShaderDesc(std::span<T, Extent> codeSpan,
											  const char* entry = nullptr) noexcept
	-> ZHLN_ShaderDesc {
	return ZHLN_ShaderDesc{.code = std::bit_cast<const uint32_t*>(codeSpan.data()),
						   .size = codeSpan.size_bytes(),
						   .entry_point = entry};
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

	template <typename T, size_t Extent1, typename U = const uint8_t,
			  size_t Extent2 = std::dynamic_extent>
	[[nodiscard]] static auto Create(const VkDevice device, std::span<T, Extent1> vertSpan,
									 std::span<U, Extent2> fragSpan = {},
									 const char* vertEntry = nullptr,
									 const char* fragEntry = nullptr) noexcept -> ShaderStages {
		return Create(device, CreateShaderDesc(vertSpan, vertEntry),
					  fragSpan.empty() ? ZHLN_ShaderDesc{} : CreateShaderDesc(fragSpan, fragEntry));
	}

	template <typename T>
	[[nodiscard]] static auto Create(const VkDevice device, const T& pair,
									 const char* vertEntry = nullptr,
									 const char* fragEntry = nullptr) noexcept -> ShaderStages {
		return Create(device, CreateShaderDesc(pair.vertex, vertEntry),
					  pair.fragment.empty() ? ZHLN_ShaderDesc{}
											: CreateShaderDesc(pair.fragment, fragEntry));
	}

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
// Scoped RAII Scissor State Guard
// ============================================================================

struct ScopedScissor {
	VkCommandBuffer commandRect;
	VkRect2D resetScissor;

	struct ScissorDesc {
		VkRect2D target;
		VkRect2D fallback;
	};
	ScopedScissor(VkCommandBuffer cmd, const ScissorDesc& desc) noexcept
		: commandRect(cmd), resetScissor(desc.fallback) {
		vkCmdSetScissor(commandRect, 0, 1, &desc.target);
	}

	~ScopedScissor() noexcept { vkCmdSetScissor(commandRect, 0, 1, &resetScissor); }

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

[[nodiscard]] auto GetBufferDeviceAddress(VkDevice device, VkBuffer buffer) noexcept
	-> VkDeviceAddress;

class RayTracingContext {
  public:
	RayTracingContext() = default;

	[[nodiscard]] bool Init(VkDevice device) noexcept;
	[[nodiscard]] bool Valid() const noexcept { return _raw.device != VK_NULL_HANDLE; }

	void GetBlasSizes(const ZHLN_BlasGeometryDesc& desc, uint32_t primCount,
					  ZHLN_AccelerationStructureSizes& outSizes) const noexcept;
	void GetTlasSizes(uint32_t instanceCount,
					  ZHLN_AccelerationStructureSizes& outSizes) const noexcept;

	[[nodiscard]] VkAccelerationStructureKHR
	CreateAS(VkBuffer buffer, VkDeviceSize size,
			 ZHLN_AccelerationStructureType type) const noexcept;
	void DestroyAS(VkAccelerationStructureKHR as) const noexcept;
	[[nodiscard]] VkDeviceAddress GetASAddress(VkAccelerationStructureKHR as) const noexcept;

	void CmdBuildBlas(VkCommandBuffer cmd, const ZHLN_BlasGeometryDesc& desc,
					  VkAccelerationStructureKHR dst, VkDeviceAddress scratch,
					  uint32_t primCount) const noexcept;
	void CmdBuildTlas(VkCommandBuffer cmd, const ZHLN_TlasGeometryDesc& desc,
					  VkAccelerationStructureKHR dst, VkDeviceAddress scratch,
					  uint32_t instanceCount) const noexcept;

  private:
	ZHLN_RayTracingContext _raw{};
};

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

template <> struct LayoutMap<UndefinedState> {
	static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_UNDEFINED;
};
template <> struct LayoutMap<ColorAttachmentState> {
	static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};
template <> struct LayoutMap<DepthAttachmentState> {
	static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
};
template <> struct LayoutMap<ShaderReadState> {
	static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
template <> struct LayoutMap<PresentState> {
	static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

template <typename InState, typename OutState, typename T>
auto IssueBarrier(VkCommandBuffer cmd, const T& resource,
				  VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE);

template <VkImageLayout NewLayout, VkImageLayout OldLayout>
[[nodiscard]] auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img,
							  VkImageAspectFlags overrideAspect = VK_IMAGE_ASPECT_NONE) noexcept
	-> TypedImage<NewLayout>;

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
// Scoped RAII Layout Transition Guards
// ============================================================================

template <typename SrcState, typename DstState> class ScopedBarrierGuard {
  public:
	VkCommandBuffer cmd;
	TypedImage<LayoutMap<SrcState>::value> resource;
	VkImageAspectFlags aspectOverride;
	bool active = true;

	ScopedBarrierGuard(VkCommandBuffer c, const TypedImage<LayoutMap<SrcState>::value>& res,
					   VkImageAspectFlags aspect) noexcept;
	~ScopedBarrierGuard() noexcept;

	// Move-only semantics
	ScopedBarrierGuard(const ScopedBarrierGuard&) = delete;
	auto operator=(const ScopedBarrierGuard&) -> ScopedBarrierGuard& = delete;

	ScopedBarrierGuard(ScopedBarrierGuard&& other) noexcept;
	auto operator=(ScopedBarrierGuard&& other) noexcept -> ScopedBarrierGuard&;
};

template <typename SrcState, typename DstState, typename T>
[[nodiscard]] auto ScopedBarrier(VkCommandBuffer cmd, const T& resource,
								 VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE) noexcept;

// ============================================================================
// Scoped Barrier Functor (Customization Point Objects)
// ============================================================================

template <typename SrcState, typename DstState> struct ScopedBarrierTrans {
	template <typename T>
	[[nodiscard]] auto
	operator()(VkCommandBuffer cmd, const T& resource,
			   VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE) const noexcept {
		return ScopedBarrier<SrcState, DstState, T>(cmd, resource, aspectOverride);
	}
};

// Aliases for common transition pipelines
using ReadToColorTrans = ScopedBarrierTrans<Vk::ShaderReadState, Vk::ColorAttachmentState>;
using ColorToReadTrans = ScopedBarrierTrans<Vk::ColorAttachmentState, Vk::ShaderReadState>;

inline constexpr ReadToColorTrans ReadToColor{};
inline constexpr ColorToReadTrans ColorToRead{};

// ============================================================================
// Frame Execution
// ============================================================================

template <uint32_t N, typename Record, typename Rebuild>
	requires RecordFn<Record> && RebuildFn<Rebuild>
auto DrawFrame(const Context& ctx, const Swapchain& swapchain, const FrameSync<N>& sync,
			   const CommandPools<N, QueueType::Graphics>& pools, uint32_t& frame_index,
			   Record&& record, Rebuild&& rebuild) noexcept -> ZHLN_FrameResult;

template <uint32_t N, typename Record, typename Rebuild>
	requires RecordFn<Record> && RebuildFn<Rebuild>
auto DrawFrame(const Context& ctx, const Swapchain& swapchain, const FrameSync<N>& sync,
			   const CommandPools<N, QueueType::Graphics>& pools, uint32_t& frame_index,
			   VkSemaphore stagingSemaphore, uint64_t stagingWaitValue, Record&& record,
			   Rebuild&& rebuild) noexcept -> ZHLN_FrameResult;

[[nodiscard]] auto SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept -> ZHLN_FrameResult;

void SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, VkSemaphore waitSemaphore = VK_NULL_HANDLE,
				   uint64_t waitValue = 0,
				   VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) noexcept;

void ExecuteCommands(const VkCommandBuffer primary,
					 const std::span<const VkCommandBuffer> secondaries) noexcept;

// ============================================================================
// Dynamic Render Pass Builder
// ============================================================================

static constexpr size_t kMaxColorAttachments = 8;

template <VkImageLayout Layout> struct Tag {};

inline constexpr Tag<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> AsColorAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> AsReadOnly;
inline constexpr Tag<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> AsDepthAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR> AsPresent;

template <VkImageLayout TargetLayout, VkImageLayout OldLayout>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img,
										Tag<TargetLayout> /*unused*/) noexcept;

/**
 * @brief Compile-time-friendly scope guard that automatically transitions depth/stencil
 * attachments back to ReadOnly on destruction.
 */
template <typename ImageT, VkImageLayout Final> class ScopedTransition {
  public:
	ScopedTransition(VkCommandBuffer cmd, ImageT& image, Vk::Tag<Final> transitionTag)
		: cmd_(cmd), image_(Transition(cmd, image, transitionTag)) {}
	~ScopedTransition() { [[maybe_unused]] auto _ = Transition(cmd_, image_, Vk::AsReadOnly); }
	ScopedTransition(const ScopedTransition&) = delete;
	ScopedTransition& operator=(const ScopedTransition&) = delete;
	ScopedTransition(ScopedTransition&&) = delete;
	ScopedTransition& operator=(ScopedTransition&&) = delete;
	auto& Get() { return image_; }

  private:
	VkCommandBuffer cmd_;
	Vk::TypedImage<Final> image_;
};

// Deduction guide for ScopedTransition CTAD
template <typename ImageT, VkImageLayout Final>
ScopedTransition(VkCommandBuffer, ImageT&, Vk::Tag<Final>) -> ScopedTransition<ImageT, Final>;

template <size_t ColorCount = 0, bool HasDepth = false> class DynamicPass {
  public:
	constexpr explicit DynamicPass(VkExtent2D extent) noexcept : _extent(extent) {}

	// Inter-state move constructor
	template <size_t InsideCount, bool InsideDepth>
	constexpr explicit DynamicPass(DynamicPass<InsideCount, InsideDepth>&& other) noexcept
		: _extent(other._extent), _flags(other._flags), _colors(std::move(other)._colors),
		  _depth(other._depth), _viewMask(other._viewMask) {}

	template <VkImageLayout Layout>
	constexpr auto AddColor(
		const TypedImage<Layout>& img, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		const ZHLN::Color4& clearColor = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}) && noexcept
		-> DynamicPass<ColorCount + 1, HasDepth>;

	template <typename... TypedImages>
	constexpr auto AddColorGroup(
		const std::tuple<TypedImages...>& imageTuple,
		VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		const ZHLN::Color4& clearColor = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}) && noexcept
		-> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth>;

	template <VkImageLayout Layout>
	constexpr auto AddDepth(const TypedImage<Layout>& img,
							VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
							VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
							float clearVal = 1.0f) && noexcept -> DynamicPass<ColorCount, true>;

	constexpr auto Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth>&&;

	template <typename Func> void Execute(VkCommandBuffer cmd, Func&& func) const;

	constexpr auto ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth>&&;

	void Bind(VkCommandBuffer cmd,
			  const TypedPipeline<ColorCount, HasDepth>& pipeline) const noexcept {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
	}

  private:
	template <size_t C, bool D> friend class DynamicPass;

	[[nodiscard]] constexpr auto GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo*;

	VkExtent2D _extent{};
	VkRenderingFlags _flags = 0;
	std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> _colors{};
	VkRenderingAttachmentInfo _depth{};
	uint32_t _viewMask = 0;
};

DynamicPass(VkExtent2D) -> DynamicPass<0, false>;

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

std::string ReportVkError(VkResult result, const char* context,
						  const std::source_location& location);
[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept;

[[nodiscard]] auto ResultString(const VkResult result) noexcept -> const char*;
std::expected<VkResult, std::string>
CheckResult(const VkResult result, const char* context = "",
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

template <VkFormat F>
[[nodiscard]] auto
CreateView2DArray(VkDevice device, VkImage image, uint32_t baseLayer, uint32_t layerCount,
				  VkImageAspectFlags aspect = GetFormatAspect(F), uint32_t mips = 1) -> ImageView;

template <VkFormat F>
[[nodiscard]] auto CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers,
									   VkImageAspectFlags aspect = GetFormatAspect(F),
									   uint32_t mips = 1) -> ImageView;

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
