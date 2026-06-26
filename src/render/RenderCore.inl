#pragma once

#include "RenderCore.hpp"
#include "Utils.hpp"

#include <functional>
#include <new>
#include <utility>
// NOLINTBEGIN(misc-misplaced-const)

namespace ZHLN {

// ============================================================================
// DoubleBuffered Implementation
// ============================================================================

template <typename T> inline auto DoubleBuffered<T>::Current() noexcept -> T& {
	return _data[_index];
}
template <typename T> inline auto DoubleBuffered<T>::Current() const noexcept -> const T& {
	return _data[_index];
}
template <typename T> inline auto DoubleBuffered<T>::Next() noexcept -> T& {
	return _data[1 - _index];
}
template <typename T> inline auto DoubleBuffered<T>::Next() const noexcept -> const T& {
	return _data[1 - _index];
}
template <typename T> inline auto DoubleBuffered<T>::operator[](uint32_t idx) noexcept -> T& {
	return _data[idx % 2];
}
template <typename T>
inline auto DoubleBuffered<T>::operator[](uint32_t idx) const noexcept -> const T& {
	return _data[idx % 2];
}
template <typename T> inline void DoubleBuffered<T>::Flip() noexcept {
	_index = 1 - _index;
}

} // namespace ZHLN

namespace ZHLN::Vk {

// ============================================================================
// Handle & DeviceHandle Implementation
// ============================================================================

template <typename T, auto DeleterFn>
inline Handle<T, DeleterFn>::Handle(T raw) noexcept : _raw(raw) {}

template <typename T, auto DeleterFn> inline Handle<T, DeleterFn>::~Handle() noexcept {
	if (_raw != VK_NULL_HANDLE) {
		DeleterFn(_raw);
	}
}

template <typename T, auto DeleterFn>
inline Handle<T, DeleterFn>::Handle(Handle&& other) noexcept
	: _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

template <typename T, auto DeleterFn>
inline auto Handle<T, DeleterFn>::operator=(Handle&& other) noexcept -> Handle& {
	if (this != &other) {
		if (_raw != VK_NULL_HANDLE) {
			DeleterFn(_raw);
		}
		_raw = std::exchange(other._raw, VK_NULL_HANDLE);
	}
	return *this;
}

template <typename T, auto DeleterFn> inline auto Handle<T, DeleterFn>::Get() const noexcept -> T {
	return _raw;
}

template <typename T, auto DeleterFn>
inline auto Handle<T, DeleterFn>::Valid() const noexcept -> bool {
	return _raw != VK_NULL_HANDLE;
}

template <typename T, auto DeleterFn> inline Handle<T, DeleterFn>::operator bool() const noexcept {
	return Valid();
}

template <typename T, auto DeleterFn> inline auto Handle<T, DeleterFn>::Release() noexcept -> T {
	return std::exchange(_raw, VK_NULL_HANDLE);
}

template <typename T, auto DeleterFn>
inline DeviceHandle<T, DeleterFn>::DeviceHandle(const VkDevice device, const T raw) noexcept
	: _device(device), _raw(raw) {}

template <typename T, auto DeleterFn> inline DeviceHandle<T, DeleterFn>::~DeviceHandle() noexcept {
	if (_raw != VK_NULL_HANDLE) {
		DeleterFn(_device, _raw);
	}
}

template <typename T, auto DeleterFn>
inline DeviceHandle<T, DeleterFn>::DeviceHandle(DeviceHandle&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)),
	  _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

template <typename T, auto DeleterFn>
inline auto DeviceHandle<T, DeleterFn>::operator=(DeviceHandle&& other) noexcept -> DeviceHandle& {
	if (this != &other) {
		if (_raw != VK_NULL_HANDLE) {
			DeleterFn(_device, _raw);
		}
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_raw = std::exchange(other._raw, VK_NULL_HANDLE);
	}
	return *this;
}

template <typename T, auto DeleterFn>
constexpr auto DeviceHandle<T, DeleterFn>::Get() const noexcept -> T {
	return _raw;
}

template <typename T, auto DeleterFn>
constexpr auto DeviceHandle<T, DeleterFn>::Valid() const noexcept -> bool {
	return _raw != VK_NULL_HANDLE;
}

template <typename T, auto DeleterFn>
constexpr DeviceHandle<T, DeleterFn>::operator bool() const noexcept {
	return Valid();
}

template <typename T, auto DeleterFn>
constexpr auto DeviceHandle<T, DeleterFn>::Release() noexcept -> T {
	return std::exchange(_raw, VK_NULL_HANDLE);
}

// ============================================================================
// Context Implementation
// ============================================================================

inline Context::~Context() noexcept {
	if (_device.handle != VK_NULL_HANDLE) {
		vkDestroyDevice(_device.handle, nullptr);
	}
	if (_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(_instance, nullptr);
	}
}

inline Context::Context(Context&& other) noexcept
	: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
	  _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
	  _physical(std::exchange(other._physical, {})), _device(std::exchange(other._device, {})) {}

inline auto Context::operator=(Context&& other) noexcept -> Context& {
	if (this != &other) {
		if (_device.handle != VK_NULL_HANDLE) {
			vkDestroyDevice(_device.handle, nullptr);
		}
		if (_instance != VK_NULL_HANDLE) {
			vkDestroyInstance(_instance, nullptr);
		}

		_instance = std::exchange(other._instance, VK_NULL_HANDLE);
		_surface = std::exchange(other._surface, VK_NULL_HANDLE);
		_physical = other._physical;
		_device = other._device;

		other._physical = {};
		other._device = {};
	}
	return *this;
}

inline auto Context::Create(const ZHLN_InstanceDesc& instance_desc,
							const ZHLN_DeviceSelectDesc& select_desc,
							const ZHLN_DeviceDesc& device_desc) noexcept -> Context {
	Context ctx;

	ctx._instance = ZHLN_CreateInstance(&instance_desc);
	if (ctx._instance == VK_NULL_HANDLE) {
		return {};
	}

	ctx._surface = select_desc.surface;

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

inline auto Context::Create(VkInstance instance, VkSurfaceKHR surface,
							const ZHLN_PhysicalDeviceInfo& physical,
							const ZHLN_DeviceDesc& device_desc) noexcept -> Context {
	Context ctx;
	ctx._instance = instance;
	ctx._surface = surface;
	ctx._physical = physical;

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

// ============================================================================
// SwapchainSupport & Swapchain Implementation
// ============================================================================

inline auto SwapchainSupport::Formats() const noexcept -> std::span<const VkSurfaceFormatKHR> {
	return {raw.formats, raw.format_count};
}

inline auto SwapchainSupport::PresentModes() const noexcept -> std::span<const VkPresentModeKHR> {
	return {raw.present_modes, raw.present_mode_count};
}

inline SwapchainSupport QuerySwapchainSupport(const VkPhysicalDevice physical,
											  const VkSurfaceKHR surface) noexcept {
	const ZHLN_SwapchainSupportDesc desc = {.physical = physical, .surface = surface};
	return {ZHLN_QuerySwapchainSupport(&desc)};
}

inline Swapchain::Swapchain(const VkDevice device, const ZHLN_Swapchain raw) noexcept
	: _device(device), _raw(raw) {}

inline Swapchain::~Swapchain() noexcept {
	Destroy();
}

inline Swapchain::Swapchain(Swapchain&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, {})) {}

inline auto Swapchain::operator=(Swapchain&& other) noexcept -> Swapchain& {
	if (this != &other) {
		Destroy();
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_raw = std::exchange(other._raw, {});
	}
	return *this;
}

inline auto Swapchain::Rebuild(const ZHLN_SwapchainDesc& desc) noexcept -> bool {
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

inline void Swapchain::Destroy() noexcept {
	if (_raw.handle != VK_NULL_HANDLE) {
		ZHLN_DestroySwapchain(_device, &_raw);
	}
}

// ============================================================================
// Sync & Pools Implementation
// ============================================================================

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline FrameSync<N>::~FrameSync() noexcept {
	if (_device != VK_NULL_HANDLE) {
		ZHLN_DestroyFrameSync(_device, _frames.data(), N);
	}
}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline FrameSync<N>::FrameSync(FrameSync&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)),
	  _frames(std::exchange(other._frames, {})) {}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline auto FrameSync<N>::operator=(FrameSync&& other) noexcept -> FrameSync& {
	if (this != &other) {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyFrameSync(_device, _frames.data(), N);
		}
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_frames = std::exchange(other._frames, {});
	}
	return *this;
}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline auto FrameSync<N>::Create(const VkDevice device) noexcept -> FrameSync {
	FrameSync fs;
	const ZHLN_FrameSyncDesc desc = {.device = device, .frame_count = N};
	if (!ZHLN_CreateFrameSync(&desc, fs._frames.data())) {
		return {};
	}
	fs._device = device;
	return fs;
}

inline CommandPool::CommandPool(const VkDevice device, const uint32_t queue_family) {
	if (ZHLN_CreateCommandPool(device, queue_family, &_raw)) {
		_device = device;
	}
}

inline CommandPool::~CommandPool() {
	if (_device != VK_NULL_HANDLE) {
		ZHLN_DestroyCommandPool(_device, &_raw);
	}
}

constexpr CommandPool::CommandPool(CommandPool&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, {})) {}

inline auto CommandPool::operator=(CommandPool&& other) noexcept -> CommandPool& {
	if (this != &other) {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyCommandPool(_device, &_raw);
		}
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_raw = std::exchange(other._raw, {});
	}
	return *this;
}

inline auto CommandPool::Allocate(const uint32_t count) -> bool {
	if (!Valid()) {
		return false;
	}
	return ZHLN_AllocateCommandBuffers(_device, &_raw, count);
}

inline auto CommandPool::AllocateSecondary(const uint32_t count) -> bool {
	if (!Valid()) {
		return false;
	}
	return ZHLN_AllocateSecondaryCommandBuffers(_device, &_raw, count);
}

inline void CommandPool::Reset() noexcept {
	if (Valid()) {
		ZHLN_ResetCommandPool(_device, &_raw);
	}
}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline auto CommandPools<N>::Create(const VkDevice device, const Description& desc) noexcept
	-> CommandPools {
	CommandPools cp;
	for (auto& pool : cp._pools) {
		pool = CommandPool(device, desc.queue_family);
		if (!pool || !pool.Allocate(desc.buffers_per_pool)) {
			return {};
		}
	}
	return cp;
}

// ============================================================================
// Command & Rendering Helpers Implementation
// ============================================================================

inline ScopedRendering::ScopedRendering(const VkCommandBuffer cmd,
										const ZHLN_RenderPassDesc& desc) noexcept
	: _cmd(cmd) {
	ZHLN_BeginRendering(_cmd, &desc);
}

inline ScopedRendering::~ScopedRendering() noexcept {
	ZHLN_EndRendering(_cmd);
}

inline void ImageBarrier(const VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc& desc) noexcept {
	ZHLN_CmdImageBarrier(cmd, &desc);
}

inline void MemoryBarrier(const VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc& desc) noexcept {
	ZHLN_CmdMemoryBarrier(cmd, &desc);
}

inline auto GetBufferDeviceAddress(VkDevice device, VkBuffer buffer) noexcept -> VkDeviceAddress {
	return ZHLN_GetBufferDeviceAddress(device, buffer);
}

inline void
RayTracingContext::GetBlasSizes(const ZHLN_BlasGeometryDesc& desc, uint32_t primCount,
								ZHLN_AccelerationStructureSizes& outSizes) const noexcept {
	ZHLN_GetBlasSizes(&_raw, &desc, primCount, &outSizes);
}

inline void
RayTracingContext::GetTlasSizes(uint32_t instanceCount,
								ZHLN_AccelerationStructureSizes& outSizes) const noexcept {
	ZHLN_GetTlasSizes(&_raw, instanceCount, &outSizes);
}

inline VkAccelerationStructureKHR
RayTracingContext::CreateAS(VkBuffer buffer, VkDeviceSize size,
							ZHLN_AccelerationStructureType type) const noexcept {
	return ZHLN_CreateAS(&_raw, buffer, size, type);
}

inline void RayTracingContext::DestroyAS(VkAccelerationStructureKHR as) const noexcept {
	ZHLN_DestroyAS(&_raw, as);
}

inline VkDeviceAddress
RayTracingContext::GetASAddress(VkAccelerationStructureKHR as) const noexcept {
	return ZHLN_GetASAddress(&_raw, as);
}

inline void RayTracingContext::CmdBuildBlas(VkCommandBuffer cmd, const ZHLN_BlasGeometryDesc& desc,
											VkAccelerationStructureKHR dst, VkDeviceAddress scratch,
											uint32_t primCount) const noexcept {
	ZHLN_CmdBuildBlas(&_raw, cmd, &desc, dst, scratch, primCount);
}

inline void RayTracingContext::CmdBuildTlas(VkCommandBuffer cmd, const ZHLN_TlasGeometryDesc& desc,
											VkAccelerationStructureKHR dst, VkDeviceAddress scratch,
											uint32_t instanceCount) const noexcept {
	ZHLN_CmdBuildTlas(&_raw, cmd, &desc, dst, scratch, instanceCount);
}

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_UNDEFINED> {
	static constexpr VkAccessFlags2 access = 0;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> {
	static constexpr VkAccessFlags2 access =
		VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_SHADER_READ_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_NONE;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
											 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage =
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL> {
	static constexpr VkAccessFlags2 access = VK_ACCESS_2_TRANSFER_READ_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
};

template <> struct LayoutTraits<VK_IMAGE_LAYOUT_GENERAL> {
	static constexpr VkAccessFlags2 access =
		VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	static constexpr VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
};

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void TransitionLayout(const VkCommandBuffer cmd, const VkImage image,
							 const VkImageAspectFlags aspect) noexcept {

	using Src = LayoutTraits<OldLayout>;
	using Dst = LayoutTraits<NewLayout>;

	const ZHLN_ImageBarrierDesc barrier = {.image = image,
										   .src_access = Src::access,
										   .dst_access = Dst::access,
										   .src_layout = OldLayout,
										   .dst_layout = NewLayout,
										   .src_stage = Src::stage,
										   .dst_stage = Dst::stage,
										   .aspect = aspect,
										   .base_mip = 0,
										   .mip_count = VK_REMAINING_MIP_LEVELS};

	ZHLN_CmdImageBarrier(cmd, &barrier);
}

// ============================================================================
// Scoped RAII Layout Transition Implementations
// ============================================================================

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::ScopedBarrierGuard(
	VkCommandBuffer c, const TypedImage<LayoutMap<SrcState>::value>& res,
	VkImageAspectFlags aspect) noexcept
	: cmd(c), resource(res), aspectOverride(aspect) {}

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::~ScopedBarrierGuard() noexcept {
	if (active) {
		IssueBarrier<DstState, SrcState>(cmd, resource, aspectOverride);
	}
}

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::ScopedBarrierGuard(ScopedBarrierGuard&& other) noexcept
	: cmd(other.cmd), resource(other.resource), aspectOverride(other.aspectOverride),
	  active(other.active) {
	other.active = false;
}

template <typename SrcState, typename DstState>
auto ScopedBarrierGuard<SrcState, DstState>::operator=(ScopedBarrierGuard&& other) noexcept
	-> ScopedBarrierGuard& {
	if (this != &other) {
		if (active) {
			IssueBarrier<DstState, SrcState>(cmd, resource, aspectOverride);
		}
		cmd = other.cmd;
		resource = other.resource;
		aspectOverride = other.aspectOverride;
		active = other.active;
		other.active = false;
	}
	return *this;
}

template <typename SrcState, typename DstState, typename T>
auto ScopedBarrier(VkCommandBuffer cmd, const T& resource,
				   VkImageAspectFlags aspectOverride) noexcept {
	auto transitionedImage = IssueBarrier<SrcState, DstState>(cmd, resource, aspectOverride);

	constexpr VkImageLayout srcLayout = LayoutMap<SrcState>::value;
	TypedImage<srcLayout> srcImage;

	if constexpr (requires { resource.State(); }) {
		auto state = resource.State();
		srcImage = {state.handle, state.view, state.extent, state.aspect};
	} else {
		srcImage = {resource.handle, resource.view, resource.extent, resource.aspect};
	}

	return std::make_pair(transitionedImage,
						  ScopedBarrierGuard<SrcState, DstState>(cmd, srcImage, aspectOverride));
}

template <typename InState, typename OutState, typename T>
inline auto IssueBarrier(VkCommandBuffer cmd, const T& resource,
						 VkImageAspectFlags aspectOverride) {
	constexpr VkImageLayout inLayout = LayoutMap<InState>::value;
	constexpr VkImageLayout outLayout = LayoutMap<OutState>::value;

	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkExtent2D extent{};
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

	if constexpr (requires { resource.handle; }) {
		image = resource.handle;
		view = resource.view;
		extent = resource.extent;
		aspect = resource.aspect;
	} else if constexpr (requires { resource.image.Handle(); }) {
		image = resource.image.Handle();
		view = resource.view.Get();
		extent = resource.extent;
		aspect = resource.State().aspect;
	}

	if (aspectOverride != VK_IMAGE_ASPECT_NONE) {
		aspect = aspectOverride;
	}

	TransitionLayout<inLayout, outLayout>(cmd, image, aspect);

	return TypedImage<outLayout>{.handle = image, .view = view, .extent = extent, .aspect = aspect};
}

template <VkImageLayout NewLayout, VkImageLayout OldLayout>
inline auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img,
					   VkImageAspectFlags overrideAspect) noexcept -> TypedImage<NewLayout> {
	VkImageAspectFlags aspect =
		(overrideAspect != VK_IMAGE_ASPECT_NONE) ? overrideAspect : img.aspect;
	TransitionLayout<OldLayout, NewLayout>(cmd, img.handle, aspect);
	return TypedImage<NewLayout>{
		.handle = img.handle, .view = img.view, .extent = img.extent, .aspect = img.aspect};
}

template <VkImageLayout TargetLayout, VkImageLayout OldLayout>
constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img,
						  Tag<TargetLayout> /*unused*/) noexcept {
	return Transition<TargetLayout>(cmd, img);
}

inline void CopyBufferToImage(const VkCommandBuffer cmd,
							  const ZHLN_BufferImageCopyDesc& desc) noexcept {
	ZHLN_CmdCopyBufferToImage(cmd, &desc);
}

template <GpuTriviallyCopyable T>
inline void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout,
				 const VkShaderStageFlags stages, const T& value) noexcept {
	ZHLN_PushConstants(cmd, layout, stages, &value, sizeof(T));
}

template <uint32_t N, typename Record, typename Rebuild>
	requires RecordFn<Record> && RebuildFn<Rebuild>
inline auto DrawFrame(const Context& ctx, const Swapchain& swapchain, const FrameSync<N>& sync,
					  const CommandPools<N>& pools, uint32_t& frame_index, Record&& record,
					  Rebuild&& rebuild) noexcept -> ZHLN_FrameResult {
	const ZHLN_FrameSync& s = sync[frame_index];
	const ZHLN_CommandPool& pool = pools[frame_index];
	const VkCommandBuffer cmd = pools.Cmd(frame_index);

	uint32_t image_index = 0;
	auto result =
		ZHLN_WaitAndAcquireImage(ctx.Device(), swapchain.Get().handle, &s, &pool, &image_index);
	if (result == ZHLN_FrameResult_OutOfDate) [[unlikely]] {
		std::invoke(std::forward<Rebuild>(rebuild));
		return result;
	}

	ZHLN_BeginCommandBuffer(cmd);
	std::invoke(std::forward<Record>(record), cmd, image_index);
	ZHLN_EndCommandBuffer(cmd);

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

	if constexpr ((N & (N - 1)) == 0) {
		frame_index = (frame_index + 1) & (N - 1);
	} else if constexpr (N == 3) {
		frame_index = (frame_index == 2) ? 0 : frame_index + 1;
	} else {
		frame_index = (frame_index + 1) % N;
	}

	return result;
}

inline auto SubmitAndPresent(const ZHLN_FrameSubmitDesc& desc) noexcept -> ZHLN_FrameResult {
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
// DynamicPass Implementation
// ============================================================================

template <size_t ColorCount, bool HasDepth>
template <size_t InsideCount, bool InsideDepth>
constexpr DynamicPass<ColorCount, HasDepth>::DynamicPass(
	const DynamicPass<InsideCount, InsideDepth>&& other,
	std::array<VkRenderingAttachmentInfo, ColorCount>&& colors,
	VkRenderingAttachmentInfo depth) noexcept
	: _extent(other._extent), _flags(other._flags), _colors(std::move(colors)), _depth(depth) {}

template <size_t ColorCount, bool HasDepth>
template <VkImageLayout Layout>
constexpr auto DynamicPass<ColorCount, HasDepth>::AddColor(const TypedImage<Layout>& img,
														   VkAttachmentLoadOp loadOp,
														   VkAttachmentStoreOp storeOp,
														   const ZHLN::Color4& clearColor) noexcept
	-> DynamicPass<ColorCount + 1, HasDepth> {
	static_assert(Layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
				  Layout == VK_IMAGE_LAYOUT_GENERAL);

	std::array<VkRenderingAttachmentInfo, ColorCount + 1> nextColors{};
	for (size_t i = 0; i < ColorCount; ++i) {
		nextColors[i] = _colors[i];
	}

	nextColors[ColorCount] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
							  .pNext = nullptr,
							  .imageView = img.view,
							  .imageLayout = Layout,
							  .resolveMode = VK_RESOLVE_MODE_NONE,
							  .resolveImageView = VK_NULL_HANDLE,
							  .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
							  .loadOp = loadOp,
							  .storeOp = storeOp,
							  .clearValue = {.color = {.float32 = {clearColor.r, clearColor.g,
																   clearColor.b, clearColor.a}}}};

	return DynamicPass<ColorCount + 1, HasDepth>(std::move(*this), std::move(nextColors), _depth);
}

template <size_t ColorCount, bool HasDepth>
template <typename... TypedImages>
constexpr auto DynamicPass<ColorCount, HasDepth>::AddColorGroup(
	const std::tuple<TypedImages...>& imageTuple, VkAttachmentLoadOp loadOp,
	VkAttachmentStoreOp storeOp, const ZHLN::Color4& clearColor) noexcept
	-> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth> {

	constexpr size_t AddedCount = sizeof...(TypedImages);
	std::array<VkRenderingAttachmentInfo, ColorCount + AddedCount> nextColors{};

	// 1. Preserve existing attachments
	for (size_t i = 0; i < ColorCount; ++i) {
		nextColors[i] = _colors[i];
	}

	// 2. Unpack tuple into parameter pack and populate remaining slots branchlessly
	std::apply(
		[&](const auto&... img) {
			size_t offset = ColorCount;
			((nextColors[offset++] =
				  {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				   .pNext = nullptr,
				   .imageView = img.view,
				   .imageLayout = std::remove_cvref_t<decltype(img)>::layout,
				   .resolveMode = VK_RESOLVE_MODE_NONE,
				   .resolveImageView = VK_NULL_HANDLE,
				   .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				   .loadOp = loadOp,
				   .storeOp = storeOp,
				   .clearValue = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b,
														clearColor.a}}}}),
			 ...);
		},
		imageTuple);

	return DynamicPass<ColorCount + AddedCount, HasDepth>(std::move(*this), std::move(nextColors),
														  _depth);
}

template <size_t ColorCount, bool HasDepth>
template <VkImageLayout Layout>
constexpr auto DynamicPass<ColorCount, HasDepth>::AddDepth(const TypedImage<Layout>& img,
														   VkAttachmentLoadOp loadOp,
														   VkAttachmentStoreOp storeOp,
														   float clearVal) noexcept
	-> DynamicPass<ColorCount, true> {
	static_assert(!HasDepth, "ZHLN Execution Error: Depth target already bound to this pass.");
	static_assert(Layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
				  Layout == VK_IMAGE_LAYOUT_GENERAL);

	VkRenderingAttachmentInfo nextDepth = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.pNext = nullptr,
		.imageView = img.view,
		.imageLayout = Layout,
		.resolveMode = VK_RESOLVE_MODE_NONE,
		.resolveImageView = VK_NULL_HANDLE,
		.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.loadOp = loadOp,
		.storeOp = storeOp,
		.clearValue = {.depthStencil = {.depth = clearVal, .stencil = 0}}};

	return DynamicPass<ColorCount, true>(std::move(*this), std::move(_colors), nextDepth);
}

template <size_t ColorCount, bool HasDepth>
constexpr auto DynamicPass<ColorCount, HasDepth>::Flags(VkRenderingFlags flags) noexcept
	-> DynamicPass<ColorCount, HasDepth>& {
	_flags = flags;
	return *this;
}

template <size_t ColorCount, bool HasDepth>
template <typename Func>
void DynamicPass<ColorCount, HasDepth>::Execute(VkCommandBuffer cmd, Func&& func) const {
	const VkRenderingInfo renderInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = _flags,
		.renderArea = {.offset = {0, 0}, .extent = _extent},
		.layerCount = 1,
		.viewMask = _viewMask,
		.colorAttachmentCount = ColorCount,
		.pColorAttachments = ColorCount > 0 ? _colors.data() : nullptr,
		.pDepthAttachment = GetDepthPtr(),
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

template <size_t ColorCount, bool HasDepth>
constexpr auto DynamicPass<ColorCount, HasDepth>::ViewMask(uint32_t mask) noexcept
	-> DynamicPass<ColorCount, HasDepth>& {
	_viewMask = mask;
	return *this;
}

template <size_t ColorCount, bool HasDepth>
constexpr auto DynamicPass<ColorCount, HasDepth>::GetDepthPtr() const noexcept
	-> const VkRenderingAttachmentInfo* {
	if constexpr (HasDepth) {
		return &_depth;
	} else {
		return nullptr;
	}
}

template <GpuTriviallyCopyable T>
inline void DrawInstanced(VkCommandBuffer cmd, const DrawState& state, const T& pushConstants,
						  VkShaderStageFlags stages) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
	if (state.set != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.layout, 0, 1,
								&state.set, 0, nullptr);
	}
	vkCmdPushConstants(cmd, state.layout, stages, 0, sizeof(T), &pushConstants);

	// Everything is a generic non-indexed draw now
	vkCmdDraw(cmd, state.vertexCount, state.instanceCount, state.firstVertex, state.firstInstance);
}

template <GpuTriviallyCopyable T>
inline void DrawIndirect(VkCommandBuffer cmd, const DrawIndirectState& state,
						 const T& pushConstants, VkShaderStageFlags stages) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
	if (state.set != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.layout, 0, 1,
								&state.set, 0, nullptr);
	}
	vkCmdPushConstants(cmd, state.layout, stages, 0, sizeof(T), &pushConstants);

	// Completely Bindless Geometry MDI Dispatch
	vkCmdDrawIndirect(cmd, state.argumentBuffer, state.offset, state.drawCount, state.stride);
}

// ============================================================================
// Surface Implementation
// ============================================================================

inline Surface::Surface(VkInstance instance, VkSurfaceKHR surface)
	: _instance(instance), _handle(surface) {}

inline Surface::~Surface() {
	if (_handle != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(_instance, _handle, nullptr);
	}
}

inline Surface::Surface(Surface&& other) noexcept
	: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
	  _handle(std::exchange(other._handle, VK_NULL_HANDLE)) {}

inline auto Surface::operator=(Surface&& other) noexcept -> Surface& {
	if (this != &other) {
		if (_handle != VK_NULL_HANDLE) {
			vkDestroySurfaceKHR(_instance, _handle, nullptr);
		}
		_instance = std::exchange(other._instance, VK_NULL_HANDLE);
		_handle = std::exchange(other._handle, VK_NULL_HANDLE);
	}
	return *this;
}

inline auto Surface::Get() const -> VkSurfaceKHR {
	return _handle;
}

// ============================================================================
// Error Helpers Implementation
// ============================================================================

inline auto ResultString(const VkResult result) noexcept -> const char* {
	return ZHLN_VkResultString(result);
}

inline std::expected<VkResult, std::string> CheckResult(const VkResult result, const char* context,
														const std::source_location location) {
	if (result != VK_SUCCESS) [[unlikely]] {
		return std::unexpected(ReportVkError(result, context, location));
	}
	return result;
}

// ============================================================================
// SemaphorePool Implementation
// ============================================================================

inline SemaphorePool::~SemaphorePool() noexcept {
	Cleanup();
}

inline SemaphorePool::SemaphorePool(SemaphorePool&& other) noexcept
	: _device(other._device), _count(other._count) {
	for (uint32_t i = 0; i < 6; ++i) {
		_semaphores[i] = other._semaphores[i];
		other._semaphores[i] = VK_NULL_HANDLE;
	}
	other._device = VK_NULL_HANDLE;
	other._count = 0;
}

inline auto SemaphorePool::operator=(SemaphorePool&& other) noexcept -> SemaphorePool& {
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

inline void SemaphorePool::Rebuild(const VkDevice device, const uint32_t count) noexcept {
	Cleanup();
	_device = device;
	_count = ZHLN::Min(count, 6U);

	for (uint32_t i = 0; i < _count; ++i) {
		_semaphores[i] = ZHLN_CreateSemaphore(_device);
	}
}

inline auto SemaphorePool::operator[](const uint32_t index) const noexcept -> VkSemaphore {
	if (index >= _count) [[unlikely]] {
		ReportSemaphoreBoundsError(index, _count);
	}
	return _semaphores[index];
}

inline auto SemaphorePool::Count() const noexcept -> uint32_t {
	return _count;
}

inline auto SemaphorePool::Valid() const noexcept -> bool {
	return _device != VK_NULL_HANDLE;
}

inline void SemaphorePool::Cleanup() noexcept {
	if (_device == VK_NULL_HANDLE) {
		return;
	}

	auto* const d = _device;
	for (uint32_t i = 0; i < _count; ++i) {
		if (_semaphores[i] != VK_NULL_HANDLE) {
			vkDestroySemaphore(d, _semaphores[i], nullptr);
		}
	}

	_semaphores.fill(VK_NULL_HANDLE);
	_count = 0;
	_device = VK_NULL_HANDLE;
}

// ============================================================================
// Image View Helpers Implementation
// ============================================================================
namespace {
struct FormatAspectMapping {
	VkFormat format;
	VkImageAspectFlags aspect;
};
} // namespace

inline constexpr std::array<FormatAspectMapping, 10> kFormatAspectTable = {
	{{.format = VK_FORMAT_R16G16B16A16_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R8G8B8A8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R8G8B8A8_SRGB, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_B8G8R8A8_SRGB, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_R8G8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .aspect = VK_IMAGE_ASPECT_COLOR_BIT},
	 {.format = VK_FORMAT_D32_SFLOAT, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT},
	 {.format = VK_FORMAT_D24_UNORM_S8_UINT,
	  .aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
	 {.format = VK_FORMAT_R16G16_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT}}};

constexpr auto GetFormatAspect(VkFormat format) noexcept -> VkImageAspectFlags {
	for (const auto& mapping : kFormatAspectTable) {
		if (mapping.format == format) {
			return mapping.aspect;
		}
	}
	return VK_IMAGE_ASPECT_NONE;
}

template <VkFormat F>
inline auto CreateView(VkDevice device, VkImage image, VkImageAspectFlags aspect, uint32_t mips)
	-> ImageView {
	ZHLN_ImageViewDesc desc = {
		.image = image,
		.format = F,
		.aspect = aspect,
		.mip_levels = mips,
		.array_layers = 1,
		.view_type = VK_IMAGE_VIEW_TYPE_2D,
		.base_array_layer = {},
	};
	return {device, ZHLN_CreateImageView(device, &desc)};
}

template <VkFormat F>
inline auto CreateViewCube(VkDevice device, VkImage image, uint32_t mips) -> ImageView {
	ZHLN_ImageViewDesc desc = {
		.image = image,
		.format = F,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
		.mip_levels = mips,
		.array_layers = 6,
		.view_type = VK_IMAGE_VIEW_TYPE_CUBE,
		.base_array_layer = {},
	};
	return {device, ZHLN_CreateImageView(device, &desc)};
}

template <VkFormat F>
inline auto CreateView2DArray(VkDevice device, VkImage image, uint32_t baseLayer,
							  uint32_t layerCount, VkImageAspectFlags aspect, uint32_t mips)
	-> ImageView {
	ZHLN_ImageViewDesc desc = {.image = image,
							   .format = F,
							   .aspect = aspect,
							   .mip_levels = mips,
							   .array_layers = layerCount,
							   .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
							   .base_array_layer = baseLayer};
	return {device, ZHLN_CreateImageView(device, &desc)};
}

template <VkFormat F>
inline auto CreateViewCubeArray(VkDevice device, VkImage image, uint32_t arrayLayers,
								VkImageAspectFlags aspect, uint32_t mips) -> ImageView {
	ZHLN_ImageViewDesc desc = {.image = image,
							   .format = F,
							   .aspect = aspect,
							   .mip_levels = mips,
							   .array_layers = arrayLayers,
							   .view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
							   .base_array_layer = 0};
	return {device, ZHLN_CreateImageView(device, &desc)};
}

inline auto IsInstanceExtensionSupported(std::string_view extension) noexcept -> bool {
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);

	std::array<VkExtensionProperties, maxInstanceExtensions> available{};
	count = ZHLN::Min<uint32_t>(count, maxInstanceExtensions);

	vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

	for (uint32_t i = 0; i < count; ++i) {
		if (extension == available[i].extensionName) {
			return true;
		}
	}
	return false;
}

inline auto IsDeviceExtensionSupported(VkPhysicalDevice physical,
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

template <size_t MaxStackBarriers>
inline void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept {
	std::array<VkImageMemoryBarrier2, MaxStackBarriers> stack_barriers;

	for (const auto& pass : passes) {
		const auto transition_count = static_cast<uint32_t>(pass.transitions.size());

		if (transition_count > 0) {
			VkImageMemoryBarrier2* p_barriers = stack_barriers.data();
			VkImageMemoryBarrier2* heap_allocated = nullptr;

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

			const VkDependencyInfo dep_info = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.pNext = nullptr,
				.dependencyFlags = {},
				.memoryBarrierCount = {},
				.pMemoryBarriers = {},
				.bufferMemoryBarrierCount = {},
				.pBufferMemoryBarriers = {},
				.imageMemoryBarrierCount = transition_count,
				.pImageMemoryBarriers = p_barriers,
			};
			vkCmdPipelineBarrier2(cmd, &dep_info);

			if (heap_allocated) [[unlikely]] {
				delete[] heap_allocated;
			}
		}

		if (pass.record) {
			pass.record(cmd, pass.pUserData);
		}
	}
}

inline void Dispatch(VkCommandBuffer cmd, uint32_t totalX, uint32_t totalY, uint32_t totalZ,
					 uint32_t localX, uint32_t localY, uint32_t localZ) noexcept {
	ZHLN_CmdDispatch(cmd, (totalX + localX - 1) / localX, (totalY + localY - 1) / localY,
					 (totalZ + localZ - 1) / localZ);
}

inline void DispatchGroups(VkCommandBuffer cmd, uint32_t gX, uint32_t gY, uint32_t gZ) noexcept {
	ZHLN_CmdDispatch(cmd, gX, gY, gZ);
}

template <uint32_t Width, uint32_t Height> consteval auto GetMipLevels() noexcept -> uint32_t {
	return std::bit_width(ZHLN::Max(Width, Height));
}

inline void GenerateMipmaps(const VkCommandBuffer cmd, const VkImage image, const uint32_t width,
							const uint32_t height) {
	uint32_t levels = std::bit_width(ZHLN::Max(width, height));
	ZHLN_GenerateMipmaps(cmd, image, static_cast<int32_t>(width), static_cast<int32_t>(height),
						 levels);
}

// NOLINTEND(misc-misplaced-const)

} // namespace ZHLN::Vk
