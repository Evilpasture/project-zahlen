#pragma once

#include "RenderCore.h"

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
		if (_raw != VK_NULL_HANDLE)
			DeleterFn(_raw);
	}

	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;

	Handle(Handle&& other) noexcept : _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}
	Handle& operator=(Handle&& other) noexcept {
		if (this != &other) {
			if (_raw != VK_NULL_HANDLE)
				DeleterFn(_raw);
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
	DeviceHandle(VkDevice device, T raw) noexcept : _device(device), _raw(raw) {}

	~DeviceHandle() noexcept {
		if (_raw != VK_NULL_HANDLE)
			DeleterFn(_device, _raw);
	}

	DeviceHandle(const DeviceHandle&) = delete;
	DeviceHandle& operator=(const DeviceHandle&) = delete;

	DeviceHandle(DeviceHandle&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

	DeviceHandle& operator=(DeviceHandle&& other) noexcept {
		if (this != &other) {
			if (_raw != VK_NULL_HANDLE)
				DeleterFn(_device, _raw);
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, VK_NULL_HANDLE);
		}
		return *this;
	}

	[[nodiscard]] T Get() const noexcept { return _raw; }
	[[nodiscard]] bool Valid() const noexcept { return _raw != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }
	[[nodiscard]] T Release() noexcept { return std::exchange(_raw, VK_NULL_HANDLE); }

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
		if (_device.handle != VK_NULL_HANDLE)
			vkDestroyDevice(_device.handle, nullptr);
		if (_instance != VK_NULL_HANDLE)
			vkDestroyInstance(_instance, nullptr);
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
			if (_device.handle != VK_NULL_HANDLE)
				vkDestroyDevice(_device.handle, nullptr);
			if (_instance != VK_NULL_HANDLE)
				vkDestroyInstance(_instance, nullptr);

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
		ctx._instance = ZHLN_CreateInstance(&instance_desc);

		// CRITICAL FIX: Bail out if instance creation failed
		if (ctx._instance == VK_NULL_HANDLE) {
			return {};
		}

		ctx._surface = select_desc.surface;

		ZHLN_DeviceSelectDesc safe_select = select_desc;
		safe_select.instance = ctx._instance;
		ctx._physical = ZHLN_SelectPhysicalDevice(&safe_select);

		// CRITICAL FIX: Bail out if no GPU found
		if (ctx._physical.handle == VK_NULL_HANDLE) {
			return {};
		}

		ZHLN_DeviceDesc safe_device = device_desc;
		safe_device.physical = &ctx._physical;
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

[[nodiscard]] inline SwapchainSupport QuerySwapchainSupport(VkPhysicalDevice physical,
															VkSurfaceKHR surface) noexcept {
	ZHLN_SwapchainSupportDesc desc = {.physical = physical, .surface = surface};
	return {ZHLN_QuerySwapchainSupport(&desc)};
}

class Swapchain {
  public:
	Swapchain() noexcept = default;
	Swapchain(VkDevice device, ZHLN_Swapchain raw) noexcept : _device(device), _raw(raw) {}

	~Swapchain() noexcept { _Destroy(); }

	Swapchain(const Swapchain&) = delete;
	Swapchain& operator=(const Swapchain&) = delete;

	Swapchain(Swapchain&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, {})) {}

	Swapchain& operator=(Swapchain&& other) noexcept {
		if (this != &other) {
			_Destroy();
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	[[nodiscard]] const ZHLN_Swapchain& Get() const noexcept { return _raw; }
	[[nodiscard]] bool Valid() const noexcept { return _raw.handle != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }

	bool Rebuild(const ZHLN_SwapchainDesc& desc) noexcept {
		ZHLN_SwapchainDesc rebuilt = desc;
		rebuilt.old_swapchain = _raw.handle;
		ZHLN_Swapchain next = ZHLN_CreateSwapchain(&rebuilt);
		if (!next.handle)
			return false;
		_Destroy();
		_raw = next;
		return true;
	}

  private:
	void _Destroy() noexcept {
		if (_raw.handle != VK_NULL_HANDLE)
			ZHLN_DestroySwapchain(_device, &_raw);
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
		if (_device != VK_NULL_HANDLE)
			ZHLN_DestroyFrameSync(_device, _frames.data(), N);
	}
	FrameSync(const FrameSync&) = delete;
	FrameSync& operator=(const FrameSync&) = delete;

	FrameSync(FrameSync&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _frames(std::exchange(other._frames, {})) {}

	FrameSync& operator=(FrameSync&& other) noexcept {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE)
				ZHLN_DestroyFrameSync(_device, _frames.data(), N);
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_frames = std::exchange(other._frames, {});
		}
		return *this;
	}

	[[nodiscard]] static FrameSync Create(VkDevice device) noexcept {
		FrameSync fs;
		ZHLN_FrameSyncDesc desc = {.device = device, .frame_count = N};
		if (!ZHLN_CreateFrameSync(&desc, fs._frames.data()))
			return {};
		fs._device = device;
		return fs;
	}

	[[nodiscard]] const ZHLN_FrameSync& operator[](uint32_t frame) const noexcept {
		return _frames[frame % N];
	}
	[[nodiscard]] static constexpr uint32_t Count() noexcept { return N; }
	[[nodiscard]] bool Valid() const noexcept { return _device != VK_NULL_HANDLE; }

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
		if (_device != VK_NULL_HANDLE)
			for (auto& pool : _pools)
				ZHLN_DestroyCommandPool(_device, &pool);
	}
	CommandPools(const CommandPools&) = delete;
	CommandPools& operator=(const CommandPools&) = delete;

	CommandPools(CommandPools&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _pools(std::exchange(other._pools, {})) {}

	CommandPools& operator=(CommandPools&& other) noexcept {
		if (this != &other) {
			if (_device != VK_NULL_HANDLE) {
				for (auto& pool : _pools)
					ZHLN_DestroyCommandPool(_device, &pool);
			}
			_device = std::exchange(other._device, VK_NULL_HANDLE);
			_pools = std::exchange(other._pools, {});
		}
		return *this;
	}

	[[nodiscard]] static CommandPools Create(VkDevice device, uint32_t queue_family,
											 uint32_t buffers_per_pool = 1) noexcept {
		CommandPools cp;
		cp._device = device;
		for (auto& pool : cp._pools) {
			if (!ZHLN_CreateCommandPool(device, queue_family, &pool) ||
				!ZHLN_AllocateCommandBuffers(device, &pool, buffers_per_pool))
				return {};
		}
		return cp;
	}

	[[nodiscard]] ZHLN_CommandPool& operator[](uint32_t frame) noexcept {
		return _pools[frame % N];
	}
	[[nodiscard]] VkCommandBuffer Cmd(uint32_t frame) const noexcept {
		return _pools[frame % N].buffers[0];
	}
	[[nodiscard]] bool Valid() const noexcept { return _device != VK_NULL_HANDLE; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<ZHLN_CommandPool, N> _pools = {};
};

class CommandPool {
  public:
	CommandPool() = default;

	CommandPool(VkDevice device, uint32_t queue_family) : _device(nullptr) {
		// Fix: Capture the return value.
		// Only assign _device if the pool was actually created.
		if (ZHLN_CreateCommandPool(device, queue_family, &_raw)) {
			_device = device;
		}
	}

	~CommandPool() {
		if (_device)
			ZHLN_DestroyCommandPool(_device, &_raw);
	}

	// Move only
	CommandPool(CommandPool&& other) noexcept
		: _device(std::exchange(other._device, nullptr)), _raw(std::exchange(other._raw, {})) {}

	CommandPool& operator=(CommandPool&& other) noexcept {
		if (this != &other) {
			if (_device)
				ZHLN_DestroyCommandPool(_device, &_raw);
			_device = std::exchange(other._device, nullptr);
			_raw = std::exchange(other._raw, {});
		}
		return *this;
	}

	[[nodiscard]] bool Valid() const noexcept { return _device != nullptr; }
	explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] bool Allocate(uint32_t count) {
		if (!Valid())
			return false;
		return ZHLN_AllocateCommandBuffers(_device, &_raw, count);
	}

	VkCommandBuffer operator[](uint32_t i) const { return _raw.buffers[i]; }

  private:
	VkDevice _device = nullptr;
	ZHLN_CommandPool _raw{};
};

// ============================================================================
// ShaderStages RAII
// ============================================================================

class ShaderStages {
  public:
	ShaderStages() noexcept = default;
	ShaderStages(VkDevice device, ZHLN_ShaderStages raw) noexcept : _device(device), _raw(raw) {}
	~ShaderStages() noexcept {
		if (_device != VK_NULL_HANDLE)
			ZHLN_DestroyShaderStages(_device, &_raw);
	}
	ShaderStages(const ShaderStages&) = delete;
	ShaderStages& operator=(const ShaderStages&) = delete;
	ShaderStages(ShaderStages&& other) noexcept
		: _device(std::exchange(other._device, VK_NULL_HANDLE)),
		  _raw(std::exchange(other._raw, {})) {}

	[[nodiscard]] static ShaderStages Create(VkDevice device, const ZHLN_ShaderDesc& vert,
											 const ZHLN_ShaderDesc& frag) noexcept {
		ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
		ZHLN_ShaderStages stages{};
		if (!ZHLN_CreateShaderStages(&desc, &stages))
			return {};
		return {device, stages};
	}

	[[nodiscard]] const ZHLN_ShaderStages* Get() const noexcept { return &_raw; }
	[[nodiscard]] bool Valid() const noexcept { return _raw.vert.handle != VK_NULL_HANDLE; }

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
	ScopedRendering(VkCommandBuffer cmd, const ZHLN_RenderPassDesc& desc) noexcept : _cmd(cmd) {
		ZHLN_BeginRendering(_cmd, &desc);
	}
	~ScopedRendering() noexcept { ZHLN_EndRendering(_cmd); }

	ScopedRendering(const ScopedRendering&) = delete;
	ScopedRendering& operator=(const ScopedRendering&) = delete;

  private:
	VkCommandBuffer _cmd;
};

inline void ImageBarrier(VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept {
	ZHLN_CmdImageBarrier(cmd, &desc);
}

inline void TransitionLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
							 VkImageLayout new_layout,
							 VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept {
	VkAccessFlags2 src_access = 0;
	VkPipelineStageFlags2 src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

	if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	} else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		src_access = VK_ACCESS_2_SHADER_READ_BIT;
		src_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	} else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	} else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		src_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
					VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	}

	ZHLN_ImageBarrierDesc barrier = {
		.image = image,
		.src_access = src_access,
		.dst_access = 0,
		.src_layout = old_layout,
		.dst_layout = new_layout,
		.src_stage = src_stage,
		.dst_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
		.aspect = aspect,
	};

	if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	} else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.dst_access = VK_ACCESS_2_SHADER_READ_BIT;
		barrier.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	} else if (new_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
		barrier.dst_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barrier.dst_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
			VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	}
	ZHLN_CmdImageBarrier(cmd, &barrier);
}

inline void CopyBufferToImage(VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc& desc) noexcept {
	ZHLN_CmdCopyBufferToImage(cmd, &desc);
}

// Typed wrapper around Push Constants
template <GpuTriviallyCopyable T>
inline void Push(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
				 const T& value) noexcept {
	ZHLN_PushConstants(cmd, layout, stages, &value, sizeof(T));
}

// ============================================================================
// Frame Execution
// ============================================================================

template <uint32_t N, RecordFn Record, RebuildFn Rebuild>
ZHLN_FrameResult DrawFrame(const Context& ctx, Swapchain& swapchain, FrameSync<N>& sync,
						   CommandPools<N>& pools, uint32_t& frame_index, Record&& record,
						   Rebuild&& rebuild) noexcept {

	const ZHLN_FrameSync& s = sync[frame_index];
	ZHLN_CommandPool& pool = pools[frame_index];
	VkCommandBuffer cmd = pools.Cmd(frame_index);

	ZHLN_WaitAndResetFrame(ctx.Device(), s.in_flight, &pool);

	uint32_t image_index = 0;
	ZHLN_AcquireDesc acquire_desc = {.swapchain = swapchain.Get().handle,
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

	ZHLN_PresentDesc present_desc = {.present_queue = ctx.PresentQueue(),
									 .swapchain = swapchain.Get().handle,
									 .render_finished = s.render_finished,
									 .image_index = image_index};
	result = ZHLN_PresentFrame(&present_desc);
	if (result == ZHLN_FrameResult_OutOfDate || result == ZHLN_FrameResult_Suboptimal)
		rebuild();

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
		if (_handle != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(_instance, _handle, nullptr);
	}

	// Move only
	Surface(const Surface&) = delete;
	Surface(Surface&& other) noexcept
		: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
		  _handle(std::exchange(other._handle, VK_NULL_HANDLE)) {}
	Surface& operator=(Surface&& other) noexcept {
		if (this != &other) {
			if (_handle != VK_NULL_HANDLE)
				vkDestroySurfaceKHR(_instance, _handle, nullptr);
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

// Add a helper for the "One Semaphore Per Swapchain Image" pattern
class SemaphorePool {
public:
    SemaphorePool() = default;
    
    void Rebuild(VkDevice device, uint32_t count) {
        _semaphores.clear(); // RAII handles destroy themselves
        _semaphores.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            _semaphores.emplace_back(device, ZHLN_CreateSemaphore(device));
        }
    }

    VkSemaphore operator[](uint32_t index) const {
        return _semaphores[index % _semaphores.size()].Get();
    }

    size_t Size() const { return _semaphores.size(); }

private:
    std::vector<Semaphore> _semaphores;
};

// ============================================================================
// Error Helpers
// ============================================================================

[[nodiscard]] inline const char* ResultString(VkResult result) noexcept {
	return ZHLN_VkResultString(result);
}

inline void CheckResult(VkResult result, const char* context = "",
						const std::source_location location = std::source_location::current()) {
	if (result != VK_SUCCESS) {
		std::print(stderr, "[Vk Error] {}:{} in {}: {} failed with {}\n", location.file_name(),
				   location.line(), location.function_name(), context, ResultString(result));
	}
}

} // namespace ZHLN::Vk