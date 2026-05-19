#pragma once

#include "RenderCore.h"
#include "Utils.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <print>
#include <source_location>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

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

	[[nodiscard]] bool AllocateSecondary(const uint32_t count) {
		if (!Valid())
			return false;
		return ZHLN_AllocateSecondaryCommandBuffers(_device, &_raw, count);
	}

	void Reset() noexcept {
		if (Valid()) {
			ZHLN_ResetCommandPool(_device, &_raw);
		}
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

	[[nodiscard]] static ShaderStages FromFiles(const VkDevice device,
												const std::filesystem::path& vert_path,
												const std::filesystem::path& frag_path,
												const char* vert_entry = "main",
												const char* frag_entry = "main") noexcept {
		auto load = [](const std::filesystem::path& path) -> std::vector<uint32_t> {
			std::ifstream file(path, std::ios::ate | std::ios::binary);
			if (!file.is_open()) {
				return {};
			}
			const std::streamsize size = file.tellg();
			std::vector<uint32_t> buffer(size / sizeof(uint32_t));
			file.seekg(0);
			file.read(reinterpret_cast<char*>(buffer.data()), size);
			return buffer;
		};

		auto vert_spv = load(vert_path);
		auto frag_spv = load(frag_path);

		if (vert_spv.empty() || frag_spv.empty()) {
			std::println(stderr, "[ZHLN::Vk] Failed to load shader files: {} or {}",
						 vert_path.string(), frag_path.string());
			return {};
		}

		const ZHLN_ShaderDesc v_desc = {
			.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = vert_entry};
		const ZHLN_ShaderDesc f_desc = {
			.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = frag_entry};

		return Create(device, v_desc, f_desc);
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
inline ZHLN_FrameResult DrawFrame(const Context& ctx, const Swapchain& swapchain,
								  const FrameSync<N>& sync, const CommandPools<N>& pools,
								  uint32_t& frame_index, Record&& record,
								  Rebuild&& rebuild) noexcept {

	const ZHLN_FrameSync& s = sync[frame_index];
	const ZHLN_CommandPool& pool = pools[frame_index];
	const VkCommandBuffer cmd = pools.Cmd(frame_index);

	// 1. Synchronize: Wait for this frame's previous command buffer to finish
	ZHLN_WaitAndResetFrame(ctx.Device(), s.in_flight, &pool);

	// 2. Acquire: Get next image from swapchain
	uint32_t image_index = 0;
	const ZHLN_AcquireDesc acquire_desc = {.swapchain = swapchain.Get().handle,
										   .image_available = s.image_available,
										   .timeout_ns = UINT64_MAX};

	auto result = ZHLN_AcquireImage(ctx.Device(), &acquire_desc, &image_index);
	if (result == ZHLN_FrameResult_OutOfDate) {
		std::invoke(std::forward<Rebuild>(rebuild));
		return result;
	}

	// 3. Record: perfectly forward the callable to ensure inlining
	ZHLN_BeginCommandBuffer(cmd);
	std::invoke(std::forward<Record>(record), cmd, image_index);
	ZHLN_EndCommandBuffer(cmd);

	// 4. Submit
	ZHLN_SubmitFrame(ctx.GraphicsQueue(), &s, cmd);

	// 5. Present
	const ZHLN_PresentDesc present_desc = {.present_queue = ctx.PresentQueue(),
										   .swapchain = swapchain.Get().handle,
										   .render_finished = s.render_finished,
										   .image_index = image_index};

	result = ZHLN_PresentFrame(&present_desc);
	if (result == ZHLN_FrameResult_OutOfDate || result == ZHLN_FrameResult_Suboptimal)
		[[unlikely]] {
		std::invoke(std::forward<Rebuild>(rebuild));
	}

	// --- 6. Optimized Frame Index Increment ---
	if constexpr ((N & (N - 1)) == 0) {
		// Optimization for power-of-two N (e.g., 2, 4, 8)
		// Uses bitwise AND instead of division/modulo
		frame_index = (frame_index + 1) & (N - 1);
	} else if constexpr (N == 3) {
		// Optimization for common Triple Buffering case
		// Avoids division entirely with a simple comparison
		frame_index = (frame_index == 2) ? 0 : frame_index + 1;
	} else {
		// Fallback for non-standard counts
		frame_index = (frame_index + 1) % N;
	}

	return result;
}

[[nodiscard]]
inline ZHLN_FrameResult SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept {
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

// Perfect 64-byte structure (1 Cache Line)
class alignas(64) SemaphorePool {
  public:
	SemaphorePool() noexcept = default;
	~SemaphorePool() noexcept { Cleanup(); }

	// Move-only
	SemaphorePool(const SemaphorePool&) = delete;
	SemaphorePool& operator=(const SemaphorePool&) = delete;

	SemaphorePool(SemaphorePool&& other) noexcept : _device(other._device), _count(other._count) {
		for (uint32_t i = 0; i < 6; ++i) {
			_semaphores[i] = other._semaphores[i];
			other._semaphores[i] = VK_NULL_HANDLE;
		}
		other._device = VK_NULL_HANDLE;
		other._count = 0;
	}

	SemaphorePool& operator=(SemaphorePool&& other) noexcept {
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
		if (_device == VK_NULL_HANDLE) {
			return;
		}

		// Locally cache device handle for the loop
		auto* const d = _device;
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
ZHLN_FORMAT_ASPECT(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT)
ZHLN_FORMAT_ASPECT(VK_FORMAT_D24_UNORM_S8_UINT,
				   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)

using ImageView = DeviceHandle<VkImageView, ZHLN_DestroyImageView>;

template <VkFormat F>
[[nodiscard]] static ImageView CreateView(VkDevice device, VkImage image,
										  VkImageAspectFlags aspect = FormatTraits<F>::aspect,
										  uint32_t mips = 1) {
	ZHLN_ImageViewDesc desc = {.image = image, .format = F, .aspect = aspect, .mip_levels = mips};

	// Braced initializer list avoids repeating 'ImageView'
	return {device, ZHLN_CreateImageView(device, &desc)};
}

using Sampler = DeviceHandle<VkSampler, ZHLN_DestroySampler>;
using DescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, ZHLN_DestroyDescriptorSetLayout>;
using DescriptorPool = DeviceHandle<VkDescriptorPool, ZHLN_DestroyDescriptorPool>;

// ============================================================================
// Extension Query Utilities
// ============================================================================

[[nodiscard]] inline bool IsInstanceExtensionSupported(std::string_view extension) noexcept {
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);

	// Most systems have < 128 extensions.
	// VkExtensionProperties is ~260 bytes, so 128 * 260 = ~33KB on stack.
	VkExtensionProperties available[128];
	if (count > 128)
		count = 128; // Cap to buffer size

	vkEnumerateInstanceExtensionProperties(nullptr, &count, available);

	for (uint32_t i = 0; i < count; ++i) {
		if (extension == available[i].extensionName)
			return true;
	}
	return false;
}

[[nodiscard]] inline bool IsDeviceExtensionSupported(VkPhysicalDevice physical,
													 std::string_view extension) noexcept {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);

	VkExtensionProperties available[128];
	if (count > 128)
		count = 128;

	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available);

	for (uint32_t i = 0; i < count; ++i) {
		if (extension == available[i].extensionName)
			return true;
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
				heap_allocated = new VkImageMemoryBarrier2[transition_count];
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
template <uint32_t Width, uint32_t Height> consteval uint32_t GetMipLevels() noexcept {
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

} // namespace ZHLN::Vk
