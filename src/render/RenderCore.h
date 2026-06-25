/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 */

/**
 * @file RenderCore.h
 * @brief Project-Zahlen's Zero-overHead vuLkan abstractioN layer (ZHLN)
 *
 * Provides a C23-compliant interface for Vulkan instance management
 * and hardware selection using a data-oriented library approach.
 */

#pragma once
#include <stdbool.h> // We use booleans as keyword but good to include nevertheless
#include <vulkan/vulkan_core.h>

#define ZHLN_RESTRICT __restrict

#ifdef __cplusplus
extern "C" {
#endif

/* --- INSTANCE MANAGEMENT --- */

static constexpr auto maxInstanceExtensions = 128;

/**
 * @struct ZHLN_InstanceDesc
 * @brief Configuration for Vulkan Instance initialization.
 */
typedef struct ZHLN_InstanceDesc {
	char app_name[64];		  /**< Application name embedded to satisfy C23 constexpr */
	const uint32_t version;	  /**< Application-specific version (VK_MAKE_API_VERSION) */
	uint32_t extension_count; /**< Number of additional extensions to enable */
	const VkDebugUtilsMessageSeverityFlagsEXT
		severity_flags;			   /**< Severity flags for the validation layer */
	const char* const* extensions; /**< Pointer to list of extension name strings */
	const bool enable_validation;  /**< Toggle for Khronos Validation Layers */
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
VkInstance ZHLN_CreateInstance(const ZHLN_InstanceDesc* ZHLN_RESTRICT desc);

/* --- DEVICE SELECTION --- */

/**
 * @struct ZHLN_PhysicalDeviceInfo
 * @brief Comprehensive snapshot of a Physical Device's capabilities.
 */
typedef struct ZHLN_PhysicalDeviceInfo {
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
typedef struct ZHLN_DeviceSelectDesc {
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
ZHLN_PhysicalDeviceInfo ZHLN_SelectPhysicalDevice(const ZHLN_DeviceSelectDesc* ZHLN_RESTRICT desc);

/* --- DEVICE CREATION --- */

typedef struct ZHLN_DeviceDesc {
	const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
	const char* const* const extensions;
	const uint32_t extension_count;
	const VkPhysicalDeviceFeatures2* features; // nullptr = nothing extra requested
	const bool enable_validation;
} ZHLN_DeviceDesc;

typedef struct ZHLN_Device {
	VkDevice handle;
	VkQueue graphics_queue;
	VkQueue present_queue;
} ZHLN_Device;

[[nodiscard]]
ZHLN_Device ZHLN_CreateDevice(const ZHLN_DeviceDesc* ZHLN_RESTRICT desc);

/* --- SWAPCHAIN --- */

typedef struct ZHLN_SwapchainSupport {
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[64];
	VkPresentModeKHR present_modes[8];
	uint32_t format_count;
	uint32_t present_mode_count;
} ZHLN_SwapchainSupport;

typedef struct ZHLN_SwapchainSupportDesc {
	const VkPhysicalDevice physical;
	const VkSurfaceKHR surface;
} ZHLN_SwapchainSupportDesc;

typedef struct ZHLN_SwapchainDesc {
	const ZHLN_Device* const ZHLN_RESTRICT device;
	const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
	const VkSurfaceKHR surface;
	const uint32_t width;
	const uint32_t height;
	const bool vsync;
	const VkSwapchainKHR old_swapchain; // VK_NULL_HANDLE on first create
} ZHLN_SwapchainDesc;

typedef struct ZHLN_Swapchain {
	VkSwapchainKHR handle;
	VkImage images[8];
	VkImageView views[8];
	uint32_t image_count;
	VkFormat format;
	VkExtent2D extent;
} ZHLN_Swapchain;

[[nodiscard]]
ZHLN_SwapchainSupport
ZHLN_QuerySwapchainSupport(const ZHLN_SwapchainSupportDesc* ZHLN_RESTRICT desc);

[[nodiscard]]
ZHLN_Swapchain ZHLN_CreateSwapchain(const ZHLN_SwapchainDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroySwapchain(VkDevice device, ZHLN_Swapchain* ZHLN_RESTRICT swapchain);

/* --- SYNC PRIMITIVES --- */

typedef struct ZHLN_FrameSync {
	VkSemaphore image_available;
	VkSemaphore render_finished;
	VkFence in_flight;
} ZHLN_FrameSync;

typedef struct ZHLN_FrameSyncDesc {
	const VkDevice device;
	const uint32_t frame_count;
} ZHLN_FrameSyncDesc;

// out_sync must point to an array of at least desc->frame_count
[[nodiscard]]
bool ZHLN_CreateFrameSync(const ZHLN_FrameSyncDesc* desc, ZHLN_FrameSync* ZHLN_RESTRICT out_sync);

void ZHLN_DestroyFrameSync(VkDevice device, ZHLN_FrameSync* ZHLN_RESTRICT sync,
						   uint32_t frame_count);

/* --- COMMAND POOL AND BUFFERS --- */

typedef struct ZHLN_CommandPool {
	VkCommandPool pool;
	uint32_t count;
	VkCommandBuffer buffers[256];
} ZHLN_CommandPool;

[[nodiscard]]
bool ZHLN_CreateCommandPool(VkDevice device, uint32_t queue_family,
							ZHLN_CommandPool* ZHLN_RESTRICT out_pool);

[[nodiscard]]
bool ZHLN_AllocateCommandBuffers(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool,
								 uint32_t count);

void ZHLN_ResetCommandPool(VkDevice device, const ZHLN_CommandPool* ZHLN_RESTRICT pool);
void ZHLN_DestroyCommandPool(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool);

/* --- FRAME LOOP STRUCTURE --- */

typedef enum : uint8_t {
	ZHLN_FrameResult_Ok,
	ZHLN_FrameResult_Suboptimal,
	ZHLN_FrameResult_OutOfDate, // C++ must rebuild swapchain
	ZHLN_FrameResult_DeviceLost,
	ZHLN_FrameResult_Error,
} ZHLN_FrameResult;

typedef struct ZHLN_AcquireDesc {
	const VkSwapchainKHR swapchain;
	const VkSemaphore image_available;
	const uint64_t timeout_ns; // UINT64_MAX = wait forever
} ZHLN_AcquireDesc;

typedef struct ZHLN_PresentDesc {
	const VkQueue present_queue;
	const VkSwapchainKHR swapchain;
	const VkSemaphore render_finished;
	const uint32_t image_index;
} ZHLN_PresentDesc;

void ZHLN_WaitAndResetFence(VkDevice device, VkFence fence);
ZHLN_FrameResult ZHLN_AcquireImage(VkDevice device, const ZHLN_AcquireDesc* ZHLN_RESTRICT desc,
								   uint32_t* out_image_index);
void ZHLN_SubmitFrame(VkQueue graphics_queue, const ZHLN_FrameSync* ZHLN_RESTRICT sync,
					  VkCommandBuffer cmd);
[[nodiscard]]
ZHLN_FrameResult ZHLN_PresentFrame(const ZHLN_PresentDesc* ZHLN_RESTRICT desc);

/* --- SHADER MANAGEMENT --- */

typedef struct ZHLN_ShaderDesc {
	const uint32_t* code;					  /**< SPIR-V bytecode */
	const size_t size;						  /**< Size in bytes */
	[[maybe_unused]] const char* entry_point; /**< Optional: defaults to SPIRV-Reflect, if fails
											 then defaults to "main" if NULL */
} ZHLN_ShaderDesc;

typedef struct ZHLN_Shader {
	VkShaderModule handle;
	VkShaderStageFlagBits stage;
	char entry_point[64];
} ZHLN_Shader;

typedef struct ZHLN_ShaderStages {
	ZHLN_Shader vert;
	ZHLN_Shader frag;
} ZHLN_ShaderStages;

typedef struct ZHLN_ShaderStagesDesc {
	const VkDevice device;
	const ZHLN_ShaderDesc vert;
	const ZHLN_ShaderDesc frag;
} ZHLN_ShaderStagesDesc;

[[nodiscard]]
VkShaderModule ZHLN_CreateShaderModule(VkDevice device, const ZHLN_ShaderDesc* ZHLN_RESTRICT desc);

// Convenience: creates both stages and returns them paired, destroys both on any failure
[[nodiscard]]
bool ZHLN_CreateShaderStages(const ZHLN_ShaderStagesDesc* ZHLN_RESTRICT desc,
							 ZHLN_ShaderStages* ZHLN_RESTRICT out);

void ZHLN_DestroyShaderModule(VkDevice device, VkShaderModule module);
void ZHLN_DestroyShaderStages(VkDevice device, ZHLN_ShaderStages* ZHLN_RESTRICT stages);

// Populates the two VkPipelineShaderStageCreateInfo entries the pipeline builder needs.
// out_stages must point to an array of 2.
[[nodiscard]] uint32_t
ZHLN_PopulateShaderStageInfos(const ZHLN_ShaderStages* ZHLN_RESTRICT stages,
							  VkPipelineShaderStageCreateInfo* ZHLN_RESTRICT out_stages,
							  const VkSpecializationInfo* spec_info);

/* --- PIPELINE LAYOUT --- */

typedef struct ZHLN_PipelineLayoutDesc {
	const VkDescriptorSetLayout* const ZHLN_RESTRICT set_layouts;
	const uint32_t set_layout_count;
	const VkPushConstantRange* const ZHLN_RESTRICT push_constants;
	const uint32_t push_constant_count;
} ZHLN_PipelineLayoutDesc;

[[nodiscard]]
VkPipelineLayout ZHLN_CreatePipelineLayout(VkDevice device,
										   const ZHLN_PipelineLayoutDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout);

/* --- GRAPHICS PIPELINE --- */

typedef struct ZHLN_GraphicsPipelineDesc {
	const ZHLN_ShaderStages* const ZHLN_RESTRICT stages;
	const VkPipelineLayout layout;

	const VkVertexInputBindingDescription* const ZHLN_RESTRICT vertex_bindings;
	const VkVertexInputAttributeDescription* const ZHLN_RESTRICT vertex_attributes;
	const uint32_t vertex_binding_count;
	const uint32_t attribute_count;

	const VkFormat* const ZHLN_RESTRICT color_formats;
	const uint32_t color_format_count;
	const VkFormat depth_format;		// VK_FORMAT_UNDEFINED = no depth
	const VkPrimitiveTopology topology; // default: VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
	const VkPolygonMode polygon_mode;	// default: VK_POLYGON_MODE_FILL
	const VkCullModeFlags cull_mode;	// default: VK_CULL_MODE_BACK_BIT
	const VkFrontFace front_face;		// default: VK_FRONT_FACE_COUNTER_CLOCKWISE

	const bool depth_test;
	const bool depth_write;
	const bool blend_enable;   // basic src_alpha / one_minus_src_alpha if true
	const bool additive_blend; // Explicitly route additive blend configuration
	const VkSpecializationInfo* specialization_info;
} ZHLN_GraphicsPipelineDesc;

[[nodiscard]]
VkPipeline ZHLN_CreateGraphicsPipeline(VkDevice device,
									   const ZHLN_GraphicsPipelineDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyPipeline(VkDevice device, VkPipeline pipeline);

/* --- RENDERING --- */

typedef struct ZHLN_RenderPassDesc {
	const VkImageView target_views[4]; // Array instead of single view
	const uint32_t target_count;	   // How many targets are we writing to?
	const VkImageView depth_view;
	const VkExtent2D extent;
	const float clear_color[4];
	const float clear_depth;
	const bool use_secondaries;
} ZHLN_RenderPassDesc;

typedef struct ZHLN_ImageBarrierDesc {
	const VkImage image;
	const VkAccessFlags2 src_access;
	const VkAccessFlags2 dst_access;
	const VkImageLayout src_layout;
	const VkImageLayout dst_layout;
	const VkPipelineStageFlags2 src_stage;
	const VkPipelineStageFlags2 dst_stage;
	const VkImageAspectFlags aspect; // e.g. VK_IMAGE_ASPECT_COLOR_BIT
	const uint32_t base_mip;
	const uint32_t mip_count; //  (use VK_REMAINING_MIP_LEVELS for all)
} ZHLN_ImageBarrierDesc;

void ZHLN_BeginRendering(VkCommandBuffer cmd, const ZHLN_RenderPassDesc* ZHLN_RESTRICT desc);
void ZHLN_EndRendering(VkCommandBuffer cmd);

typedef struct ZHLN_FrameSubmitDesc {
	const VkQueue graphicsQueue;
	const VkQueue presentQueue;
	const VkCommandBuffer cmd;
	const VkSemaphore imageAvailable;
	const VkSemaphore renderFinished;
	const VkFence inFlight;
	const VkSwapchainKHR swapchain;
	const uint32_t imageIndex;
} ZHLN_FrameSubmitDesc;

[[nodiscard]]
ZHLN_FrameResult ZHLN_SubmitAndPresent(const ZHLN_FrameSubmitDesc* ZHLN_RESTRICT desc);

/* --- FRAME HELPERS --- */

typedef struct ZHLN_SecondaryCmdDesc {
	const VkFormat color_format;
	const VkFormat depth_format;
} ZHLN_SecondaryCmdDesc;

void ZHLN_BeginSecondaryCommandBuffer(VkCommandBuffer cmd,
									  const ZHLN_SecondaryCmdDesc* ZHLN_RESTRICT desc);
bool ZHLN_AllocateSecondaryCommandBuffers(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool,
										  uint32_t count);

ZHLN_FrameResult ZHLN_WaitAndResetFrame(VkDevice device, VkFence in_flight_fence,
										const ZHLN_CommandPool* ZHLN_RESTRICT pool);

// Wraps vkBeginCommandBuffer with one-time-submit flag for frame recording
void ZHLN_BeginCommandBuffer(VkCommandBuffer cmd);
void ZHLN_EndCommandBuffer(VkCommandBuffer cmd);

/* --- FRAME LOOP COHESION --- */

/**
 * @brief Waits for the in-flight fence, resets it, and acquires the next swapchain image.
 */
[[nodiscard]]
ZHLN_FrameResult ZHLN_WaitAndAcquireImage(VkDevice device, VkSwapchainKHR swapchain,
										  const ZHLN_FrameSync* ZHLN_RESTRICT sync,
										  const ZHLN_CommandPool* ZHLN_RESTRICT pool,
										  uint32_t* out_image_index);

/* --- PUSH CONSTANT HELPERS --- */

void ZHLN_PushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
						const void* ZHLN_RESTRICT data, uint32_t size);

// Typed convenience macro so C doesn't spell out sizeof every time
#ifndef __cplusplus
#define ZHLN_Push(cmd, layout, stages, value)                                                      \
	ZHLN_PushConstants(cmd, layout, stages, &(value), sizeof(value))
#endif

/* --- ERROR HELPERS --- */

const char* ZHLN_VkResultString(VkResult result);

/* --- EXECUTION HELPERS --- */

typedef struct ZHLN_BufferCopyDesc {
	const VkBuffer src;
	const VkBuffer dst;
	const VkDeviceSize size;
	const VkDeviceSize src_offset;
	const VkDeviceSize dst_offset;
} ZHLN_BufferCopyDesc;

/**
 * @brief Executes a buffer-to-buffer copy.
 */
void ZHLN_CmdCopyBuffer(VkCommandBuffer cmd, const ZHLN_BufferCopyDesc* ZHLN_RESTRICT desc);

/**
 * @brief Injects a pipeline barrier for an image (Sync 2).
 */
void ZHLN_CmdImageBarrier(VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_BufferImageCopyDesc {
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
void ZHLN_CmdCopyBufferToImage(VkCommandBuffer cmd,
							   const ZHLN_BufferImageCopyDesc* ZHLN_RESTRICT desc);

/* --- SEMAPHORE HELPERS --- */

[[nodiscard]]
VkSemaphore ZHLN_CreateSemaphore(VkDevice device);
void ZHLN_DestroySemaphore(VkDevice device, VkSemaphore semaphore);

/* --- IMAGE VIEW HELPERS --- */

typedef struct ZHLN_ImageViewDesc {
	const VkImage image;
	const VkFormat format;
	const VkImageAspectFlags aspect;
	const uint32_t mip_levels;	 // default 1
	const uint32_t array_layers; // default 1
	const VkImageViewType view_type;
	const uint32_t base_array_layer;
} ZHLN_ImageViewDesc;

[[nodiscard]]
VkImageView ZHLN_CreateImageView(VkDevice device, const ZHLN_ImageViewDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyImageView(VkDevice device, VkImageView view);

/* --- SAMPLER HELPERS --- */

void ZHLN_DestroySampler(VkDevice device, VkSampler sampler);
void ZHLN_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout);
void ZHLN_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool);

/* --- COMPUTE PIPELINE --- */

typedef struct ZHLN_ComputePipelineDesc {
	const ZHLN_ShaderDesc shader;
	const VkPipelineLayout layout;
} ZHLN_ComputePipelineDesc;

[[nodiscard]]
VkPipeline ZHLN_CreateComputePipeline(VkDevice device,
									  const ZHLN_ComputePipelineDesc* ZHLN_RESTRICT desc);

void ZHLN_CmdDispatch(VkCommandBuffer cmd, uint32_t group_count_x, uint32_t group_count_y,
					  uint32_t group_count_z);

/* --- MIPMAPPING --- */

/**
 * @brief Generates mipmaps for a color image using linear blits.
 * Transitions image to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
 */
void ZHLN_GenerateMipmaps(VkCommandBuffer cmd, VkImage image, int32_t width, int32_t height,
						  uint32_t mip_levels);

/* --- MEMORY BARRIERS --- */

typedef struct ZHLN_MemoryBarrierDesc {
	VkPipelineStageFlags2 src_stage;
	VkAccessFlags2 src_access;
	VkPipelineStageFlags2 dst_stage;
	VkAccessFlags2 dst_access;
} ZHLN_MemoryBarrierDesc;

void ZHLN_CmdMemoryBarrier(VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc* ZHLN_RESTRICT desc);

/* --- HARDWARE RAY TRACING --- */

VkDeviceAddress ZHLN_GetBufferDeviceAddress(VkDevice device, VkBuffer buffer);

typedef struct ZHLN_RayTracingContext {
	VkDevice device;
	PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes;
	PFN_vkCreateAccelerationStructureKHR create_as;
	PFN_vkCmdBuildAccelerationStructuresKHR build_as;
	PFN_vkGetAccelerationStructureDeviceAddressKHR get_address;
	PFN_vkDestroyAccelerationStructureKHR destroy_as;
} ZHLN_RayTracingContext;

[[nodiscard]]
bool ZHLN_InitRayTracingContext(VkDevice device, ZHLN_RayTracingContext* ZHLN_RESTRICT out_ctx);

typedef enum ZHLN_AccelerationStructureType : uint8_t {
	ZHLN_AS_TYPE_TOP_LEVEL = 0,
	ZHLN_AS_TYPE_BOTTOM_LEVEL = 1
} ZHLN_AccelerationStructureType;

typedef struct ZHLN_AccelerationStructureSizes {
	VkDeviceSize acceleration_structure_size;
	VkDeviceSize build_scratch_size;
	VkDeviceSize update_scratch_size;
} ZHLN_AccelerationStructureSizes;

typedef struct ZHLN_BlasGeometryDesc {
	VkDeviceAddress vertex_data;
	uint32_t vertex_stride;
	uint32_t max_vertex;
	VkFormat vertex_format;
	VkDeviceAddress index_data;
	VkIndexType index_type;
} ZHLN_BlasGeometryDesc;

typedef struct ZHLN_TlasGeometryDesc {
	VkDeviceAddress instance_data;
} ZHLN_TlasGeometryDesc;

void ZHLN_GetBlasSizes(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx,
					   const ZHLN_BlasGeometryDesc* ZHLN_RESTRICT desc, uint32_t primitive_count,
					   ZHLN_AccelerationStructureSizes* ZHLN_RESTRICT out_sizes);
void ZHLN_GetTlasSizes(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, uint32_t instance_count,
					   ZHLN_AccelerationStructureSizes* ZHLN_RESTRICT out_sizes);

[[nodiscard]]
VkAccelerationStructureKHR ZHLN_CreateAS(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx,
										 VkBuffer buffer, VkDeviceSize size,
										 ZHLN_AccelerationStructureType type);
void ZHLN_DestroyAS(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, VkAccelerationStructureKHR as);
[[nodiscard]]
VkDeviceAddress ZHLN_GetASAddress(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx,
								  VkAccelerationStructureKHR as);

void ZHLN_CmdBuildBlas(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, VkCommandBuffer cmd,
					   const ZHLN_BlasGeometryDesc* ZHLN_RESTRICT desc,
					   VkAccelerationStructureKHR dst_as, VkDeviceAddress scratch,
					   uint32_t primitive_count);
void ZHLN_CmdBuildTlas(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, VkCommandBuffer cmd,
					   const ZHLN_TlasGeometryDesc* ZHLN_RESTRICT desc,
					   VkAccelerationStructureKHR dst_as, VkDeviceAddress scratch,
					   uint32_t instance_count);

#ifdef __cplusplus
}
#endif
