#pragma once

namespace ZHLN::Vk {
// ============================================================================
// Context RAII
// ============================================================================

VkInstance CreateInstance(std::string_view appName, uint32_t appVersion,
						  std::span<const std::string_view> extensions,
						  bool enableValidation) noexcept;

ZHLN_PhysicalDeviceInfo SelectDevice(VkInstance instance, VkSurfaceKHR surface) noexcept;

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
} // namespace ZHLN::Vk
