#pragma once

#include "RenderCore.h"
#include "Utils.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <source_location>
#include <span>
#include <type_traits>
#include <utility>

namespace ZHLN {

struct Color4 {
	float r, g, b, a;
};

// NOLINTBEGIN(misc-misplaced-const)

/**
 * @brief Thread-safe-ish ping-pong buffer for temporal rendering state.
 */
template <typename T> class DoubleBuffered {
	std::array<T, 2> _data{};
	uint32_t _index = 0;

  public:
	DoubleBuffered() = default;

	[[nodiscard]] auto Current() noexcept -> T& { return _data[_index]; }
	[[nodiscard]] auto Current() const noexcept -> const T& { return _data[_index]; }

	[[nodiscard]] auto Next() noexcept -> T& { return _data[1 - _index]; }
	[[nodiscard]] auto Next() const noexcept -> const T& { return _data[1 - _index]; }

	// Direct access for initialization loops
	[[nodiscard]] auto operator[](uint32_t idx) noexcept -> T& { return _data[idx % 2]; }
	[[nodiscard]] auto operator[](uint32_t idx) const noexcept -> const T& {
		return _data[idx % 2];
	}

	void Flip() noexcept { _index = 1 - _index; }
};

} // namespace ZHLN

namespace ZHLN::Vk {

// ============================================================================
// TMP / Concepts
// ============================================================================

// Ensures a type is safe to push to the GPU (no pointers, no virtuals)
template <typename T>
concept GpuTriviallyCopyable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

// Concepts for the frame loop callbacks
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
	explicit Handle(T raw) noexcept : _raw(raw) {}

	~Handle() noexcept {
		if (_raw != VK_NULL_HANDLE) {
			DeleterFn(_raw);
		}
	}

	Handle(const Handle&) = delete;
	auto operator=(const Handle&) -> Handle& = delete;

	Handle(Handle&& other) noexcept : _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}
	auto operator=(Handle&& other) noexcept -> Handle& {
		if (this != &other) {
			if (_raw != VK_NULL_HANDLE) {
				DeleterFn(_raw);
			}
			_raw = std::exchange(other._raw, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] auto Get() const noexcept -> T { return _raw; }
	[[nodiscard]] auto Valid() const noexcept -> bool { return _raw != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }
	[[nodiscard]] auto Release() noexcept -> T { return std::exchange(_raw, VK_NULL_HANDLE); }

  private:
	T _raw = VK_NULL_HANDLE;
};

template <typename T, auto DeleterFn> class DeviceHandle {
  public:
	DeviceHandle() noexcept = default;
	DeviceHandle(const VkDevice device, const T raw) noexcept : _device(device), _raw(raw) {}

	~DeviceHandle() noexcept {
		if (_raw != VK_NULL_HANDLE) {
			DeleterFn(_device, _raw);
		}
	}

	DeviceHandle(const DeviceHandle&) = delete;
	auto operator=(const DeviceHandle&) -> DeviceHandle& = delete;

	DeviceHandle(DeviceHandle&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

	auto operator=(DeviceHandle&& other) noexcept -> DeviceHandle& {
		if (this != &other) {
			if (_raw != VK_NULL_HANDLE) {
				DeleterFn(_device, _raw);
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] constexpr auto Get() const noexcept -> T { return _raw; }
	[[nodiscard]] constexpr auto Valid() const noexcept -> bool { return _raw != VK_NULL_HANDLE; }
	constexpr explicit operator bool() const noexcept { return Valid(); }
	[[nodiscard]] constexpr auto Release() noexcept -> T {
		return std::exchange(_raw, VK_NULL_HANDLE);
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	T _raw = VK_NULL_HANDLE;
};

using ShaderModule = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using Semaphore = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;

// ============================================================================
// Context RAII
// Now properly manages Instance and Device destruction.
// ============================================================================

class Context {
  public:
	Context() noexcept = default;

	~Context() noexcept {
		if (_device.handle != VK_NULL_HANDLE) {
			vkDestroyDevice(_device.handle, nullptr);
		}
		if (_instance != VK_NULL_HANDLE) {
			vkDestroyInstance(_instance, nullptr);
		}
	}

	Context(const Context&) = delete;
	auto operator=(const Context&) -> Context& = delete;

	Context(Context&& other) noexcept
		: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
		  _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
		  _physical(std::exchange(other._physical, {})), _device(std::exchange(other._device, {})) {
	}

	auto operator=(Context&& other) noexcept -> Context& {
		if (this != &other) {
			if (_device.handle != VK_NULL_HANDLE) {
				vkDestroyDevice(_device.handle, nullptr);
			}
			if (_instance != VK_NULL_HANDLE) {
				vkDestroyInstance(_instance, nullptr);
			}

			// Clean primitive exchanges
			_instance = std::exchange(other._instance, VK_NULL_HANDLE);
			_surface = std::exchange(other._surface, VK_NULL_HANDLE);

			// Fast, flat value assignments for structure states
			_physical = other._physical;
			_device = other._device;

			other._physical = {};
			other._device = {};
		}
		return *this;
	}

	[[nodiscard(
		"Vulkan context creation may fail; check validity with Valid() or explicit bool cast")]]
	static auto Create(const ZHLN_InstanceDesc& instance_desc,
					   const ZHLN_DeviceSelectDesc& select_desc,
					   const ZHLN_DeviceDesc& device_desc) noexcept -> Context {
		Context ctx;

		// 1. Create Instance
		ctx._instance = ZHLN_CreateInstance(&instance_desc);
		if (ctx._instance == VK_NULL_HANDLE) {
			return {};
		}

		ctx._surface = select_desc.surface;

		// 2. Select Physical Device
		// Create a local const descriptor to inject the newly created instance handle
		const ZHLN_DeviceSelectDesc safe_select = {
			.instance = ctx._instance,
			.surface = select_desc.surface,
			.score_fn = select_desc.score_fn,
			.score_userdata = select_desc.score_userdata,
		};
		ctx._physical = ZHLN_SelectPhysicalDevice(&safe_select);

		if (ctx._physical.handle == VK_NULL_HANDLE) {
			return {};
		}

		// 3. Create Logical Device
		// Create a local const descriptor to inject the physical device snapshot
		const ZHLN_DeviceDesc safe_device = {
			.physical = &ctx._physical,
			.extensions = device_desc.extensions,
			.extension_count = device_desc.extension_count,
			.features = device_desc.features,
			.enable_validation = device_desc.enable_validation,
		};
		ctx._device = ZHLN_CreateDevice(&safe_device);

		return ctx;
	}

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

	[[nodiscard]] auto Formats() const noexcept -> std::span<const VkSurfaceFormatKHR> {
		return {raw.formats, raw.format_count};
	}
	[[nodiscard]] auto PresentModes() const noexcept -> std::span<const VkPresentModeKHR> {
		return {raw.present_modes, raw.present_mode_count};
	}
};

[[nodiscard]] inline auto QuerySwapchainSupport(const VkPhysicalDevice physical,
												const VkSurfaceKHR surface) noexcept
	-> SwapchainSupport {
	const ZHLN_SwapchainSupportDesc desc = {.physical = physical, .surface = surface};
	return {ZHLN_QuerySwapchainSupport(&desc)};
}

class Swapchain {
  public:
	Swapchain() noexcept = default;
	Swapchain(const VkDevice device, const ZHLN_Swapchain raw) noexcept
		: _device(device), _raw(raw) {}

	~Swapchain() noexcept { Destroy(); }

	Swapchain(const Swapchain&) = delete;
	auto operator=(const Swapchain&) -> Swapchain& = delete;

	Swapchain(Swapchain&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, {})) {}

	auto operator=(Swapchain&& other) noexcept -> Swapchain& {
		if (this != &other) {
			Destroy();
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	[[nodiscard]] constexpr auto Get() const noexcept -> const ZHLN_Swapchain& { return _raw; }
	[[nodiscard("Verify swapchain validity before use")]]
	constexpr auto Valid() const noexcept -> bool {
		return _raw.handle != VK_NULL_HANDLE;
	}
	constexpr explicit operator bool() const noexcept { return Valid(); }

	auto Rebuild(const ZHLN_SwapchainDesc& desc) noexcept -> bool {
		// Update our internal device handle so destructors work later
		_device = desc.device->handle;

		const ZHLN_SwapchainDesc rebuilt = {.device = desc.device,
											.physical = desc.physical,
											.surface = desc.surface,
											.width = desc.width,
											.height = desc.height,
											.vsync = desc.vsync,
											.old_swapchain = _raw.handle};

		const ZHLN_Swapchain next = ZHLN_CreateSwapchain(&rebuilt);
		if (next.handle == VK_NULL_HANDLE) {
			return false;
		}

		Destroy();
		_raw = next;
		return true;
	}

  private:
	void Destroy() noexcept {
		if (_raw.handle != VK_NULL_HANDLE) {
			ZHLN_DestroySwapchain(_device, &_raw);
		}
	}

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
	~FrameSync() noexcept {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyFrameSync(_device, _frames.data(), N);
		}
	}
	FrameSync(const FrameSync&) = delete;
	auto operator=(const FrameSync&) -> FrameSync& = delete;

	FrameSync(FrameSync&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _frames(std::exchange(other._frames, {})) {}

	auto operator=(FrameSync&& other) noexcept -> FrameSync& {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				ZHLN_DestroyFrameSync(_device, _frames.data(), N);
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_frames = std::exchange(other._frames, {});
		}
		return *this;
	}

	[[nodiscard("Frame sync creation may fail; verify validity before use in frame loop")]]
	static auto Create(const VkDevice device) noexcept -> FrameSync {
		FrameSync fs;
		const ZHLN_FrameSyncDesc desc = {.device = device, .frame_count = N};
		if (!ZHLN_CreateFrameSync(&desc, fs._frames.data())) {
			return {};
		}
		fs._device = device;
		return fs;
	}

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

	CommandPool(const VkDevice device, const uint32_t queue_family) {
		if (ZHLN_CreateCommandPool(device, queue_family, &_raw)) {
			_device = device;
		}
	}

	~CommandPool() {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyCommandPool(_device, &_raw);
		}
	}

	CommandPool(const CommandPool&) = delete;
	auto operator=(const CommandPool&) -> CommandPool& = delete;

	// Move only
	constexpr CommandPool(CommandPool&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, {})) {}

	auto operator=(CommandPool&& other) noexcept -> CommandPool& {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				ZHLN_DestroyCommandPool(_device, &_raw);
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	[[nodiscard("Verify command pool validity before allocation")]]
	constexpr auto Valid() const noexcept -> bool {
		return _device != VK_NULL_HANDLE;
	}
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return Valid(); }

	// Seamless casting back to the raw C layout structure
	[[nodiscard]] constexpr operator const ZHLN_CommandPool&() const noexcept { return _raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool&() noexcept { return _raw; }

	[[nodiscard("Check allocation success before using command buffers")]]
	auto Allocate(const uint32_t count) -> bool {
		if (!Valid()) {
			return false;
		}
		return ZHLN_AllocateCommandBuffers(_device, &_raw, count);
	}

	[[nodiscard]] constexpr auto operator[](const uint32_t idx) const noexcept -> VkCommandBuffer {
		return _raw.buffers[idx];
	}

	[[nodiscard("Check allocation success before using secondary command buffers")]]
	auto AllocateSecondary(const uint32_t count) -> bool {
		if (!Valid()) {
			return false;
		}
		return ZHLN_AllocateSecondaryCommandBuffers(_device, &_raw, count);
	}

	void Reset() noexcept {
		if (Valid()) {
			ZHLN_ResetCommandPool(_device, &_raw);
		}
	}

	// Implicit conversions to raw pointers
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

	// Rule of Zero: Destructor, Copy, and Move variants are entirely
	// compiler-synthesized. No redundant tracking code required.

	[[nodiscard("Command pools must be verified before use in command recording")]]
	static auto Create(const VkDevice device, const Description& desc) noexcept -> CommandPools {
		CommandPools cp;
		for (auto& pool : cp._pools) {
			pool = CommandPool(device, desc.queue_family);
			if (!pool || !pool.Allocate(desc.buffers_per_pool)) {
				return {};
			}
		}
		return cp;
	}

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) noexcept -> CommandPool& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) const noexcept
		-> const CommandPool& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto Cmd(const uint32_t frame) const noexcept -> VkCommandBuffer {
		return _pools[frame % N][0]; // Uses CommandPool's cleaner indexing
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
	return ZHLN_ShaderDesc{
		.code = code,
		.size = size,
		.entry_point = entry // Safely initialized to nullptr if omitted
	};
}

class ShaderStages {
  public:
	constexpr ShaderStages() noexcept = default;
	constexpr ShaderStages(const VkDevice device, const ZHLN_ShaderStages raw) noexcept
		: _device(device), _raw(raw) {}

	// Destructor & Move Assignment (Move implementation to .cpp)
	~ShaderStages() noexcept;
	ShaderStages(const ShaderStages&) = delete;
	auto operator=(const ShaderStages&) -> ShaderStages& = delete;
	ShaderStages(ShaderStages&& other) noexcept;
	auto operator=(ShaderStages&& other) noexcept -> ShaderStages&;

	[[nodiscard("Shader creation may fail; verify validity before binding")]]
	static auto Create(const VkDevice device, const ZHLN_ShaderDesc& vert,
					   const ZHLN_ShaderDesc& frag) noexcept -> ShaderStages {
		const ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
		ZHLN_ShaderStages stages{};
		if (!ZHLN_CreateShaderStages(&desc, &stages)) {
			return {};
		}
		return {device, stages};
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

// ============================================================================
// Command & Rendering Helpers
// ============================================================================

// Scoped RAII for Dynamic Rendering
class ScopedRendering {
  public:
	ScopedRendering(const VkCommandBuffer cmd, const ZHLN_RenderPassDesc& desc) noexcept
		: _cmd(cmd) {
		ZHLN_BeginRendering(_cmd, &desc);
	}
	~ScopedRendering() noexcept { ZHLN_EndRendering(_cmd); }

	ScopedRendering(ScopedRendering&&) = delete;
	auto operator=(ScopedRendering&&) -> ScopedRendering& = delete;

	ScopedRendering(const ScopedRendering&) = delete;
	auto operator=(const ScopedRendering&) -> ScopedRendering& = delete;

  private:
	VkCommandBuffer _cmd;
};

inline void ImageBarrier(const VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept {
	ZHLN_CmdImageBarrier(cmd, &desc);
}

template <VkImageLayout Layout> struct LayoutTraits;

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_UNDEFINED> {
	// No access needed because we are discarding the contents
	static constexpr VkAccessFlags2 access = 0;

	// By waiting at COLOR_ATTACHMENT_OUTPUT_BIT, this transition properly synchronizes
	// with the vkAcquireNextImageKHR semaphore which is signaled at this exact stage.
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
};

// Specialization for Color Attachment
template <> struct LayoutTraits<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
};

// Specialization for Shader Read
template <> struct LayoutTraits<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_SHADER_READ_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
};

// Specialization for Presentation (The hand-off to the OS)
template <> struct LayoutTraits<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR> {
	// We aren't doing anything to the image; we're just giving it away.
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_NONE;

	// Presentation happens after the color attachment output is finished.
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
};

// Specialization for Depth Attachment
template <> struct LayoutTraits<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	// MUST include both Early and Late for full synchronization
	static constexpr VkPipelineStageFlags2 stage =
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
};

// For Transfer (Blitting / Copying)
template <> struct LayoutTraits<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_TRANSFER_READ_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
};

// Specialization for Compute Shader Storage Image Read/Write
template <> struct LayoutTraits<VK_IMAGE_LAYOUT_GENERAL> {
	static constexpr VkAccessFlags2 access =
		VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
};

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void TransitionLayout(const VkCommandBuffer cmd, const VkImage image,
							 const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept {

	using Src = LayoutTraits<OldLayout>;
	using Dst = LayoutTraits<NewLayout>;

	const ZHLN_ImageBarrierDesc barrier = {
		.image = image,
		.src_access = Src::access,
		.dst_access = Dst::access,
		.src_layout = OldLayout,
		.dst_layout = NewLayout,
		.src_stage = Src::stage,
		.dst_stage = Dst::stage,
		.aspect = aspect,
		.base_mip = 0, // Added
		.mip_count =
			VK_REMAINING_MIP_LEVELS // Added: Fixes VUID-VkImageSubresourceRange-levelCount-01720
	};

	ZHLN_CmdImageBarrier(cmd, &barrier);
}

inline void CopyBufferToImage(const VkCommandBuffer cmd,
							  const ZHLN_BufferImageCopyDesc& desc) noexcept {
	ZHLN_CmdCopyBufferToImage(cmd, &desc);
}

// Typed wrapper around Push Constants
template <GpuTriviallyCopyable T>
inline void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout,
				 const VkShaderStageFlags stages, const T& value) noexcept {
	ZHLN_PushConstants(cmd, layout, stages, &value, sizeof(T));
}

// ============================================================================
// Frame Execution
// ============================================================================

template <uint32_t N, typename Record, typename Rebuild>
	requires RecordFn<Record> && RebuildFn<Rebuild>
inline auto DrawFrame(const Context& ctx, const Swapchain& swapchain, const FrameSync<N>& sync,
					  const CommandPools<N>& pools, uint32_t& frame_index, Record&& record,
					  Rebuild&& rebuild) noexcept -> ZHLN_FrameResult {

	const ZHLN_FrameSync& s = sync[frame_index];
	const ZHLN_CommandPool& pool = pools[frame_index];
	const VkCommandBuffer cmd = pools.Cmd(frame_index);

	// 1 & 2. Synchronize & Acquire (Offloaded to C)
	uint32_t image_index = 0;
	auto result =
		ZHLN_WaitAndAcquireImage(ctx.Device(), swapchain.Get().handle, &s, &pool, &image_index);
	if (result == ZHLN_FrameResult_OutOfDate) [[unlikely]] {
		std::invoke(std::forward<Rebuild>(rebuild));
		return result;
	}

	// 3. Record: perfectly forward the callable (Inlined for compiler optimization)
	ZHLN_BeginCommandBuffer(cmd);
	std::invoke(std::forward<Record>(record), cmd, image_index);
	ZHLN_EndCommandBuffer(cmd);

	// 4 & 5. Submit & Present (Offloaded to C via the existing ZHLN_SubmitAndPresent)
	const ZHLN_FrameSubmitDesc submit_desc = {
		.graphicsQueue = ctx.GraphicsQueue(),
		.presentQueue = ctx.PresentQueue(),
		.cmd = cmd,
		.imageAvailable = s.image_available,
		.renderFinished = s.render_finished,
		.inFlight = s.in_flight,
		.swapchain = swapchain.Get().handle,
		.imageIndex = image_index,
	};

	result = ZHLN_SubmitAndPresent(&submit_desc);
	if (result == ZHLN_FrameResult_OutOfDate || result == ZHLN_FrameResult_Suboptimal)
		[[unlikely]] {
		std::invoke(std::forward<Rebuild>(rebuild));
	}

	// --- 6. Optimized Frame Index Increment ---
	if constexpr ((N & (N - 1)) == 0) {
		frame_index = (frame_index + 1) & (N - 1);
	} else if constexpr (N == 3) {
		frame_index = (frame_index == 2) ? 0 : frame_index + 1;
	} else {
		frame_index = (frame_index + 1) % N;
	}

	return result;
}

[[nodiscard]]
inline auto SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept -> ZHLN_FrameResult {
	// Simply pass the address of the reference to the C function
	return ZHLN_SubmitAndPresent(&desc);
}

inline void ExecuteCommands(const VkCommandBuffer primary,
							const std::span<const VkCommandBuffer> secondaries) noexcept {
	if (!secondaries.empty()) {
		vkCmdExecuteCommands(primary, static_cast<uint32_t>(secondaries.size()),
							 secondaries.data());
	}
}

// ============================================================================
// Type-Safe, Zero-Allocation Dynamic Render Pass Builder
// ============================================================================
template <size_t ColorCount = 1, bool HasDepth = false> class DynamicPass {
  public:
	constexpr explicit DynamicPass(VkExtent2D extent) noexcept : _extent(extent) {}

	constexpr auto Color(size_t index, VkImageView view,
						 VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
						 VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE) noexcept
		-> DynamicPass& {
		_colors[index] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
						  .pNext = nullptr,
						  .imageView = view,
						  .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						  .resolveMode = VK_RESOLVE_MODE_NONE,
						  .resolveImageView = VK_NULL_HANDLE,
						  .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
						  .loadOp = loadOp,
						  .storeOp = storeOp,
						  .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}};
		return *this;
	}

	constexpr auto Depth(VkImageView view, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
						 VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						 float clearVal = 1.0f) noexcept -> DynamicPass&
		requires(HasDepth)
	{
		_depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				  .pNext = nullptr,
				  .imageView = view,
				  .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
				  .resolveMode = VK_RESOLVE_MODE_NONE,
				  .resolveImageView = VK_NULL_HANDLE,
				  .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				  .loadOp = loadOp,
				  .storeOp = storeOp,
				  .clearValue = {.depthStencil = {.depth = clearVal, .stencil = 0}}};
		return *this;
	}

	constexpr auto ClearColor(size_t index, const ZHLN::Color4& color) noexcept -> DynamicPass& {
		_colors[index].clearValue.color = {.float32 = {color.r, color.g, color.b, color.a}};
		return *this;
	}

	constexpr auto ClearColor(size_t index, float r, float g, float b, float a = 1.0f) noexcept
		-> DynamicPass& {
		_colors[index].clearValue.color = {.float32 = {r, g, b, a}};
		return *this;
	}

	constexpr auto Flags(VkRenderingFlags flags) noexcept -> DynamicPass& {
		_flags = flags;
		return *this;
	}

	template <typename Func> void Execute(VkCommandBuffer cmd, Func&& func) const {
		const VkRenderingInfo renderInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = _flags,
			.renderArea = {.offset = {0, 0}, .extent = _extent},
			.layerCount = 1,
			.viewMask = 0,
			.colorAttachmentCount = ColorCount,
			.pColorAttachments = ColorCount > 0 ? _colors.data() : nullptr,
			.pDepthAttachment = GetDepthPtr(), // <-- Safely resolve using the constexpr helper
			.pStencilAttachment = nullptr,
		};

		vkCmdBeginRendering(cmd, &renderInfo);

		const VkViewport viewport = {.x = 0.0f,
									 .y = (float)_extent.height,
									 .width = (float)_extent.width,
									 .height = -(float)_extent.height,
									 .minDepth = 0.0f,
									 .maxDepth = 1.0f};
		const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = _extent};
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		std::forward<Func>(func)();

		vkCmdEndRendering(cmd);
	}

  private:
	// This constexpr helper safely discards the invalid pointer conversion branch at compile time
	[[nodiscard]] constexpr auto GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo* {
		if constexpr (HasDepth) {
			return &_depth;
		} else {
			return nullptr;
		}
	}

	VkExtent2D _extent;
	VkRenderingFlags _flags = 0;
	std::array<VkRenderingAttachmentInfo, ColorCount> _colors{};
	[[no_unique_address]] std::conditional_t<HasDepth, VkRenderingAttachmentInfo, std::monostate>
		_depth{};
};

// ============================================================================
// Consolidated Graphics State & Instanced Draw Dispatcher
// ============================================================================
struct DrawState {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer vbo = VK_NULL_HANDLE;
	uint32_t vertexCount = 0;
	uint32_t instanceCount = 1;
	uint32_t firstVertex = 0;
	uint32_t firstInstance = 0;
};

template <GpuTriviallyCopyable T>
inline void DrawInstanced(VkCommandBuffer cmd, const DrawState& state, const T& pushConstants,
						  VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
													  VK_SHADER_STAGE_FRAGMENT_BIT) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
	if (state.set != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.layout, 0, 1,
								&state.set, 0, nullptr);
	}
	vkCmdPushConstants(cmd, state.layout, stages, 0, sizeof(T), &pushConstants);

	VkDeviceSize offset = 0;
	VkBuffer vboHandle = state.vbo;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vboHandle, &offset);

	// True instanced dispatch
	vkCmdDraw(cmd, state.vertexCount, state.instanceCount, state.firstVertex, state.firstInstance);
}

// ============================================================================
// Surface helpers
// ============================================================================

class Surface {
  public:
	Surface() = default;
	Surface(VkInstance instance, VkSurfaceKHR surface) : _instance(instance), _handle(surface) {}

	~Surface() {
		if (_handle != VK_NULL_HANDLE) {
			vkDestroySurfaceKHR(_instance, _handle, nullptr);
		}
	}

	// Move only
	Surface(const Surface&) = delete;
	auto operator=(const Surface&)
		-> Surface& = delete; // <-- Explicitly deleted to satisfy Rule of Five

	Surface(Surface&& other) noexcept
		: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
		  _handle(std::exchange(other._handle, VK_NULL_HANDLE)) {}

	auto operator=(Surface&& other) noexcept -> Surface& {
		if (this != &other) {
			if (_handle != VK_NULL_HANDLE) {
				vkDestroySurfaceKHR(_instance, _handle, nullptr);
			}
			_instance = std::exchange(other._instance, VK_NULL_HANDLE);
			_handle = std::exchange(other._handle, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] auto Get() const -> VkSurfaceKHR { return _handle; }

  private:
	VkInstance _instance = VK_NULL_HANDLE;
	VkSurfaceKHR _handle = VK_NULL_HANDLE;
};

// ============================================================================
// Error Helpers
// ============================================================================

void ReportVkError(VkResult result, const char* context, const std::source_location& location);
[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept;

[[nodiscard]] inline auto ResultString(const VkResult result) noexcept -> const char* {
	return ZHLN_VkResultString(result);
}

inline void CheckResult(const VkResult result, const char* context = "",
						const std::source_location location = std::source_location::current()) {
	if (result != VK_SUCCESS) [[unlikely]] {
		// No printing or formatting is invoked here!
		ReportVkError(result, context, location);
	}
}

// ============================================================================
// Semaphore Helpers
// ============================================================================

// Perfect 64-byte structure (1 Cache Line)
class alignas(64) SemaphorePool {
  public:
	SemaphorePool() noexcept = default;
	~SemaphorePool() noexcept { Cleanup(); }

	// Move-only
	SemaphorePool(const SemaphorePool&) = delete;
	auto operator=(const SemaphorePool&) -> SemaphorePool& = delete;

	SemaphorePool(SemaphorePool&& other) noexcept : _device(other._device), _count(other._count) {
		for (uint32_t i = 0; i < 6; ++i) {
			_semaphores[i] = other._semaphores[i];
			other._semaphores[i] = VK_NULL_HANDLE;
		}
		other._device = VK_NULL_HANDLE;
		other._count = 0;
	}

	auto operator=(SemaphorePool&& other) noexcept -> SemaphorePool& {
		if (this != &other) {
			Cleanup();
			_device = other._device;
			_count = other._count;

			for (uint32_t i = 0; i < 6; ++i) {
				_semaphores[i] = other._semaphores[i];
				other._semaphores[i] = VK_NULL_HANDLE;
			}

			other._device = VK_NULL_HANDLE;
			other._count = 0;
		}
		return *this;
	}

	void Rebuild(const VkDevice device, const uint32_t count) noexcept {
		Cleanup();
		_device = device;
		// Cap at 6 to ensure 64-byte struct size
		_count = ZHLN::Min(count, 6U);

		for (uint32_t i = 0; i < _count; ++i) {
			_semaphores[i] = ZHLN_CreateSemaphore(_device);
		}
	}

	[[nodiscard("Semaphore access must be checked for bounds; invalid indices will crash")]]
	auto operator[](const uint32_t index) const noexcept -> VkSemaphore {
		if (index >= _count) [[unlikely]] {
			// Dispatches directly to the compilation unit's out-of-line crash reporter
			ReportSemaphoreBoundsError(index, _count);
		}
		return _semaphores[index];
	}

	[[nodiscard]] auto Count() const noexcept -> uint32_t { return _count; }
	[[nodiscard("Verify semaphore pool is initialized before use")]]
	auto Valid() const noexcept -> bool {
		return _device != VK_NULL_HANDLE;
	}

  private:
	void Cleanup() noexcept {
		if (_device == VK_NULL_HANDLE) {
			return;
		}

		// Locally cache device handle for the loop
		auto* const d = _device;
		for (uint32_t i = 0; i < _count; ++i) {
			// Local null check prevents expensive driver thunk if slot is empty
			if (_semaphores[i] != VK_NULL_HANDLE) {
				vkDestroySemaphore(d, _semaphores[i], nullptr);
			}
		}

		_semaphores.fill(VK_NULL_HANDLE);
		_count = 0;
		_device = VK_NULL_HANDLE;
	}

	// Order matters for packing:
	VkDevice _device = VK_NULL_HANDLE;			 // 8 bytes
	uint32_t _count = 0;						 // 4 bytes
	[[maybe_unused]] uint32_t _padding = 0;		 // 4 bytes (Explicit padding for 64-bit alignment)
	std::array<VkSemaphore, 6> _semaphores = {}; // 48 bytes
												 // Total: Exactly 64 bytes.
};

// ============================================================================
// Image View Helpers
// ============================================================================

[[nodiscard]] constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags {
	switch (format) {
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_SRGB:
			return VK_IMAGE_ASPECT_COLOR_BIT;

		case VK_FORMAT_D32_SFLOAT:
			return VK_IMAGE_ASPECT_DEPTH_BIT;

		case VK_FORMAT_D24_UNORM_S8_UINT:
			return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

		case VK_FORMAT_R16G16_SFLOAT:
			return VK_IMAGE_ASPECT_COLOR_BIT;

		default:
			return VK_IMAGE_ASPECT_NONE;
	}
}

using ImageView = DeviceHandle<VkImageView, ZHLN_DestroyImageView>;

template <VkFormat F>
[[nodiscard]] static auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips = 1)
	-> ImageView {
	ZHLN_ImageViewDesc desc = {
		.image = image,
		.format = F,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
		.mip_levels = mips,
		.array_layers = 6,
		.view_type = VK_IMAGE_VIEW_TYPE_CUBE // <-- Explicitly flag as CUBE
	};
	return {device, ZHLN_CreateImageView(device, &desc)};
}

template <VkFormat F>
[[nodiscard]] static auto CreateView(VkDevice device, VkImage image,
									 VkImageAspectFlags aspect = GetFormatAspect(F),
									 uint32_t mips = 1) -> ImageView {
	// Guard compile-time usage against unmapped formats
	static_assert(GetFormatAspect(F) != VK_IMAGE_ASPECT_NONE,
				  "The provided VkFormat has no mapped aspect flags. Add it to GetFormatAspect().");

	ZHLN_ImageViewDesc desc = {
		.image = image, .format = F, .aspect = aspect, .mip_levels = mips, .array_layers = 0};
	return {device, ZHLN_CreateImageView(device, &desc)};
}

using Sampler = DeviceHandle<VkSampler, ZHLN_DestroySampler>;
using DescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, ZHLN_DestroyDescriptorSetLayout>;
using DescriptorPool = DeviceHandle<VkDescriptorPool, ZHLN_DestroyDescriptorPool>;

// ============================================================================
// Extension Query Utilities
// ============================================================================

[[nodiscard]] inline auto IsInstanceExtensionSupported(std::string_view extension) noexcept
	-> bool {
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);

	// Most systems have < 128 extensions.
	// VkExtensionProperties is ~260 bytes, so 128 * 260 = ~33KB on stack.
	std::array<VkExtensionProperties, maxInstanceExtensions> available{};
	count = ZHLN::Min<uint32_t>(count, maxInstanceExtensions); // Cap to buffer size

	vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

	for (uint32_t i = 0; i < count; ++i) {
		if (extension == available[i].extensionName) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline auto IsDeviceExtensionSupported(VkPhysicalDevice physical,
													 std::string_view extension) noexcept -> bool {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);

	std::array<VkExtensionProperties, maxInstanceExtensions> available{};
	count = ZHLN::Min<uint32_t>(count, maxInstanceExtensions);

	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());

	for (uint32_t i = 0; i < count; ++i) {
		if (extension == available[i].extensionName) {
			return true;
		}
	}
	return false;
}

// ============================================================================
// Zero-Allocation Render Graph
// ============================================================================

struct PassResource {
	ZHLN_ImageBarrierDesc barrier;
};

// Use a raw function pointer and a void* for state.
// This is exactly what std::function does, but without the heap allocation
// and with a trivial, predictable call site.
using PassRecordFn = void (*)(VkCommandBuffer, const void* userData);

struct PassDesc {
	const char* name = "Unnamed Pass";

	// Use std::span instead of std::vector to avoid copying/allocation.
	// The transitions must live as long as ExecutePasses is running.
	std::span<const PassResource> transitions;

	PassRecordFn record = nullptr;
	const void* pUserData = nullptr;
};

/**
 * @brief Executes a sequence of render passes.
 *
 * @tparam MaxStackBarriers The number of image barriers to allocate on the stack before
 *                          falling back to the heap. Defaults to 16.
 * @param cmd The command buffer to record into.
 * @param passes A span of pass descriptions to execute.
 */
template <size_t MaxStackBarriers = 16>
inline void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept {
	// Stack-allocated buffer for barriers to avoid heap allocation in the hot loop.
	std::array<VkImageMemoryBarrier2, MaxStackBarriers> stack_barriers;

	for (const auto& pass : passes) {
		const auto transition_count = static_cast<uint32_t>(pass.transitions.size());

		if (transition_count > 0) {
			VkImageMemoryBarrier2* p_barriers = stack_barriers.data();
			VkImageMemoryBarrier2* heap_allocated = nullptr;

			// Fallback to manual heap allocation if stack is too small
			if (transition_count > MaxStackBarriers) [[unlikely]] {
				heap_allocated = new (std::nothrow) VkImageMemoryBarrier2[transition_count];
				p_barriers = heap_allocated;
			}

			for (uint32_t i = 0; i < transition_count; ++i) {
				const auto& res = pass.transitions[i];
				p_barriers[i] = {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.pNext = nullptr,
					.srcStageMask = res.barrier.src_stage,
					.srcAccessMask = res.barrier.src_access,
					.dstStageMask = res.barrier.dst_stage,
					.dstAccessMask = res.barrier.dst_access,
					.oldLayout = res.barrier.src_layout,
					.newLayout = res.barrier.dst_layout,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = res.barrier.image,
					.subresourceRange =
						{
							.aspectMask = res.barrier.aspect,
							.baseMipLevel = 0,
							.levelCount = VK_REMAINING_MIP_LEVELS,
							.baseArrayLayer = 0,
							.layerCount = VK_REMAINING_ARRAY_LAYERS,
						},
				};
			}

			const VkDependencyInfo dep_info = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
											   .pNext = nullptr,
											   .imageMemoryBarrierCount = transition_count,
											   .pImageMemoryBarriers = p_barriers};
			vkCmdPipelineBarrier2(cmd, &dep_info);

			if (heap_allocated) [[unlikely]] {
				delete[] heap_allocated;
			}
		}

		// Execute the recording callback (Function pointer + UserData)
		if (pass.record) {
			pass.record(cmd, pass.pUserData);
		}
	}
}

// Typed dispatch helper — computes group count from total invocations and local size
inline void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ,
					 uint32_t localX, uint32_t localY, uint32_t localZ) noexcept {
	ZHLN_CmdDispatch(cmd, (totalX + localX - 1) / localX, (totalY + localY - 1) / localY,
					 (totalZ + localZ - 1) / localZ);
}

// Direct group count version when you're managing it yourself
inline void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept {
	ZHLN_CmdDispatch(cmd, gX, gY, gZ);
}

// ============================================================================
// Mipmapping
// ============================================================================

/**
 * @brief TMP helper to calculate mip levels at compile-time.
 */
template <uint32_t Width, uint32_t Height> consteval auto GetMipLevels() noexcept -> uint32_t {
	return std::bit_width(ZHLN::Max(Width, Height));
}

/**
 * @brief Runtime/Compile-time hybrid mipmap generator.
 * Uses std::bit_width for O(1) level calculation instead of log2.
 */
inline void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width,
							const uint32_t height) {
	// std::bit_width(n) returns 1 + floor(log2(n)).
	// It's constexpr and maps to a single CPU instruction (BSR/CLZ).
	uint32_t levels = std::bit_width(ZHLN::Max(width, height));

	ZHLN_GenerateMipmaps(cmd, image, static_cast<int32_t>(width), static_cast<int32_t>(height),
						 levels);
}

// NOLINTEND(misc-misplaced-const)

} // namespace ZHLN::Vk
