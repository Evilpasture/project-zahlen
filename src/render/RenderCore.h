/**
 * @file RenderCore.h
 * @brief Zero-overHead vuLkan abstractioN layer (ZHLN)
 *
 * Provides a C23-compliant interface for Vulkan instance management
 * and hardware selection using a data-oriented library approach.
 */

#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h> // We use booleans as keyword but good to include nevertheless

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
	bool enable_validation;	  /**< Toggle for Khronos Validation Layers */
	uint32_t extension_count; /**< Number of additional extensions to enable */
	const char** extensions;  /**< Pointer to list of extension name strings */
} ZHLN_InstanceDesc;

/**
 * @brief Default instance configuration for ZHLN.
 * Uses C23 array-copy initialization for the application name.
 */
static constexpr ZHLN_InstanceDesc ZHLN_DEFAULT_INSTANCE_DESC = {
	.app_name = "ZHLN Engine",
	.version = VK_MAKE_API_VERSION(0, 1, 0, 0),
	.enable_validation = true,
	.extension_count = 0,
	.extensions = nullptr};

/**
 * @brief Creates a Vulkan Instance with debug messenger attached to pNext.
 * @param desc Pointer to initialization descriptor.
 * @return VkInstance handle or VK_NULL_HANDLE on critical failure.
 */
[[nodiscard]]
VkInstance ZHLN_CreateInstance(const ZHLN_InstanceDesc* desc);

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
typedef int32_t (*ZHLN_DeviceScoreFn)(const ZHLN_PhysicalDeviceInfo* info, void* userdata);

/**
 * @struct ZHLN_DeviceSelectDesc
 * @brief Constraints and logic for selecting the optimal GPU.
 */
typedef struct {
	VkInstance instance;		 /**< Required: Active Vulkan instance */
	VkSurfaceKHR surface;		 /**< Optional: Surface for present support checks */
	ZHLN_DeviceScoreFn score_fn; /**< Optional: Scoring logic (null = discrete GPU preference) */
	void* score_userdata;		 /**< Context passed to the scoring function */
} ZHLN_DeviceSelectDesc;

/**
 * @brief Queries all GPUs and selects the best candidate based on scoring.
 * @param desc Selection criteria and context.
 * @return Populated info struct. Check .handle == VK_NULL_HANDLE for failure.
 */
[[nodiscard]]
ZHLN_PhysicalDeviceInfo ZHLN_SelectPhysicalDevice(const ZHLN_DeviceSelectDesc* desc);

/* --- DEVICE CREATION --- */

typedef struct {
	ZHLN_PhysicalDeviceInfo* physical;
	const char** const extensions;
	uint32_t extension_count;
	VkPhysicalDeviceFeatures2* features; // nullptr = nothing extra requested
	bool enable_validation;
} ZHLN_DeviceDesc;

typedef struct {
	VkDevice handle;
	VkQueue graphics_queue;
	VkQueue present_queue;
} ZHLN_Device;

[[nodiscard]]
ZHLN_Device ZHLN_CreateDevice(const ZHLN_DeviceDesc* desc);

/* --- SWAPCHAIN --- */

typedef struct {
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[64];
	VkPresentModeKHR present_modes[8];
	uint32_t format_count;
	uint32_t present_mode_count;
} ZHLN_SwapchainSupport;

typedef struct {
	VkPhysicalDevice physical;
	VkSurfaceKHR surface;
} ZHLN_SwapchainSupportDesc;

typedef struct {
	ZHLN_Device* device;
	ZHLN_PhysicalDeviceInfo* physical;
	VkSurfaceKHR surface;
	uint32_t width;
	uint32_t height;
	bool vsync;
	VkSwapchainKHR old_swapchain; // VK_NULL_HANDLE on first create
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
ZHLN_SwapchainSupport ZHLN_QuerySwapchainSupport(const ZHLN_SwapchainSupportDesc* desc);

[[nodiscard]]
ZHLN_Swapchain ZHLN_CreateSwapchain(const ZHLN_SwapchainDesc* desc);

void ZHLN_DestroySwapchain(VkDevice device, ZHLN_Swapchain* swapchain);

/* --- SYNC PRIMITIVES --- */

typedef struct {
	VkSemaphore image_available;
	VkSemaphore render_finished;
	VkFence in_flight;
} ZHLN_FrameSync;

typedef struct {
	VkDevice device;
	uint32_t frame_count;
} ZHLN_FrameSyncDesc;

// out_sync must point to an array of at least desc->frame_count
[[nodiscard]]
bool ZHLN_CreateFrameSync(const ZHLN_FrameSyncDesc* desc, ZHLN_FrameSync* out_sync);

void ZHLN_DestroyFrameSync(VkDevice device, ZHLN_FrameSync* sync, uint32_t frame_count);

/* --- COMMAND POOL AND BUFFERS --- */

typedef struct {
	VkCommandPool pool;
	uint32_t count;
	VkCommandBuffer buffers[8]; // matches max frames in flight
} ZHLN_CommandPool;

[[nodiscard]]
bool ZHLN_CreateCommandPool(VkDevice device, uint32_t queue_family, ZHLN_CommandPool* out_pool);

[[nodiscard]]
bool ZHLN_AllocateCommandBuffers(VkDevice device, ZHLN_CommandPool* pool, uint32_t count);

void ZHLN_ResetCommandPool(VkDevice device, ZHLN_CommandPool* pool);
void ZHLN_DestroyCommandPool(VkDevice device, ZHLN_CommandPool* pool);

/* --- FRAME LOOP STRUCTURE --- */

typedef enum {
	ZHLN_FrameResult_Ok,
	ZHLN_FrameResult_Suboptimal,
	ZHLN_FrameResult_OutOfDate, // C++ must rebuild swapchain
	ZHLN_FrameResult_Error,
} ZHLN_FrameResult;

typedef struct {
	VkSwapchainKHR swapchain;
	VkSemaphore image_available;
	uint64_t timeout_ns; // UINT64_MAX = wait forever
} ZHLN_AcquireDesc;

typedef struct {
	VkQueue present_queue;
	VkSwapchainKHR swapchain;
	VkSemaphore render_finished;
	uint32_t image_index;
} ZHLN_PresentDesc;

void ZHLN_WaitAndResetFence(VkDevice device, VkFence fence);
ZHLN_FrameResult ZHLN_AcquireImage(VkDevice device, const ZHLN_AcquireDesc* desc,
								   uint32_t* out_image_index);
void ZHLN_SubmitFrame(VkQueue graphics_queue, const ZHLN_FrameSync* sync, VkCommandBuffer cmd);
ZHLN_FrameResult ZHLN_PresentFrame(const ZHLN_PresentDesc* desc);

/* --- SHADER MANAGEMENT --- */

typedef struct {
	const uint32_t* code; /**< SPIR-V bytecode */
	size_t size;		  /**< Size in bytes */
} ZHLN_ShaderDesc;

typedef struct {
	VkShaderModule handle;
	VkShaderStageFlagBits stage; // Carried so pipeline builder doesn't need to track it separately
} ZHLN_Shader;

typedef struct {
	ZHLN_Shader vert;
	ZHLN_Shader frag;
} ZHLN_ShaderStages;

typedef struct {
	VkDevice device;
	ZHLN_ShaderDesc vert;
	ZHLN_ShaderDesc frag;
} ZHLN_ShaderStagesDesc;

[[nodiscard]]
VkShaderModule ZHLN_CreateShaderModule(VkDevice device, const ZHLN_ShaderDesc* desc);

// Convenience: creates both stages and returns them paired, destroys both on any failure
[[nodiscard]]
bool ZHLN_CreateShaderStages(const ZHLN_ShaderStagesDesc* desc, ZHLN_ShaderStages* out);

void ZHLN_DestroyShaderModule(VkDevice device, VkShaderModule module);
void ZHLN_DestroyShaderStages(VkDevice device, ZHLN_ShaderStages* stages);

// Populates the two VkPipelineShaderStageCreateInfo entries the pipeline builder needs.
// out_stages must point to an array of 2.
void ZHLN_PopulateShaderStageInfos(const ZHLN_ShaderStages* stages,
								   VkPipelineShaderStageCreateInfo* out_stages);

/* --- PIPELINE LAYOUT --- */

typedef struct {
    VkDescriptorSetLayout*  set_layouts;
    uint32_t                set_layout_count;
    VkPushConstantRange*    push_constants;
    uint32_t                push_constant_count;
} ZHLN_PipelineLayoutDesc;

[[nodiscard]]
VkPipelineLayout ZHLN_CreatePipelineLayout(VkDevice device, const ZHLN_PipelineLayoutDesc* desc);

void ZHLN_DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout);

/* --- GRAPHICS PIPELINE --- */

typedef struct {
    ZHLN_ShaderStages*      stages;
    VkPipelineLayout        layout;
    VkFormat                color_format;
    VkFormat                depth_format;   // VK_FORMAT_UNDEFINED = no depth
    VkPrimitiveTopology     topology;       // default: VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    VkPolygonMode           polygon_mode;   // default: VK_POLYGON_MODE_FILL
    VkCullModeFlags         cull_mode;      // default: VK_CULL_MODE_BACK_BIT
    VkFrontFace             front_face;     // default: VK_FRONT_FACE_COUNTER_CLOCKWISE
    bool                    depth_test;
    bool                    depth_write;
    bool                    blend_enable;   // basic src_alpha / one_minus_src_alpha if true
} ZHLN_GraphicsPipelineDesc;

[[nodiscard]]
VkPipeline ZHLN_CreateGraphicsPipeline(VkDevice device, const ZHLN_GraphicsPipelineDesc* desc);

void ZHLN_DestroyPipeline(VkDevice device, VkPipeline pipeline);

/* --- RENDERING --- */

typedef struct {
    VkImageView     target_view;
    VkImageView     depth_view;     // VK_NULL_HANDLE = no depth
    VkExtent2D      extent;
    float           clear_color[4];
    float           clear_depth;    // default 1.0f
} ZHLN_RenderPassDesc;

typedef struct {
    VkImage         image;
    VkAccessFlags2  src_access;
    VkAccessFlags2  dst_access;
    VkImageLayout   src_layout;
    VkImageLayout   dst_layout;
    VkPipelineStageFlags2 src_stage;
    VkPipelineStageFlags2 dst_stage;
    VkImageAspectFlags aspect; // e.g. VK_IMAGE_ASPECT_COLOR_BIT
} ZHLN_ImageBarrierDesc;

void ZHLN_BeginRendering(VkCommandBuffer cmd, const ZHLN_RenderPassDesc* desc);
void ZHLN_EndRendering(VkCommandBuffer cmd);

/* --- FRAME HELPERS --- */

void ZHLN_WaitAndResetFrame(VkDevice device, VkFence in_flight_fence, ZHLN_CommandPool* pool);

// Wraps vkBeginCommandBuffer with one-time-submit flag for frame recording
void ZHLN_BeginCommandBuffer(VkCommandBuffer cmd);
void ZHLN_EndCommandBuffer(VkCommandBuffer cmd);

/* --- PUSH CONSTANT HELPERS --- */

void ZHLN_PushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
                        VkShaderStageFlags stages, const void* data, uint32_t size);

// Typed convenience macro so C doesn't spell out sizeof every time
#ifndef __cplusplus
#define ZHLN_Push(cmd, layout, stages, value) \
    ZHLN_PushConstants(cmd, layout, stages, &(value), sizeof(value))
#endif

/* --- ERROR HELPERS --- */

const char* ZHLN_VkResultString(VkResult result);

/* --- EXECUTION HELPERS --- */

typedef struct {
    VkBuffer src;
    VkBuffer dst;
    VkDeviceSize size;
    VkDeviceSize src_offset;
    VkDeviceSize dst_offset;
} ZHLN_BufferCopyDesc;

/**
 * @brief Executes a buffer-to-buffer copy.
 */
void ZHLN_CmdCopyBuffer(VkCommandBuffer cmd, const ZHLN_BufferCopyDesc* desc);

/**
 * @brief Injects a pipeline barrier for an image (Sync 2).
 */
void ZHLN_CmdImageBarrier(VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc* desc);

typedef struct {
    VkBuffer        buffer;
    VkImage         image;
    VkImageLayout   layout;
    uint32_t        width;
    uint32_t        height;
    VkDeviceSize    buffer_offset;      // 0 for tightly packed
    uint32_t        mip_level;          // 0 for base
    uint32_t        base_array_layer;   // 0 for non-array
} ZHLN_BufferImageCopyDesc;

/**
 * @brief Copies buffer data into an image (e.g. texture upload).
 */
void ZHLN_CmdCopyBufferToImage(VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc* desc);

/* --- SEMAPHORE HELPERS --- */

[[nodiscard]]
VkSemaphore ZHLN_CreateSemaphore(VkDevice device);
void ZHLN_DestroySemaphore(VkDevice device, VkSemaphore semaphore);

#ifdef __cplusplus
}
#endif