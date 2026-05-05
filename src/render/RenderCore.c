#include "RenderCore.h"

#include "Utils.h"

#include <spirv_reflect.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static VkBool32 VKAPI_CALL ZHLN_Internal_DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
	const VkDebugUtilsMessengerCallbackDataEXT* data, void* userdata) {

	// Check if it's an Error or Warning
	const char* prefix = "VULKAN";
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		prefix = "VULKAN ERROR";
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		prefix = "VULKAN WARNING";
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
		prefix = "VULKAN INFO";

	fprintf(stderr, "[%s] %s\n", prefix, data->pMessage);

	return VK_FALSE;
}

VkInstance ZHLN_CreateInstance(const ZHLN_InstanceDesc* desc) {
	// C23: We can initialize the Vulkan structs directly inside the call
	const VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
										.pApplicationName = desc->app_name,
										.applicationVersion = desc->version,
										.apiVersion = VK_API_VERSION_1_3};

	static const char* const validation_layers[] = {"VK_LAYER_KHRONOS_validation"};

	// We need to merge the user extensions with the debug extension if validation is on
	const char* final_extensions[32];
	uint32_t final_count = desc->extension_count;
	for (uint32_t i = 0; i < desc->extension_count; ++i) {
		final_extensions[i] = desc->extensions[i];
	}

	if (desc->enable_validation) {
		bool has_debug_ext = false;
		for (uint32_t i = 0; i < final_count; ++i) {
			if (strcmp(final_extensions[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
				has_debug_ext = true;
			}
		}
		if (!has_debug_ext) {
			final_extensions[final_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		}
	}

	VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
		.enabledExtensionCount = final_count,
		.ppEnabledExtensionNames = final_extensions,
		.enabledLayerCount = desc->enable_validation ? 1U : 0U,
		.ppEnabledLayerNames =
			desc->enable_validation ? validation_layers : nullptr, // Compound literal
		.flags = 0,
	};

#ifdef __APPLE__
	// Required to see MoltenVK/KosmicKrisp devices
	create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	VkDebugUtilsMessengerCreateInfoEXT debug_info = {};
	const VkValidationFeatureEnableEXT enables[] = {
		VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
		VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
	const VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
											  .enabledValidationFeatureCount = 2,
											  .pEnabledValidationFeatures = enables};
	if (desc->enable_validation) {
		debug_info = (VkDebugUtilsMessengerCreateInfoEXT){
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
							   VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
							   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
							   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
						   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			.pfnUserCallback = ZHLN_Internal_DebugCallback,
		};
		debug_info.pNext = &features;
		create_info.pNext = &debug_info;
	}

	VkInstance instance;
	if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return instance;
}
[[nodiscard]]
static int32_t ZHLN_Internal_DefaultScoreFn(const ZHLN_PhysicalDeviceInfo* const restrict info,
											[[maybe_unused]] const void* const restrict userdata) {
	// Reject anything missing required queues
	if (!info->has_graphics) {
		return -1;
	}
	if (!info->has_present && /* surface requested */ info->present_family == UINT32_MAX) {
		return -1;
	}

	int32_t score = 0;
	const VkPhysicalDeviceProperties* p = &info->properties.properties;

	if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
		score += 1000;
	}
	if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
		score += 100;
	}

	// Reward VRAM (in MB, capped to avoid overflow)
	for (uint32_t i = 0; i < info->memory.memoryProperties.memoryHeapCount; ++i) {
		const VkMemoryHeap heap = info->memory.memoryProperties.memoryHeaps[i];
		if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
			score += (int32_t)(heap.size / ((VkDeviceSize)1024U * 1024U));
		}
	}

	return score;
}

static void ZHLN_Internal_QueryQueueFamilies(const VkPhysicalDevice device,
											 const VkSurfaceKHR surface,
											 uint32_t* const restrict out_graphics,
											 uint32_t* const restrict out_present) {
	*out_graphics = UINT32_MAX;
	*out_present = UINT32_MAX;

	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

	VkQueueFamilyProperties families[64] = {};
	if (count > 64) {
		count = 64; // clamp; no heap alloc in C layer
	}
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);

	for (uint32_t i = 0; i < count; ++i) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && *out_graphics == UINT32_MAX) {
			*out_graphics = i;
		}

		if (surface != VK_NULL_HANDLE && *out_present == UINT32_MAX) {
			VkBool32 supported = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported);
			if (supported) {
				*out_present = i;
			}
		}
	}

	if (surface == VK_NULL_HANDLE && *out_graphics != UINT32_MAX) {
		*out_present = *out_graphics;
	}
}
[[nodiscard]]
ZHLN_PhysicalDeviceInfo
ZHLN_SelectPhysicalDevice(const ZHLN_DeviceSelectDesc* const restrict desc) {
	ZHLN_PhysicalDeviceInfo null_result = {};

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(desc->instance, &count, nullptr);
	if (count == 0) {
		return null_result;
	}
	if (count > 16) {
		count = 16; // clamp; stack only
	}

	VkPhysicalDevice devices[16] = {};
	vkEnumeratePhysicalDevices(desc->instance, &count, devices);

	const ZHLN_DeviceScoreFn score_fn =
		desc->score_fn ? desc->score_fn : ZHLN_Internal_DefaultScoreFn;

	ZHLN_PhysicalDeviceInfo best = {};
	int32_t best_score = -1;

	for (uint32_t i = 0; i < count; ++i) {
		ZHLN_PhysicalDeviceInfo info = {
			.handle = devices[i],
			.properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2},
			.features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2},
			.memory = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2},
		};

		vkGetPhysicalDeviceProperties2(devices[i], &info.properties);
		vkGetPhysicalDeviceFeatures2(devices[i], &info.features);
		vkGetPhysicalDeviceMemoryProperties2(devices[i], &info.memory);

		ZHLN_Internal_QueryQueueFamilies(devices[i], desc->surface, &info.graphics_family,
										 &info.present_family);

		info.has_graphics = (info.graphics_family != UINT32_MAX);
		info.has_present = (info.present_family != UINT32_MAX);

		const int32_t score = score_fn(&info, desc->score_userdata);
		if (score > best_score) {
			best_score = score;
			best = info;
		}
	}

	return best_score >= 0 ? best : null_result;
}
[[nodiscard]]
ZHLN_Device ZHLN_CreateDevice(const ZHLN_DeviceDesc* const restrict desc) {
	ZHLN_Device null_result = {};

	// --- Queue Creation ---
	// Deduplicate: if graphics and present are the same family, only create one queue
	const uint32_t unique_families[2] = {
		desc->physical->graphics_family,
		desc->physical->present_family,
	};
	const uint32_t unique_count = (unique_families[0] == unique_families[1]) ? 1U : 2U;

	const float priority = 1.0F;
	VkDeviceQueueCreateInfo queue_infos[2] = {};
	for (uint32_t i = 0; i < unique_count; ++i) {
		queue_infos[i] = (VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = unique_families[i],
			.queueCount = 1,
			.pQueuePriorities = &priority,
		};
	}

	// --- Feature Chain ---
	// If the caller passed a features2 chain, use it directly as pNext.
	// Otherwise wire in a plain zero-initialized one so sType is always set.
	const VkPhysicalDeviceFeatures2 default_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
	};
	const VkPhysicalDeviceFeatures2* features = desc->features ? desc->features : &default_features;

	const VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = features,			 // The modern, extensible way
		.pEnabledFeatures = nullptr, // Explicitly null to avoid dual-source conflict

		.queueCreateInfoCount = unique_count,
		.pQueueCreateInfos = queue_infos,

		.enabledExtensionCount = desc->extension_count,
		.ppEnabledExtensionNames = desc->extensions,

		// Explicitly zero these out.
		// It's cleaner than pretending they do something.
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = nullptr};

	VkDevice handle;
	if (vkCreateDevice(desc->physical->handle, &create_info, nullptr, &handle) != VK_SUCCESS) {
		return null_result;
	}

	// --- Queue Retrieval ---
	VkQueue graphics_queue;
	VkQueue present_queue;
	vkGetDeviceQueue(handle, desc->physical->graphics_family, 0, &graphics_queue);

	// If families are the same, present_queue is just an alias — no second call needed
	present_queue =
		(unique_count == 1)
			? graphics_queue
			: (vkGetDeviceQueue(handle, desc->physical->present_family, 0, &present_queue),
			   present_queue);

	return (ZHLN_Device){
		.handle = handle,
		.graphics_queue = graphics_queue,
		.present_queue = present_queue,
	};
}
[[nodiscard]]
ZHLN_SwapchainSupport
ZHLN_QuerySwapchainSupport(const ZHLN_SwapchainSupportDesc* const restrict desc) {
	ZHLN_SwapchainSupport result = {};

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(desc->physical, desc->surface, &result.capabilities);

	uint32_t hardware_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(desc->physical, desc->surface, &hardware_count, nullptr);
	result.format_count = (hardware_count > 64) ? 64 : hardware_count;
	if (result.format_count > 0) {
		vkGetPhysicalDeviceSurfaceFormatsKHR(desc->physical, desc->surface, &result.format_count,
											 result.formats);
	}

	uint32_t present_count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(desc->physical, desc->surface, &present_count,
											  nullptr);
	result.present_mode_count = (present_count > 8) ? 8 : present_count;
	if (result.present_mode_count > 0) {
		vkGetPhysicalDeviceSurfacePresentModesKHR(desc->physical, desc->surface,
												  &result.present_mode_count, result.present_modes);
	}
	return result;
}

static VkSurfaceFormatKHR
ZHLN_Internal_ChooseFormat(const ZHLN_SwapchainSupport* const restrict support) {
	for (uint32_t i = 0; i < support->format_count; ++i) {
		const VkSurfaceFormatKHR f = support->formats[i];
		if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
			f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return f;
		}
	}
	// Fallback: whatever the driver gives us first
	return support->formats[0];
}
[[nodiscard]]
static VkPresentModeKHR
ZHLN_Internal_ChoosePresentMode(const ZHLN_SwapchainSupport* const restrict support, bool vsync) {
	if (!vsync) {
		for (uint32_t i = 0; i < support->present_mode_count; ++i) {
			if (support->present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
				return VK_PRESENT_MODE_MAILBOX_KHR;
			}
		}
		// Immediate is better than FIFO for non-vsync if mailbox unavailable
		for (uint32_t i = 0; i < support->present_mode_count; ++i) {
			if (support->present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
				return VK_PRESENT_MODE_IMMEDIATE_KHR;
			}
		}
	}
	// FIFO is always guaranteed by the spec
	return VK_PRESENT_MODE_FIFO_KHR;
}
[[nodiscard]]
static VkExtent2D ZHLN_Internal_ChooseExtent(const VkSurfaceCapabilitiesKHR* const restrict caps,
											 const uint32_t width, const uint32_t height) {
	// UINT32_MAX signals the surface lets us pick freely
	if (caps->currentExtent.width != UINT32_MAX) {
		return caps->currentExtent;
	}

	return (VkExtent2D){
		.width = ZHLN_Clamp(width, caps->minImageExtent.width, caps->maxImageExtent.width),
		.height = ZHLN_Clamp(height, caps->minImageExtent.height, caps->maxImageExtent.height),
	};
}
[[nodiscard]]
static VkCompositeAlphaFlagBitsKHR
ZHLN_Internal_ChooseCompositeAlpha(const VkCompositeAlphaFlagsKHR supported) {
	static const VkCompositeAlphaFlagBitsKHR preferred[] = {
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
	};
	for (uint32_t i = 0; i < 4; ++i) {
		if (supported & preferred[i]) {
			return preferred[i];
		}
	}
	// Spec guarantees at least one bit is set, so this is unreachable
	return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
[[nodiscard]]
ZHLN_Swapchain ZHLN_CreateSwapchain(const ZHLN_SwapchainDesc* const restrict desc) {
	ZHLN_Swapchain null_result = {};

	const ZHLN_SwapchainSupportDesc support_desc = {
		.physical = desc->physical->handle,
		.surface = desc->surface,
	};
	const ZHLN_SwapchainSupport support = ZHLN_QuerySwapchainSupport(&support_desc);
	if (support.format_count == 0 || support.present_mode_count == 0) {
		return null_result;
	}

	const VkSurfaceFormatKHR format = ZHLN_Internal_ChooseFormat(&support);
	const VkPresentModeKHR present_mode = ZHLN_Internal_ChoosePresentMode(&support, desc->vsync);
	const VkExtent2D extent =
		ZHLN_Internal_ChooseExtent(&support.capabilities, desc->width, desc->height);

	if (support.capabilities.minImageCount > 8) {
		return null_result;
	}

	// Determine ideal count (min + 1 for triple buffering/stalling avoidance)
	uint32_t image_count = support.capabilities.minImageCount + 1;

	// Clamp to hardware maximum (maxImageCount == 0 means no limit)
	if (support.capabilities.maxImageCount > 0 &&
		image_count > support.capabilities.maxImageCount) {
		image_count = support.capabilities.maxImageCount;
	}

	// Clamp to library capacity
	if (image_count > 8) {
		image_count = 8;
	}

	const uint32_t queue_families[2] = {
		desc->physical->graphics_family,
		desc->physical->present_family,
	};
	const bool shared = (queue_families[0] == queue_families[1]);

	// Define the present modes we are "aware" of to satisfy Maintenance1
	// We provide the modes found during query support.
	const VkSwapchainPresentModesCreateInfoKHR present_modes_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
		.pNext = nullptr,
		.presentModeCount = support.present_mode_count,
		.pPresentModes = support.present_modes};

	const VkSwapchainCreateInfoKHR create_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.pNext = &present_modes_info,
		.flags = 0,
		.surface = desc->surface,
		.minImageCount = image_count,
		.imageFormat = format.format,
		.imageColorSpace = format.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = shared ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
		.queueFamilyIndexCount = shared ? 0U : 2U,
		.pQueueFamilyIndices = shared ? nullptr : queue_families,
		.preTransform = support.capabilities.currentTransform,
		.compositeAlpha =
			ZHLN_Internal_ChooseCompositeAlpha(support.capabilities.supportedCompositeAlpha),
		.presentMode = present_mode,
		.clipped = VK_TRUE,
		.oldSwapchain = desc->old_swapchain,
	};

	VkSwapchainKHR handle;
	if (vkCreateSwapchainKHR(desc->device->handle, &create_info, nullptr, &handle) != VK_SUCCESS) {
		return null_result;
	}

	// --- Image Retrieval ---
	ZHLN_Swapchain swapchain = {
		.handle = handle,
		.format = format.format,
		.extent = extent,
		.image_count = image_count,
	};

	vkGetSwapchainImagesKHR(desc->device->handle, handle, &swapchain.image_count, swapchain.images);

	// --- Image Views ---
	for (uint32_t i = 0; i < swapchain.image_count; ++i) {
		const VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = swapchain.images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format.format,
			.components =
				{
					.r = VK_COMPONENT_SWIZZLE_IDENTITY,
					.g = VK_COMPONENT_SWIZZLE_IDENTITY,
					.b = VK_COMPONENT_SWIZZLE_IDENTITY,
					.a = VK_COMPONENT_SWIZZLE_IDENTITY,
				},
			.subresourceRange =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
		};

		if (vkCreateImageView(desc->device->handle, &view_info, nullptr, &swapchain.views[i]) !=
			VK_SUCCESS) {
			// Destroy already-created views before bailing
			for (uint32_t j = 0; j < i; ++j) {
				vkDestroyImageView(desc->device->handle, swapchain.views[j], nullptr);
			}
			vkDestroySwapchainKHR(desc->device->handle, handle, nullptr);
			return null_result;
		}
	}

	return swapchain;
}

void ZHLN_DestroySwapchain(const VkDevice device, ZHLN_Swapchain* const swapchain) {
	for (uint32_t i = 0; i < swapchain->image_count; ++i) {
		vkDestroyImageView(device, swapchain->views[i], nullptr);
	}
	vkDestroySwapchainKHR(device, swapchain->handle, nullptr);
	*swapchain = (ZHLN_Swapchain){};
}

[[nodiscard]]
bool ZHLN_CreateFrameSync(const ZHLN_FrameSyncDesc* const desc,
						  ZHLN_FrameSync* const restrict out_sync) {
	static constexpr VkSemaphoreCreateInfo sem_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};

	// Fence starts signaled so the first frame doesn't wait forever
	static constexpr VkFenceCreateInfo fence_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};

	for (uint32_t i = 0; i < desc->frame_count; ++i) {
		bool ok =
			vkCreateSemaphore(desc->device, &sem_info, nullptr, &out_sync[i].image_available) ==
				VK_SUCCESS &&
			vkCreateSemaphore(desc->device, &sem_info, nullptr, &out_sync[i].render_finished) ==
				VK_SUCCESS &&
			vkCreateFence(desc->device, &fence_info, nullptr, &out_sync[i].in_flight) == VK_SUCCESS;

		if (!ok) {
			// Destroy everything successfully created up to and including this frame
			ZHLN_DestroyFrameSync(desc->device, out_sync, i + 1);
			return false;
		}
	}

	return true;
}

void ZHLN_DestroyFrameSync(const VkDevice device, ZHLN_FrameSync* const restrict sync,
						   const uint32_t frame_count) {
	for (uint32_t i = 0; i < frame_count; ++i) {
		if (sync[i].image_available != VK_NULL_HANDLE) {
			vkDestroySemaphore(device, sync[i].image_available, nullptr);
		}
		if (sync[i].render_finished != VK_NULL_HANDLE) {
			vkDestroySemaphore(device, sync[i].render_finished, nullptr);
		}
		if (sync[i].in_flight != VK_NULL_HANDLE) {
			vkDestroyFence(device, sync[i].in_flight, nullptr);
		}
		sync[i] = (ZHLN_FrameSync){};
	}
}
[[nodiscard]]
bool ZHLN_CreateCommandPool(const VkDevice device, const uint32_t queue_family,
							ZHLN_CommandPool* const restrict out_pool) {
	VkCommandPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = queue_family,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};

	if (vkCreateCommandPool(device, &info, nullptr, &out_pool->pool) != VK_SUCCESS) {
		return false;
	}

	out_pool->count = 0;
	return true;
}
[[nodiscard]]
bool ZHLN_AllocateCommandBuffers(const VkDevice device, ZHLN_CommandPool* const restrict pool,
								 const uint32_t count) {
	if (count > 8) {
		return false;
	}

	const VkCommandBufferAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = count,
	};

	if (vkAllocateCommandBuffers(device, &info, pool->buffers) != VK_SUCCESS) {
		return false;
	}

	pool->count = count;
	return true;
}

void ZHLN_ResetCommandPool(const VkDevice device, const ZHLN_CommandPool* const restrict pool) {
	vkResetCommandPool(device, pool->pool, 0);
}

void ZHLN_DestroyCommandPool(const VkDevice device, ZHLN_CommandPool* const restrict pool) {
	// Implicitly frees all command buffers allocated from it
	if (pool->pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(device, pool->pool, nullptr);
	}
	*pool = (ZHLN_CommandPool){};
}

void ZHLN_WaitAndResetFence(const VkDevice device, const VkFence fence) {
	vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &fence);
}
[[nodiscard]]
ZHLN_FrameResult ZHLN_AcquireImage(const VkDevice device,
								   const ZHLN_AcquireDesc* const restrict desc,
								   uint32_t* const restrict out_image_index) {
	const VkResult result =
		vkAcquireNextImageKHR(device, desc->swapchain, desc->timeout_ns, desc->image_available,
							  VK_NULL_HANDLE, out_image_index);
	switch (result) {
		case VK_SUCCESS:
			return ZHLN_FrameResult_Ok;
		case VK_SUBOPTIMAL_KHR:
			return ZHLN_FrameResult_Suboptimal;
		case VK_ERROR_OUT_OF_DATE_KHR:
			return ZHLN_FrameResult_OutOfDate;
		default:
			return ZHLN_FrameResult_Error;
	}
}

void ZHLN_SubmitFrame(const VkQueue graphics_queue, const ZHLN_FrameSync* const restrict sync,
					  const VkCommandBuffer cmd) {
	const VkCommandBufferSubmitInfo cmd_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = cmd,
	};

	const VkSemaphoreSubmitInfo wait_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = sync->image_available,
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	};

	const VkSemaphoreSubmitInfo signal_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = sync->render_finished,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
	};

	const VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = 1,
		.pWaitSemaphoreInfos = &wait_info,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cmd_info,
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signal_info,
	};

	vkQueueSubmit2(graphics_queue, 1, &submit, sync->in_flight);
}
[[nodiscard]]
ZHLN_FrameResult ZHLN_PresentFrame(const ZHLN_PresentDesc* const restrict desc) {
	const VkPresentInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &desc->render_finished,
		.swapchainCount = 1,
		.pSwapchains = &desc->swapchain,
		.pImageIndices = &desc->image_index,
	};

	const VkResult result = vkQueuePresentKHR(desc->present_queue, &info);
	switch (result) {
		case VK_SUCCESS:
			return ZHLN_FrameResult_Ok;
		case VK_SUBOPTIMAL_KHR:
			return ZHLN_FrameResult_Suboptimal;
		case VK_ERROR_OUT_OF_DATE_KHR:
			return ZHLN_FrameResult_OutOfDate;
		default:
			return ZHLN_FrameResult_Error;
	}
}

VkShaderModule ZHLN_CreateShaderModule(const VkDevice device,
									   const ZHLN_ShaderDesc* const restrict desc) {
	if (!desc->code || desc->size == 0 || desc->size % 4 != 0) {
		return VK_NULL_HANDLE;
	}

	const VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = desc->size,
		.pCode = desc->code,
	};

	VkShaderModule module;
	if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}

	return module;
}

[[nodiscard]]
bool ZHLN_CreateShaderStages(const ZHLN_ShaderStagesDesc* const restrict desc,
							 ZHLN_ShaderStages* const restrict out) {
	out->vert.handle = ZHLN_CreateShaderModule(desc->device, &desc->vert);
	out->frag.handle = ZHLN_CreateShaderModule(desc->device, &desc->frag);

	if (out->vert.handle == VK_NULL_HANDLE || out->frag.handle == VK_NULL_HANDLE) {
		ZHLN_DestroyShaderStages(desc->device, out);
		return false;
	}

	out->vert.stage = VK_SHADER_STAGE_VERTEX_BIT;
	out->frag.stage = VK_SHADER_STAGE_FRAGMENT_BIT;

	// --- NEW: AUTO-REFLECTION LOGIC ---
	const ZHLN_ShaderDesc* descs[2] = {&desc->vert, &desc->frag};
	ZHLN_Shader* targets[2] = {&out->vert, &out->frag};

	for (int i = 0; i < 2; ++i) {
		if (descs[i]->entry_point) {
			// Use provided name
			strncpy(targets[i]->entry_point, descs[i]->entry_point, 63);
		} else {
			// Auto-reflect name from SPIR-V
			SpvReflectShaderModule module;
			if (spvReflectCreateShaderModule(descs[i]->size, descs[i]->code, &module) ==
				SPV_REFLECT_RESULT_SUCCESS) {
				if (module.entry_point_count > 0) {
					strncpy(targets[i]->entry_point, module.entry_points[0].name, 63);
				}
				spvReflectDestroyShaderModule(&module);
			} else {
				// Fallback to main if reflection fails
				strncpy(targets[i]->entry_point, "main", 63);
			}
		}
	}
	return true;
}

void ZHLN_DestroyShaderModule(const VkDevice device, const VkShaderModule module) {
	if (module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(device, module, nullptr);
	}
}

void ZHLN_DestroyShaderStages(const VkDevice device, ZHLN_ShaderStages* const restrict stages) {
	ZHLN_DestroyShaderModule(device, stages->vert.handle);
	ZHLN_DestroyShaderModule(device, stages->frag.handle);
	*stages = (ZHLN_ShaderStages){};
}

void ZHLN_PopulateShaderStageInfos(const ZHLN_ShaderStages* const restrict stages,
								   VkPipelineShaderStageCreateInfo* const restrict out_stages) {
	out_stages[0] = (const VkPipelineShaderStageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = stages->vert.stage,
		.module = stages->vert.handle,
		.pName = stages->vert.entry_point,
	};

	out_stages[1] = (const VkPipelineShaderStageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = stages->frag.stage,
		.module = stages->frag.handle,
		.pName = stages->frag.entry_point,
	};
}

VkPipelineLayout ZHLN_CreatePipelineLayout(const VkDevice device,
										   const ZHLN_PipelineLayoutDesc* const restrict desc) {
	const VkPipelineLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = desc->set_layout_count,
		.pSetLayouts = desc->set_layouts,
		.pushConstantRangeCount = desc->push_constant_count,
		.pPushConstantRanges = desc->push_constants,
	};

	VkPipelineLayout layout;
	if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return layout;
}

void ZHLN_DestroyPipelineLayout(const VkDevice device, const VkPipelineLayout layout) {
	if (layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(device, layout, nullptr);
	}
}
[[nodiscard]]
VkPipeline ZHLN_CreateGraphicsPipeline(const VkDevice device,
									   const ZHLN_GraphicsPipelineDesc* const restrict desc) {

	// --- Shader Stages ---
	VkPipelineShaderStageCreateInfo shader_stages[2];
	ZHLN_PopulateShaderStageInfos(desc->stages, shader_stages);

	// --- Vertex Input ---
	const VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = desc->vertex_binding_count,
		.pVertexBindingDescriptions = desc->vertex_bindings,
		.vertexAttributeDescriptionCount = desc->vertex_attribute_count,
		.pVertexAttributeDescriptions = desc->vertex_attributes,
	};

	// --- Input Assembly ---
	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = desc->topology ? desc->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,
	};

	// --- Viewport & Scissor (fully dynamic, no hardcoded resolution) ---
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	// --- Rasterizer ---
	const VkPipelineRasterizationStateCreateInfo rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = desc->polygon_mode, // Just use the value directly
		.cullMode = desc->cull_mode,	   // Just use the value directly
		.frontFace = desc->front_face ? desc->front_face : VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0F,
	};

	// --- Multisampling ---
	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	// --- Depth/Stencil ---
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = desc->depth_test ? VK_TRUE : VK_FALSE,
		.depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};

	// --- Color Blend ---
	const VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = desc->blend_enable ? VK_TRUE : VK_FALSE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
						  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};

	const VkPipelineColorBlendStateCreateInfo color_blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};

	// --- Dynamic State ---
	const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};

	// --- Dynamic Rendering (Vulkan 1.3, no VkRenderPass needed) ---
	const VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &desc->color_format,
		.depthAttachmentFormat = desc->depth_format,
	};

	const VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.stageCount = 2,
		.pStages = shader_stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blend,
		.pDynamicState = &dynamic_state,
		.layout = desc->layout,
	};

	VkPipeline pipeline;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
		VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

void ZHLN_DestroyPipeline(const VkDevice device, const VkPipeline pipeline) {
	if (pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(device, pipeline, nullptr);
	}
}

void ZHLN_BeginRendering(const VkCommandBuffer cmd,
						 const ZHLN_RenderPassDesc* const restrict desc) {
	const VkRenderingAttachmentInfo color_attachment = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = desc->target_view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = {.color = {.float32 =
									 {
										 desc->clear_color[0],
										 desc->clear_color[1],
										 desc->clear_color[2],
										 desc->clear_color[3],
									 }}},
	};

	const VkRenderingAttachmentInfo depth_attachment = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = desc->depth_view,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // don't need depth after the frame
		.clearValue = {.depthStencil = {.depth = desc->clear_depth ? desc->clear_depth : 1.0F}},
	};

	const VkRenderingInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {.offset = {0, 0}, .extent = desc->extent},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_attachment,
		.pDepthAttachment = desc->depth_view != VK_NULL_HANDLE ? &depth_attachment : nullptr,
	};

	vkCmdBeginRendering(cmd, &rendering_info);

	const VkViewport viewport = {
		.x = 0.0F,
		.y = 0.0F,
		.width = (float)desc->extent.width,
		.height = (float)desc->extent.height,
		.minDepth = 0.0F,
		.maxDepth = 1.0F,
	};
	const VkRect2D scissor = {{0, 0}, desc->extent};

	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void ZHLN_EndRendering(VkCommandBuffer cmd) {
	vkCmdEndRendering(cmd);
}

/* --- FRAME HELPERS --- */

void ZHLN_WaitAndResetFrame(const VkDevice device, const VkFence in_flight_fence,
							const ZHLN_CommandPool* const restrict pool) {
	vkWaitForFences(device, 1, &in_flight_fence, VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &in_flight_fence);
	ZHLN_ResetCommandPool(device, pool);
}

void ZHLN_BeginCommandBuffer(const VkCommandBuffer cmd) {
	const VkCommandBufferBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(cmd, &info);
}

void ZHLN_EndCommandBuffer(const VkCommandBuffer cmd) {
	vkEndCommandBuffer(cmd);
}

/* --- PUSH CONSTANT HELPERS --- */

void ZHLN_PushConstants(const VkCommandBuffer cmd, const VkPipelineLayout layout,
						const VkShaderStageFlags stages, const void* const restrict data,
						const uint32_t size) {
	vkCmdPushConstants(cmd, layout, stages, 0, size, data);
}

/* --- ERROR HELPERS --- */

const char* ZHLN_VkResultString(const VkResult result) {
	switch (result) {
		case VK_SUCCESS:
			return "VK_SUCCESS";
		case VK_NOT_READY:
			return "VK_NOT_READY";
		case VK_TIMEOUT:
			return "VK_TIMEOUT";
		case VK_EVENT_SET:
			return "VK_EVENT_SET";
		case VK_EVENT_RESET:
			return "VK_EVENT_RESET";
		case VK_INCOMPLETE:
			return "VK_INCOMPLETE";
		case VK_SUBOPTIMAL_KHR:
			return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_HOST_MEMORY:
			return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:
			return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST:
			return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_MEMORY_MAP_FAILED:
			return "VK_ERROR_MEMORY_MAP_FAILED";
		case VK_ERROR_LAYER_NOT_PRESENT:
			return "VK_ERROR_LAYER_NOT_PRESENT";
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT:
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_TOO_MANY_OBJECTS:
			return "VK_ERROR_TOO_MANY_OBJECTS";
		case VK_ERROR_FORMAT_NOT_SUPPORTED:
			return "VK_ERROR_FORMAT_NOT_SUPPORTED";
		case VK_ERROR_FRAGMENTED_POOL:
			return "VK_ERROR_FRAGMENTED_POOL";
		case VK_ERROR_SURFACE_LOST_KHR:
			return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
			return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR:
			return "VK_ERROR_OUT_OF_DATE_KHR";
		case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
			return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
		case VK_ERROR_VALIDATION_FAILED_EXT:
			return "VK_ERROR_VALIDATION_FAILED_EXT";
		case VK_ERROR_INVALID_SHADER_NV:
			return "VK_ERROR_INVALID_SHADER_NV";
		case VK_ERROR_OUT_OF_POOL_MEMORY:
			return "VK_ERROR_OUT_OF_POOL_MEMORY";
		case VK_ERROR_INVALID_EXTERNAL_HANDLE:
			return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
		case VK_ERROR_FRAGMENTATION:
			return "VK_ERROR_FRAGMENTATION";
		case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
			return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
		case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
			return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
		case VK_ERROR_UNKNOWN:
			return "VK_ERROR_UNKNOWN";
		default:
			return "<unrecognized VkResult>";
	}
}

void ZHLN_CmdCopyBuffer(const VkCommandBuffer cmd, const ZHLN_BufferCopyDesc* const restrict desc) {
	// Vulkan 1.3 Copy 2 API
	const VkBufferCopy2 region = {.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
								  .srcOffset = desc->src_offset,
								  .dstOffset = desc->dst_offset,
								  .size = desc->size};

	const VkCopyBufferInfo2 copy_info = {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
										 .srcBuffer = desc->src,
										 .dstBuffer = desc->dst,
										 .regionCount = 1,
										 .pRegions = &region};

	vkCmdCopyBuffer2(cmd, &copy_info);
}

void ZHLN_CmdImageBarrier(const VkCommandBuffer cmd,
						  const ZHLN_ImageBarrierDesc* const restrict desc) {
	// Vulkan 1.3 Synchronization 2 API
	const VkImageMemoryBarrier2 barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = desc->src_stage,
		.srcAccessMask = desc->src_access,
		.dstStageMask = desc->dst_stage,
		.dstAccessMask = desc->dst_access,
		.oldLayout = desc->src_layout,
		.newLayout = desc->dst_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = desc->image,
		.subresourceRange =
			{
				.aspectMask = desc->aspect,
				.baseMipLevel = 0,
				.levelCount = VK_REMAINING_MIP_LEVELS,
				.baseArrayLayer = 0,
				.layerCount = VK_REMAINING_ARRAY_LAYERS,
			},
	};

	const VkDependencyInfo dependency_info = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
											  .imageMemoryBarrierCount = 1,
											  .pImageMemoryBarriers = &barrier};

	vkCmdPipelineBarrier2(cmd, &dependency_info);
}

void ZHLN_CmdCopyBufferToImage(const VkCommandBuffer cmd,
							   const ZHLN_BufferImageCopyDesc* const restrict desc) {
	const VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferOffset = desc->buffer_offset,
		.bufferRowLength = 0,	// tightly packed
		.bufferImageHeight = 0, // tightly packed
		.imageSubresource =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = desc->mip_level,
				.baseArrayLayer = desc->base_array_layer,
				.layerCount = 1,
			},
		.imageOffset = {0, 0, 0},
		.imageExtent = {desc->width, desc->height, 1},
	};

	const VkCopyBufferToImageInfo2 copy_info = {
		.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		.srcBuffer = desc->buffer,
		.dstImage = desc->image,
		.dstImageLayout = desc->layout,
		.regionCount = 1,
		.pRegions = &region,
	};

	vkCmdCopyBufferToImage2(cmd, &copy_info);
}
[[nodiscard]]
VkSemaphore ZHLN_CreateSemaphore(const VkDevice device) {
	const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore semaphore = VK_NULL_HANDLE;
	vkCreateSemaphore(device, &info, nullptr, &semaphore);
	return semaphore;
}

void ZHLN_DestroySemaphore(const VkDevice device, const VkSemaphore semaphore) {
	if (semaphore != VK_NULL_HANDLE) {
		vkDestroySemaphore(device, semaphore, nullptr);
	}
}