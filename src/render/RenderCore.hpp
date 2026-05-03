#pragma once

#include "RenderCore.h"

#include <Zahlen/Log.hpp>
#include <array>
#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

// ============================================================================
// RAII Handle
// Wraps any C-layer object with a typed deleter. Zero overhead — the deleter
// is a template parameter, so it's resolved at compile time with no indirection.
// ============================================================================

template <typename T, auto DeleterFn> class Handle {
  public:
	Handle() noexcept = default;

	explicit Handle(T raw) noexcept : _raw(raw) {}

	~Handle() noexcept {
		if (_raw != VK_NULL_HANDLE)
			DeleterFn(_raw);
	}

	// Non-copyable
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;

	// Movable
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

	// Release ownership without destroying — for passing back to C layer
	[[nodiscard]] T Release() noexcept { return std::exchange(_raw, VK_NULL_HANDLE); }

  private:
	T _raw = VK_NULL_HANDLE;
};

// ============================================================================
// Device-owned Handle
// Most Vulkan objects need both a VkDevice and the handle to destroy.
// Captures the device at construction so the deleter signature stays uniform.
// ============================================================================

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

// ============================================================================
// Concrete Handle Aliases
// Each maps to the matching ZHLN_Destroy* function.
// ============================================================================

using ShaderModule = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;

// Swapchain and CommandPool wrap structs rather than raw handles,
// so they get their own thin RAII wrappers below.

// ============================================================================
// Swapchain RAII
// ============================================================================

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

	// Rebuilds in-place, passing the old handle for driver memory reuse
	bool Rebuild(const ZHLN_SwapchainDesc& desc) noexcept {
		ZHLN_SwapchainDesc rebuilt = desc;
		rebuilt.old_swapchain = _raw.handle;
		ZHLN_Swapchain next = ZHLN_CreateSwapchain(&rebuilt);
		if (next.handle == VK_NULL_HANDLE)
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
// FrameSync RAII
// N is frame-in-flight count — a compile-time constant so the array lives
// on the stack and the frame index is bounds-checked at zero cost.
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
	FrameSync(FrameSync&&) = delete;
	FrameSync& operator=(FrameSync&&) = delete;

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

// ============================================================================
// CommandPool RAII
// Paired 1:1 with a FrameSync slot. N matches the frame-in-flight count.
// ============================================================================

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

	[[nodiscard]] static CommandPools Create(VkDevice device, uint32_t queue_family,
											 uint32_t buffers_per_pool = 1) noexcept {
		CommandPools cp;
		cp._device = device; // set early so destructor cleans up on partial failure
		for (auto& pool : cp._pools) {
			if (!ZHLN_CreateCommandPool(device, queue_family, &pool))
				return {};
			if (!ZHLN_AllocateCommandBuffers(device, &pool, buffers_per_pool))
				return {};
		}
		return cp;
	}

	[[nodiscard]] ZHLN_CommandPool& operator[](uint32_t frame) noexcept {
		return _pools[frame % N];
	}

	// Primary command buffer for a given frame
	[[nodiscard]] VkCommandBuffer Cmd(uint32_t frame) const noexcept {
		return _pools[frame % N].buffers[0];
	}

	[[nodiscard]] bool Valid() const noexcept { return _device != VK_NULL_HANDLE; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<ZHLN_CommandPool, N> _pools = {};
};

// ============================================================================
// Concepts
// Constrain template parameters that interact with the frame loop.
// ============================================================================

template <typename T>
concept RecordFn = std::invocable<T, VkCommandBuffer, uint32_t>;
// void record(VkCommandBuffer cmd, uint32_t image_index)

template <typename T>
concept RebuildFn = std::invocable<T>;
// void rebuild()

// ============================================================================
// Context
// Stable, non-rebuilding Vulkan state. Created once, passed by const ref.
// Swapchain is intentionally excluded — it rebuilds independently on resize.
// ============================================================================

struct Context {
    VkInstance              instance = VK_NULL_HANDLE;
    VkSurfaceKHR            surface  = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo physical = {};
    ZHLN_Device             device   = {};

    [[nodiscard]] VkDevice      Device()        const noexcept { return device.handle; }
    [[nodiscard]] VkQueue       GraphicsQueue() const noexcept { return device.graphics_queue; }
    [[nodiscard]] VkQueue       PresentQueue()  const noexcept { return device.present_queue; }
    [[nodiscard]] VkPhysicalDevice Physical()   const noexcept { return physical.handle; }
    [[nodiscard]] bool          Valid()         const noexcept { return device.handle != VK_NULL_HANDLE; }
};

// Named constructor — caller controls every policy decision.
// Returns an invalid Context on any failure; check .Valid() before use.
[[nodiscard]]
inline Context CreateContext(const ZHLN_InstanceDesc&     instance_desc,
                             const ZHLN_DeviceSelectDesc& select_desc,
                             const ZHLN_DeviceDesc&       device_desc) noexcept {
    Context ctx;

    ctx.instance = ZHLN_CreateInstance(&instance_desc);
    if (ctx.instance == VK_NULL_HANDLE) return {};

    ctx.surface  = select_desc.surface; // caller owns surface lifetime
    ctx.physical = ZHLN_SelectPhysicalDevice(&select_desc);
    if (ctx.physical.handle == VK_NULL_HANDLE) return {};

    ctx.device = ZHLN_CreateDevice(&device_desc);
    if (ctx.device.handle == VK_NULL_HANDLE) return {};

    return ctx;
}

// ============================================================================
// Frame
// Drives one iteration of the frame loop. Takes the record and rebuild
// callbacks as template parameters — resolved at compile time, zero overhead.
// ============================================================================

template <uint32_t N, RecordFn Record, RebuildFn Rebuild>
ZHLN_FrameResult DrawFrame(const Context&   ctx,
                           Swapchain&       swapchain,
                           FrameSync<N>&    sync,
                           CommandPools<N>& pools,
                           uint32_t&        frame_index,
                           Record&&         record,
                           Rebuild&&        rebuild) noexcept {

    const ZHLN_FrameSync& s    = sync[frame_index];
    ZHLN_CommandPool&     pool = pools[frame_index];
    VkCommandBuffer       cmd  = pools.Cmd(frame_index);

    ZHLN_WaitAndResetFrame(ctx.Device(), s.in_flight, &pool);

    uint32_t image_index = 0;
    ZHLN_AcquireDesc acquire_desc = {
        .swapchain       = swapchain.Get().handle,
        .image_available = s.image_available,
        .timeout_ns      = UINT64_MAX,
    };

    auto result = ZHLN_AcquireImage(ctx.Device(), &acquire_desc, &image_index);
    if (result == ZHLN_FrameResult_OutOfDate) { rebuild(); return result; }

    ZHLN_BeginCommandBuffer(cmd);
    std::invoke(std::forward<Record>(record), cmd, image_index);
    ZHLN_EndCommandBuffer(cmd);

    ZHLN_SubmitFrame(ctx.GraphicsQueue(), &s, cmd);

    ZHLN_PresentDesc present_desc = {
        .present_queue   = ctx.PresentQueue(),
        .swapchain       = swapchain.Get().handle,
        .render_finished = s.render_finished,
        .image_index     = image_index,
    };
    result = ZHLN_PresentFrame(&present_desc);
    if (result == ZHLN_FrameResult_OutOfDate ||
        result == ZHLN_FrameResult_Suboptimal) rebuild();

    frame_index = (frame_index + 1) % N;
    return result;
}

// ============================================================================
// Push constant helpers
// Typed wrappers around ZHLN_Push so the C++ side stays macro-free.
// ============================================================================

template <typename T>
	requires std::is_trivially_copyable_v<T>
inline void Push(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
				 const T& value) noexcept {
	ZHLN_PushConstants(cmd, layout, stages, &value, sizeof(T));
}

// ============================================================================
// Error helpers
// ============================================================================

[[nodiscard]] inline const char* ResultString(VkResult result) noexcept {
	return ZHLN_VkResultString(result);
}

inline void CheckResult(VkResult result, const char* context = "") {
	if (result != VK_SUCCESS)
		Log("[Vk] {} failed: {}\n", context, ResultString(result));
}

} // namespace ZHLN::Vk