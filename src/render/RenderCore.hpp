#pragma once

#include "RenderCore.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <print>
#include <source_location>
#include <span>
#include <type_traits>
#include <utility>

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
	Handle& operator=(const Handle&) = delete;

	Handle(Handle&& other) noexcept : _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}
	Handle& operator=(Handle&& other) noexcept {
		if (this != &other) {
			if (_raw != VK_NULL_HANDLE) {
				DeleterFn(_raw);
			}
			_raw = std::exchange(other._raw, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] T Get() const noexcept { return _raw; }
	[[nodiscard]] bool Valid() const noexcept { return _raw != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }
	[[nodiscard]] T Release() noexcept { return std::exchange(_raw, VK_NULL_HANDLE); }

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
	DeviceHandle& operator=(const DeviceHandle&) = delete;

	DeviceHandle(DeviceHandle&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

	DeviceHandle& operator=(DeviceHandle&& other) noexcept {
		if (this != &other) {
			if (_raw != VK_NULL_HANDLE) {
				DeleterFn(_device, _raw);
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] constexpr T Get() const noexcept { return _raw; }
	[[nodiscard]] constexpr bool Valid() const noexcept { return _raw != VK_NULL_HANDLE; }
	constexpr explicit operator bool() const noexcept { return Valid(); }
	[[nodiscard]] constexpr T Release() noexcept { return std::exchange(_raw, VK_NULL_HANDLE); }

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
	Context& operator=(const Context&) = delete;

	Context(Context&& other) noexcept
		: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
		  _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
		  _physical(std::exchange(other._physical, {})), _device(std::exchange(other._device, {})) {
	}

	Context& operator=(Context&& other) noexcept {
		if (this != &other) {
			if (_device.handle != VK_NULL_HANDLE) {
				vkDestroyDevice(_device.handle, nullptr);
			}
			if (_instance != VK_NULL_HANDLE) {
				vkDestroyInstance(_instance, nullptr);
			}

			_instance = std::exchange(other._instance, VK_NULL_HANDLE);
			_surface = std::exchange(other._surface, VK_NULL_HANDLE);
			_physical = std::exchange(other._physical, {});
			_device = std::exchange(other._device, {});
		}
		return *this;
	}

	[[nodiscard]] static Context Create(const ZHLN_InstanceDesc& instance_desc,
										const ZHLN_DeviceSelectDesc& select_desc,
										const ZHLN_DeviceDesc& device_desc) noexcept {
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

	[[nodiscard]] VkInstance Instance() const noexcept { return _instance; }
	[[nodiscard]] VkDevice Device() const noexcept { return _device.handle; }
	[[nodiscard]] VkQueue GraphicsQueue() const noexcept { return _device.graphics_queue; }
	[[nodiscard]] VkQueue PresentQueue() const noexcept { return _device.present_queue; }
	[[nodiscard]] VkPhysicalDevice Physical() const noexcept { return _physical.handle; }
	[[nodiscard]] const ZHLN_PhysicalDeviceInfo& PhysicalInfo() const noexcept { return _physical; }

	[[nodiscard]] bool Valid() const noexcept { return _device.handle != VK_NULL_HANDLE; }
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

	[[nodiscard]] std::span<const VkSurfaceFormatKHR> Formats() const noexcept {
		return {raw.formats, raw.format_count};
	}
	[[nodiscard]] std::span<const VkPresentModeKHR> PresentModes() const noexcept {
		return {raw.present_modes, raw.present_mode_count};
	}
};

[[nodiscard]] inline SwapchainSupport QuerySwapchainSupport(const VkPhysicalDevice physical,
															const VkSurfaceKHR surface) noexcept {
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
	Swapchain& operator=(const Swapchain&) = delete;

	Swapchain(Swapchain&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, {})) {}

	Swapchain& operator=(Swapchain&& other) noexcept {
		if (this != &other) {
			Destroy();
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	[[nodiscard]] constexpr const ZHLN_Swapchain& Get() const noexcept { return _raw; }
	[[nodiscard]] constexpr bool Valid() const noexcept { return _raw.handle != VK_NULL_HANDLE; }
	constexpr explicit operator bool() const noexcept { return Valid(); }

	bool Rebuild(const ZHLN_SwapchainDesc& desc) noexcept {
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
		if (!next.handle) {
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
	FrameSync& operator=(const FrameSync&) = delete;

	FrameSync(FrameSync&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _frames(std::exchange(other._frames, {})) {}

	FrameSync& operator=(FrameSync&& other) noexcept {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				ZHLN_DestroyFrameSync(_device, _frames.data(), N);
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_frames = std::exchange(other._frames, {});
		}
		return *this;
	}

	[[nodiscard]] static FrameSync Create(const VkDevice device) noexcept {
		FrameSync fs;
		const ZHLN_FrameSyncDesc desc = {.device = device, .frame_count = N};
		if (!ZHLN_CreateFrameSync(&desc, fs._frames.data())) {
			return {};
		}
		fs._device = device;
		return fs;
	}

	[[nodiscard]] constexpr const ZHLN_FrameSync& operator[](const uint32_t frame) const noexcept {
		return _frames[frame % N];
	}
	[[nodiscard]] static constexpr uint32_t Count() noexcept { return N; }
	[[nodiscard]] constexpr bool Valid() const noexcept { return _device != VK_NULL_HANDLE; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<ZHLN_FrameSync, N> _frames = {};
};

template <uint32_t N>
	requires(N > 0 && N <= 8)
class CommandPools {
  public:
	CommandPools() noexcept = default;
	~CommandPools() noexcept {
		if (_device != VK_NULL_HANDLE) {
			for (auto& pool : _pools) {
				ZHLN_DestroyCommandPool(_device, &pool);
			}
		}
	}
	CommandPools(const CommandPools&) = delete;
	CommandPools& operator=(const CommandPools&) = delete;

	constexpr CommandPools(CommandPools&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _pools(std::exchange(other._pools, {})) {}

	CommandPools& operator=(CommandPools&& other) noexcept {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				for (auto& pool : _pools) {
					ZHLN_DestroyCommandPool(_device, &pool);
				}
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_pools = std::exchange(other._pools, {});
		}
		return *this;
	}

	[[nodiscard]] static CommandPools Create(const VkDevice device, const uint32_t queue_family,
											 const uint32_t buffers_per_pool = 1) noexcept {
		CommandPools cp;
		cp._device = device;
		for (auto& pool : cp._pools) {
			if (!ZHLN_CreateCommandPool(device, queue_family, &pool) ||
				!ZHLN_AllocateCommandBuffers(device, &pool, buffers_per_pool)) {
				return {};
			}
		}
		return cp;
	}

	[[nodiscard]] constexpr ZHLN_CommandPool& operator[](const uint32_t frame) noexcept {
		return _pools[frame % N];
	}
	[[nodiscard]] constexpr VkCommandBuffer Cmd(const uint32_t frame) const noexcept {
		return _pools[frame % N].buffers[0];
	}
	[[nodiscard]] constexpr bool Valid() const noexcept { return _device != VK_NULL_HANDLE; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<ZHLN_CommandPool, N> _pools = {};
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
		if (_device) {
			ZHLN_DestroyCommandPool(_device, &_raw);
		}
	}

	// Move only
	constexpr CommandPool(CommandPool&& other) noexcept
		: _device(std::exchange(other._device, nullptr)), _raw(std::exchange(other._raw, {})) {}

	CommandPool& operator=(CommandPool&& other) noexcept {
		if (this != &other) {
			if (_device) {
				ZHLN_DestroyCommandPool(_device, &_raw);
			}
			_device = std::exchange(other._device, nullptr);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	[[nodiscard]] constexpr bool Valid() const noexcept { return _device != nullptr; }
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] bool Allocate(const uint32_t count) {
		if (!Valid()) {
			return false;
		}
		return ZHLN_AllocateCommandBuffers(_device, &_raw, count);
	}

	[[nodiscard]] constexpr VkCommandBuffer operator[](const uint32_t i) const {
		return _raw.buffers[i];
	}

  private:
	VkDevice _device = nullptr;
	ZHLN_CommandPool _raw{};
};

// ============================================================================
// ShaderStages RAII
// ============================================================================

class ShaderStages {
  public:
	// Default constructor is pure state initialization
	constexpr ShaderStages() noexcept = default;

	// Simple handle copying is constexpr-safe
	constexpr ShaderStages(const VkDevice device, const ZHLN_ShaderStages raw) noexcept
		: _device(device), _raw(raw) {}

	// Calls ZHLN_DestroyShaderStages (C-backend)
	~ShaderStages() noexcept {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyShaderStages(_device, &_raw);
		}
	}

	ShaderStages(const ShaderStages&) = delete;
	ShaderStages& operator=(const ShaderStages&) = delete;

	ShaderStages& operator=(ShaderStages&& other) noexcept {
		if (this != &other) {
			// 1. Clean up our own existing resources first!
			if (_device != VK_NULL_HANDLE) {
				ZHLN_DestroyShaderStages(_device, &_raw);
			}
			// 2. Transfer ownership from the other object
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	// std::exchange on handles is constexpr since C++20
	constexpr ShaderStages(ShaderStages&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, {})) {}

	// Calls ZHLN_CreateShaderStages (C-backend)
	[[nodiscard]] static ShaderStages Create(const VkDevice device, const ZHLN_ShaderDesc& vert,
											 const ZHLN_ShaderDesc& frag) noexcept {
		const ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
		ZHLN_ShaderStages stages{};
		if (!ZHLN_CreateShaderStages(&desc, &stages)) {
			return {};
		}
		return {device, stages};
	}

	// Just returning an address
	[[nodiscard]] constexpr const ZHLN_ShaderStages* Get() const noexcept { return &_raw; }

	// Simple handle comparison
	[[nodiscard]] constexpr bool Valid() const noexcept {
		return _raw.vert.handle != VK_NULL_HANDLE;
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_ShaderStages _raw{};
};

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

	ScopedRendering(const ScopedRendering&) = delete;
	ScopedRendering& operator=(const ScopedRendering&) = delete;

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
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
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

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void TransitionLayout(const VkCommandBuffer cmd, const VkImage image,
							 const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept {

	using Src = LayoutTraits<OldLayout>;
	using Dst = LayoutTraits<NewLayout>;

	// This entire struct is populated with static constants.
	// The compiler will fold this into a single block of immediate values.
	const ZHLN_ImageBarrierDesc barrier = {
		.image = image,
		.src_access = Src::access,
		.dst_access = Dst::access,
		.src_layout = OldLayout,
		.dst_layout = NewLayout,
		.src_stage = Src::stage,
		.dst_stage = Dst::stage,
		.aspect = aspect,
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

template <uint32_t N, RecordFn Record, RebuildFn Rebuild>
ZHLN_FrameResult DrawFrame(const Context& ctx, const Swapchain& swapchain, const FrameSync<N>& sync,
						   const CommandPools<N>& pools, uint32_t& frame_index,
						   const Record&& record, const Rebuild&& rebuild) noexcept {

	const ZHLN_FrameSync& s = sync[frame_index];
	const ZHLN_CommandPool& pool = pools[frame_index];
	const VkCommandBuffer cmd = pools.Cmd(frame_index);

	ZHLN_WaitAndResetFrame(ctx.Device(), s.in_flight, &pool);

	uint32_t image_index = 0;
	const ZHLN_AcquireDesc acquire_desc = {.swapchain = swapchain.Get().handle,
										   .image_available = s.image_available,
										   .timeout_ns = UINT64_MAX};
	auto result = ZHLN_AcquireImage(ctx.Device(), &acquire_desc, &image_index);
	if (result == ZHLN_FrameResult_OutOfDate) {
		rebuild();
		return result;
	}

	ZHLN_BeginCommandBuffer(cmd);
	std::invoke(std::forward<Record>(record), cmd, image_index);
	ZHLN_EndCommandBuffer(cmd);

	ZHLN_SubmitFrame(ctx.GraphicsQueue(), &s, cmd);

	const ZHLN_PresentDesc present_desc = {.present_queue = ctx.PresentQueue(),
										   .swapchain = swapchain.Get().handle,
										   .render_finished = s.render_finished,
										   .image_index = image_index};
	result = ZHLN_PresentFrame(&present_desc);
	if (result == ZHLN_FrameResult_OutOfDate || result == ZHLN_FrameResult_Suboptimal) {
		rebuild();
	}

	frame_index = (frame_index + 1) % N;
	return result;
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
	Surface(Surface&& other) noexcept
		: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
		  _handle(std::exchange(other._handle, VK_NULL_HANDLE)) {}
	Surface& operator=(Surface&& other) noexcept {
		if (this != &other) {
			if (_handle != VK_NULL_HANDLE) {
				vkDestroySurfaceKHR(_instance, _handle, nullptr);
			}
			_instance = std::exchange(other._instance, VK_NULL_HANDLE);
			_handle = std::exchange(other._handle, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] VkSurfaceKHR Get() const { return _handle; }

  private:
	VkInstance _instance = VK_NULL_HANDLE;
	VkSurfaceKHR _handle = VK_NULL_HANDLE;
};

// ============================================================================
// Semaphore Helpers
// ============================================================================

#include <algorithm>
#include <array>
#include <cstdlib>
#include <print>

// Perfect 64-byte structure (1 Cache Line)
class alignas(64) SemaphorePool {
  public:
	SemaphorePool() noexcept = default;
	~SemaphorePool() noexcept { Cleanup(); }

	// Move-only
	SemaphorePool(const SemaphorePool&) = delete;
	SemaphorePool& operator=(const SemaphorePool&) = delete;

	SemaphorePool(SemaphorePool&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _count(std::exchange(other._count, 0)) {
		std::ranges::move(other._semaphores, _semaphores.begin());
		other._semaphores.fill(VK_NULL_HANDLE);
	}

	SemaphorePool& operator=(SemaphorePool&& other) noexcept {
		if (this != &other) {
			Cleanup();
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_count = std::exchange(other._count, 0);
			std::ranges::move(other._semaphores, _semaphores.begin());
			other._semaphores.fill(VK_NULL_HANDLE);
		}
		return *this;
	}

	void Rebuild(const VkDevice device, const uint32_t count) noexcept {
		Cleanup();
		_device = device;
		// Cap at 6 to ensure 64-byte struct size
		_count = std::min(count, 6u);

		for (uint32_t i = 0; i < _count; ++i)
			_semaphores[i] = ZHLN_CreateSemaphore(_device);
	}

	[[nodiscard]] VkSemaphore operator[](const uint32_t index) const noexcept {
		// Hot path check: unlikely() keeps the branch away from the main logic flow
		if (index >= _count) [[unlikely]] {
			std::println(stderr,
						 "[ZHLN::Vk] FATAL: SemaphorePool index {} out of bounds (Size: {})", index,
						 _count);
			std::abort();
		}
		return _semaphores[index];
	}

	[[nodiscard]] uint32_t Count() const noexcept { return _count; }
	[[nodiscard]] bool Valid() const noexcept { return _device != VK_NULL_HANDLE; }

  private:
	void Cleanup() noexcept {
		if (_device == VK_NULL_HANDLE)
			return;

		// Locally cache device handle for the loop
		const auto d = _device;
		for (uint32_t i = 0; i < _count; ++i) {
			// Local null check prevents expensive driver thunk if slot is empty
			if (_semaphores[i]) {
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
// Error Helpers
// ============================================================================

[[nodiscard]] inline const char* ResultString(const VkResult result) noexcept {
	return ZHLN_VkResultString(result);
}

inline void CheckResult(const VkResult result, const char* context = "",
						const std::source_location location = std::source_location::current()) {
	if (result != VK_SUCCESS) {
		std::println(stderr, "[Vk Error] {}:{} in {}: {} failed with {}", location.file_name(),
					 location.line(), location.function_name(), context, ResultString(result));
	}
}

// ============================================================================
// Image View Helpers
// ============================================================================
template <VkFormat F> struct FormatTraits;

// Macro to avoid typing the same boilerplate for common formats
#define ZHLN_FORMAT_ASPECT(Format, Aspect)                                                         \
	template <> struct FormatTraits<Format> {                                                      \
		static constexpr VkImageAspectFlags aspect = Aspect;                                       \
	};

ZHLN_FORMAT_ASPECT(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_D24_UNORM_S8_UINT,
				   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)

using ImageView = DeviceHandle<VkImageView, vkDestroyImageView>;

template <VkFormat F>
[[nodiscard]] static ImageView CreateView(const VkDevice device, const VkImage image,
										  const uint32_t mips = 1) {
	ZHLN_ImageViewDesc desc = {.image = image,
							   .format = F,
							   .aspect = FormatTraits<F>::aspect, // Deduced via TMP!
							   .mip_levels = mips};
	return ImageView(device, ZHLN_CreateImageView(device, &desc));
}

} // namespace ZHLN::Vk