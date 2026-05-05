/**
 * @file RenderCore.h
 * @brief Project-Zahlen's Zero-overHead vuLkan abstractioN layer (ZHLN)
 *
 * Provides a C23-compliant interface for Vulkan instance management
 * and hardware selection using a data-oriented library approach.
 */

#pragma once
#include <stdbool.h> // We use booleans as keyword but good to include nevertheless
#include <vulkan/vulkan.h>

#define ZHLN_RESTRICT __restrict

#ifdef __cplusplus
extern "C" {
#endif

/* --- INSTANCE MANAGEMENT --- */

/**
 * @struct ZHLN_InstanceDesc
 * @brief Configuration for Vulkan Instance initialization.
 */
typedef struct {
	char app_name[64];		  /**< Application name embedded to satisfy C23 constexpr */
	uint32_t version;		  /**< Application-specific version (VK_MAKE_API_VERSION) */
	uint32_t extension_count; /**< Number of additional extensions to enable */
	const VkDebugUtilsMessageSeverityFlagsEXT
		severity_flags;			   /**< Severity flags for the validation layer */
	const char* const* extensions; /**< Pointer to list of extension name strings */
	bool enable_validation;		   /**< Toggle for Khronos Validation Layers */
} ZHLN_InstanceDesc;

/**
 * @brief Default instance configuration for ZHLN.
 * Uses C23 array-copy initialization for the application name.
 */
static constexpr ZHLN_InstanceDesc ZHLN_DEFAULT_INSTANCE_DESC = {
	.app_name = "ZHLN Engine",
	.version = VK_MAKE_API_VERSION(0, 1, 0, 0),
	.extension_count = 0,
	.severity_flags = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
					  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
	.extensions = nullptr,
	.enable_validation = true,
};

static constexpr ZHLN_InstanceDesc ZHLN_VERBOSE_INSTANCE_DESC = {
	.app_name = "ZHLN Engine",
	.version = VK_MAKE_API_VERSION(0, 1, 0, 0),

	.extension_count = 0,
	.severity_flags = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
					  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
					  VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
					  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
	.extensions = nullptr,
	.enable_validation = true,
};

/**
 * @brief Creates a Vulkan Instance with debug messenger attached to pNext.
 * @param desc Pointer to initialization descriptor.
 * @return VkInstance handle or VK_NULL_HANDLE on critical failure.
 */
[[nodiscard]]
VkInstance ZHLN_CreateInstance(const ZHLN_InstanceDesc* const ZHLN_RESTRICT desc);

/* --- DEVICE SELECTION --- */

/**
 * @struct ZHLN_PhysicalDeviceInfo
 * @brief Comprehensive snapshot of a Physical Device's capabilities.
 */
typedef struct {
	VkPhysicalDevice handle;				  /**< Raw Vulkan handle */
	VkPhysicalDeviceProperties2 properties;	  /**< Device limits and name */
	VkPhysicalDeviceFeatures2 features;		  /**< Supported hardware features */
	VkPhysicalDeviceMemoryProperties2 memory; /**< Heap and memory type info */
	uint32_t graphics_family;				  /**< Index of the graphics queue family */
	uint32_t present_family;				  /**< Index of the presentation queue family */
	bool has_graphics;						  /**< True if a graphics queue was found */
	bool has_present;						  /**< True if presentation is supported on 'surface' */
} ZHLN_PhysicalDeviceInfo;

/**
 * @typedef ZHLN_DeviceScoreFn
 * @brief User-defined callback to rank hardware candidates.
 * @return A score where >0 is preferred, and <0 rejects the device entirely.
 */
typedef int32_t (*ZHLN_DeviceScoreFn)(const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT info,
									  const void* const ZHLN_RESTRICT userdata);

/**
 * @struct ZHLN_DeviceSelectDesc
 * @brief Constraints and logic for selecting the optimal GPU.
 */
typedef const struct {
	const VkInstance instance;	/**< Required: Active Vulkan instance */
	const VkSurfaceKHR surface; /**< Optional: Surface for present support checks */
	const ZHLN_DeviceScoreFn
		score_fn;				/**< Optional: Scoring logic (null = discrete GPU preference) */
	const void* score_userdata; /**< Context passed to the scoring function */
} ZHLN_DeviceSelectDesc;

/**
 * @brief Queries all GPUs and selects the best candidate based on scoring.
 * @param desc Selection criteria and context.
 * @return Populated info struct. Check .handle == VK_NULL_HANDLE for failure.
 */
[[nodiscard]]
ZHLN_PhysicalDeviceInfo
ZHLN_SelectPhysicalDevice(const ZHLN_DeviceSelectDesc* const ZHLN_RESTRICT desc);

/* --- DEVICE CREATION --- */

typedef const struct {
	const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
	const char* const* const extensions;
	const uint32_t extension_count;
	const VkPhysicalDeviceFeatures2* features; // nullptr = nothing extra requested
	const bool enable_validation;
} ZHLN_DeviceDesc;

typedef struct {
	VkDevice handle;
	VkQueue graphics_queue;
	VkQueue present_queue;
} ZHLN_Device;

[[nodiscard]]
ZHLN_Device ZHLN_CreateDevice(const ZHLN_DeviceDesc* const ZHLN_RESTRICT desc);

/* --- SWAPCHAIN --- */

typedef struct {
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[64];
	VkPresentModeKHR present_modes[8];
	uint32_t format_count;
	uint32_t present_mode_count;
} ZHLN_SwapchainSupport;

typedef const struct {
	const VkPhysicalDevice physical;
	const VkSurfaceKHR surface;
} ZHLN_SwapchainSupportDesc;

typedef const struct {
	const ZHLN_Device* const ZHLN_RESTRICT device;
	const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
	const VkSurfaceKHR surface;
	const uint32_t width;
	const uint32_t height;
	const bool vsync;
	const VkSwapchainKHR old_swapchain; // VK_NULL_HANDLE on first create
} ZHLN_SwapchainDesc;

typedef struct {
	VkSwapchainKHR handle;
	VkImage images[8];
	VkImageView views[8];
	uint32_t image_count;
	VkFormat format;
	VkExtent2D extent;
} ZHLN_Swapchain;

[[nodiscard]]
ZHLN_SwapchainSupport
ZHLN_QuerySwapchainSupport(const ZHLN_SwapchainSupportDesc* const ZHLN_RESTRICT desc);

[[nodiscard]]
ZHLN_Swapchain ZHLN_CreateSwapchain(const ZHLN_SwapchainDesc* const ZHLN_RESTRICT desc);

void ZHLN_DestroySwapchain(const VkDevice device, ZHLN_Swapchain* const ZHLN_RESTRICT swapchain);

/* --- SYNC PRIMITIVES --- */

typedef struct {
	VkSemaphore image_available;
	VkSemaphore render_finished;
	VkFence in_flight;
} ZHLN_FrameSync;

typedef const struct {
	const VkDevice device;
	const uint32_t frame_count;
} ZHLN_FrameSyncDesc;

// out_sync must point to an array of at least desc->frame_count
[[nodiscard]]
bool ZHLN_CreateFrameSync(const ZHLN_FrameSyncDesc* const desc,
						  ZHLN_FrameSync* const ZHLN_RESTRICT out_sync);

void ZHLN_DestroyFrameSync(const VkDevice device, ZHLN_FrameSync* const ZHLN_RESTRICT sync,
						   const uint32_t frame_count);

/* --- COMMAND POOL AND BUFFERS --- */

typedef struct {
	VkCommandPool pool;
	uint32_t count;
	VkCommandBuffer buffers[8]; // matches max frames in flight
} ZHLN_CommandPool;

[[nodiscard]]
bool ZHLN_CreateCommandPool(const VkDevice device, const uint32_t queue_family,
							ZHLN_CommandPool* const ZHLN_RESTRICT out_pool);

[[nodiscard]]
bool ZHLN_AllocateCommandBuffers(const VkDevice device, ZHLN_CommandPool* const ZHLN_RESTRICT pool,
								 const uint32_t count);

void ZHLN_ResetCommandPool(const VkDevice device, const ZHLN_CommandPool* const ZHLN_RESTRICT pool);
void ZHLN_DestroyCommandPool(const VkDevice device, ZHLN_CommandPool* const ZHLN_RESTRICT pool);

/* --- FRAME LOOP STRUCTURE --- */

typedef enum {
	ZHLN_FrameResult_Ok,
	ZHLN_FrameResult_Suboptimal,
	ZHLN_FrameResult_OutOfDate, // C++ must rebuild swapchain
	ZHLN_FrameResult_Error,
} ZHLN_FrameResult;

typedef const struct {
	const VkSwapchainKHR swapchain;
	const VkSemaphore image_available;
	const uint64_t timeout_ns; // UINT64_MAX = wait forever
} ZHLN_AcquireDesc;

typedef const struct {
	const VkQueue present_queue;
	const VkSwapchainKHR swapchain;
	const VkSemaphore render_finished;
	const uint32_t image_index;
} ZHLN_PresentDesc;

void ZHLN_WaitAndResetFence(const VkDevice device, const VkFence fence);
ZHLN_FrameResult ZHLN_AcquireImage(const VkDevice device,
								   const ZHLN_AcquireDesc* const ZHLN_RESTRICT desc,
								   uint32_t* const out_image_index);
void ZHLN_SubmitFrame(const VkQueue graphics_queue, const ZHLN_FrameSync* const ZHLN_RESTRICT sync,
					  const VkCommandBuffer cmd);
[[nodiscard]]
ZHLN_FrameResult ZHLN_PresentFrame(const ZHLN_PresentDesc* const ZHLN_RESTRICT desc);

/* --- SHADER MANAGEMENT --- */

typedef const struct {
	const uint32_t* code;					  /**< SPIR-V bytecode */
	const size_t size;						  /**< Size in bytes */
	[[maybe_unused]] const char* entry_point; /**< Optional: defaults to SPIRV-Reflect, if fails
												 then defaults to "main" if NULL */
} ZHLN_ShaderDesc;

typedef struct {
	VkShaderModule handle;
	VkShaderStageFlagBits stage;
	char entry_point[64];
} ZHLN_Shader;

typedef struct {
	ZHLN_Shader vert;
	ZHLN_Shader frag;
} ZHLN_ShaderStages;

typedef const struct {
	const VkDevice device;
	const ZHLN_ShaderDesc vert;
	const ZHLN_ShaderDesc frag;
} ZHLN_ShaderStagesDesc;

[[nodiscard]]
VkShaderModule ZHLN_CreateShaderModule(const VkDevice device,
									   const ZHLN_ShaderDesc* const ZHLN_RESTRICT desc);

// Convenience: creates both stages and returns them paired, destroys both on any failure
[[nodiscard]]
bool ZHLN_CreateShaderStages(const ZHLN_ShaderStagesDesc* const ZHLN_RESTRICT desc,
							 ZHLN_ShaderStages* const ZHLN_RESTRICT out);

void ZHLN_DestroyShaderModule(const VkDevice device, const VkShaderModule module);
void ZHLN_DestroyShaderStages(const VkDevice device, ZHLN_ShaderStages* const ZHLN_RESTRICT stages);

// Populates the two VkPipelineShaderStageCreateInfo entries the pipeline builder needs.
// out_stages must point to an array of 2.
void ZHLN_PopulateShaderStageInfos(const ZHLN_ShaderStages* const ZHLN_RESTRICT stages,
								   VkPipelineShaderStageCreateInfo* const ZHLN_RESTRICT out_stages);

/* --- PIPELINE LAYOUT --- */

typedef const struct {
	const VkDescriptorSetLayout* const ZHLN_RESTRICT set_layouts;
	const uint32_t set_layout_count;
	const VkPushConstantRange* const ZHLN_RESTRICT push_constants;
	const uint32_t push_constant_count;
} ZHLN_PipelineLayoutDesc;

[[nodiscard]]
VkPipelineLayout ZHLN_CreatePipelineLayout(const VkDevice device,
										   const ZHLN_PipelineLayoutDesc* const ZHLN_RESTRICT desc);

void ZHLN_DestroyPipelineLayout(const VkDevice device, const VkPipelineLayout layout);

/* --- GRAPHICS PIPELINE --- */

typedef const struct {
	const ZHLN_ShaderStages* const ZHLN_RESTRICT stages;
	const VkPipelineLayout layout;

	const VkVertexInputBindingDescription* const ZHLN_RESTRICT vertex_bindings;
	const VkVertexInputAttributeDescription* const ZHLN_RESTRICT vertex_attributes;
	const uint32_t vertex_binding_count;
	const uint32_t vertex_attribute_count;

	const VkFormat color_format;
	const VkFormat depth_format;		// VK_FORMAT_UNDEFINED = no depth
	const VkPrimitiveTopology topology; // default: VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
	const VkPolygonMode polygon_mode;	// default: VK_POLYGON_MODE_FILL
	const VkCullModeFlags cull_mode;	// default: VK_CULL_MODE_BACK_BIT
	const VkFrontFace front_face;		// default: VK_FRONT_FACE_COUNTER_CLOCKWISE

	const bool depth_test;
	const bool depth_write;
	const bool blend_enable; // basic src_alpha / one_minus_src_alpha if true
} ZHLN_GraphicsPipelineDesc;

[[nodiscard]]
VkPipeline ZHLN_CreateGraphicsPipeline(const VkDevice device,
									   const ZHLN_GraphicsPipelineDesc* const ZHLN_RESTRICT desc);

void ZHLN_DestroyPipeline(const VkDevice device, const VkPipeline pipeline);

/* --- RENDERING --- */

typedef const struct {
	const VkImageView target_view;
	const VkImageView depth_view; // VK_NULL_HANDLE = no depth
	const VkExtent2D extent;
	const float clear_color[4];
	const float clear_depth; // default 1.0f
} ZHLN_RenderPassDesc;

typedef const struct {
	const VkImage image;
	const VkAccessFlags2 src_access;
	const VkAccessFlags2 dst_access;
	const VkImageLayout src_layout;
	const VkImageLayout dst_layout;
	const VkPipelineStageFlags2 src_stage;
	const VkPipelineStageFlags2 dst_stage;
	const VkImageAspectFlags aspect; // e.g. VK_IMAGE_ASPECT_COLOR_BIT
} ZHLN_ImageBarrierDesc;

void ZHLN_BeginRendering(const VkCommandBuffer cmd,
						 const ZHLN_RenderPassDesc* const ZHLN_RESTRICT desc);
void ZHLN_EndRendering(const VkCommandBuffer cmd);

/* --- FRAME HELPERS --- */

void ZHLN_WaitAndResetFrame(const VkDevice device, const VkFence in_flight_fence,
							const ZHLN_CommandPool* const ZHLN_RESTRICT pool);

// Wraps vkBeginCommandBuffer with one-time-submit flag for frame recording
void ZHLN_BeginCommandBuffer(const VkCommandBuffer cmd);
void ZHLN_EndCommandBuffer(const VkCommandBuffer cmd);

/* --- PUSH CONSTANT HELPERS --- */

void ZHLN_PushConstants(const VkCommandBuffer cmd, const VkPipelineLayout layout,
						const VkShaderStageFlags stages, const void* const ZHLN_RESTRICT data,
						const uint32_t size);

// Typed convenience macro so C doesn't spell out sizeof every time
#ifndef __cplusplus
#define ZHLN_Push(cmd, layout, stages, value)                                                      \
	ZHLN_PushConstants(cmd, layout, stages, &(value), sizeof(value))
#endif

/* --- ERROR HELPERS --- */

const char* ZHLN_VkResultString(const VkResult result);

/* --- EXECUTION HELPERS --- */

typedef const struct {
	const VkBuffer src;
	const VkBuffer dst;
	const VkDeviceSize size;
	const VkDeviceSize src_offset;
	const VkDeviceSize dst_offset;
} ZHLN_BufferCopyDesc;

/**
 * @brief Executes a buffer-to-buffer copy.
 */
void ZHLN_CmdCopyBuffer(const VkCommandBuffer cmd,
						const ZHLN_BufferCopyDesc* const ZHLN_RESTRICT desc);

/**
 * @brief Injects a pipeline barrier for an image (Sync 2).
 */
void ZHLN_CmdImageBarrier(const VkCommandBuffer cmd,
						  const ZHLN_ImageBarrierDesc* const ZHLN_RESTRICT desc);

typedef const struct {
	const VkBuffer buffer;
	const VkImage image;
	const VkImageLayout layout;
	const uint32_t width;
	const uint32_t height;
	const VkDeviceSize buffer_offset; // 0 for tightly packed
	const uint32_t mip_level;		  // 0 for base
	const uint32_t base_array_layer;  // 0 for non-array
} ZHLN_BufferImageCopyDesc;

/**
 * @brief Copies buffer data into an image (e.g. texture upload).
 */
void ZHLN_CmdCopyBufferToImage(const VkCommandBuffer cmd,
							   const ZHLN_BufferImageCopyDesc* const ZHLN_RESTRICT desc);

/* --- SEMAPHORE HELPERS --- */

[[nodiscard]]
VkSemaphore ZHLN_CreateSemaphore(const VkDevice device);
void ZHLN_DestroySemaphore(const VkDevice device, const VkSemaphore semaphore);

#ifdef __cplusplus
}
#endif
