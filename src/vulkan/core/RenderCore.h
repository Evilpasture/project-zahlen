/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <stdbool.h>
#include <volk.h>

#ifndef ZHLN_RESTRICT
#define ZHLN_RESTRICT __restrict
#endif

#ifdef __cplusplus
extern "C" {
#endif

static constexpr auto maxInstanceExtensions = 128;

typedef void (*ZHLN_DebugHookFn)(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity);

typedef struct ZHLN_DebugForwarding {
    ZHLN_DebugHookFn hook;
    void*            userdata;
} ZHLN_DebugForwarding;

typedef enum ZHLN_ValidationMode : uint8_t { ZHLN_VALIDATION_OFF = 0, ZHLN_VALIDATION_ON = 1, ZHLN_VALIDATION_GPU = 2 } ZHLN_ValidationMode;

typedef struct ZHLN_InstanceDesc {
    char                                      app_name[64];
    const uint32_t                            version;
    uint32_t                                  extension_count;
    const VkDebugUtilsMessageSeverityFlagsEXT severity_flags;
    const char* const*                        extensions;
    const ZHLN_ValidationMode                 validation_mode;
    ZHLN_DebugForwarding*                     debug;
} ZHLN_InstanceDesc;

static constexpr ZHLN_InstanceDesc ZHLN_DEFAULT_INSTANCE_DESC = {
    .app_name        = "ZHLN Engine",
    .version         = VK_MAKE_API_VERSION(0, 1, 0, 0),
    .extension_count = 0,
    .severity_flags  = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .extensions      = nullptr,
    .validation_mode = ZHLN_VALIDATION_ON,
};

static constexpr ZHLN_InstanceDesc ZHLN_VERBOSE_INSTANCE_DESC = {
    .app_name = "ZHLN Engine",
    .version  = VK_MAKE_API_VERSION(0, 1, 0, 0),

    .extension_count = 0,
    .severity_flags  = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
    .extensions      = nullptr,
    .validation_mode = ZHLN_VALIDATION_ON,
};

[[nodiscard]]
VkResult ZHLN_EnsureVulkanLoader(void);

[[nodiscard]]
VkInstance ZHLN_CreateInstance(const ZHLN_InstanceDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_PhysicalDeviceInfo {
    VkPhysicalDevice                  handle;
    VkPhysicalDeviceProperties2       properties;
    VkPhysicalDeviceFeatures2         features;
    VkPhysicalDeviceMemoryProperties2 memory;
    uint32_t                          graphics_family;
    uint32_t                          present_family;
    uint32_t                          transfer_family;
    uint32_t                          compute_family;
    bool                              has_graphics;
    bool                              has_present;
    bool                              has_transfer;
    bool                              has_compute;
} ZHLN_PhysicalDeviceInfo;

typedef int32_t (*ZHLN_DeviceScoreFn)(const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT info, const void* const ZHLN_RESTRICT userdata);

typedef struct ZHLN_DeviceSelectDesc {
    const VkInstance         instance;
    const VkSurfaceKHR       surface;
    const ZHLN_DeviceScoreFn score_fn;
    const void*              score_userdata;
} ZHLN_DeviceSelectDesc;

[[nodiscard]]
ZHLN_PhysicalDeviceInfo ZHLN_SelectPhysicalDevice(const ZHLN_DeviceSelectDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_DeviceDesc {
    const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
    const char* const* const                           extensions;
    const uint32_t                                     extension_count;
    const VkPhysicalDeviceFeatures2*                   features;
    const bool                                         enable_validation;
} ZHLN_DeviceDesc;

typedef struct ZHLN_Device {
    VkDevice handle;
    VkQueue  graphics_queue;
    VkQueue  present_queue;
    VkQueue  transfer_queue;
    VkQueue  compute_queue;

    bool descriptor_heap_enabled;

    bool mesh_shader_enabled;

    bool ray_tracing_enabled;
} ZHLN_Device;

typedef struct ZHLN_MeshShaderLimits {
    uint32_t max_mesh_output_vertices;
    uint32_t max_mesh_output_primitives;
    uint32_t max_task_work_group_invocations;
    uint32_t max_mesh_work_group_invocations;
    uint32_t max_preferred_task_work_group_invocations;
    uint32_t max_preferred_mesh_work_group_invocations;
    bool     prefers_compact_vertex_output;
    bool     supported;
} ZHLN_MeshShaderLimits;

[[nodiscard]]
VkDebugUtilsMessengerEXT ZHLN_CreateDebugMessenger(VkInstance instance, VkDebugUtilsMessageSeverityFlagsEXT severity, ZHLN_DebugForwarding* debug);

void ZHLN_DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger);

[[nodiscard]]
ZHLN_MeshShaderLimits ZHLN_QueryMeshShaderLimits(VkPhysicalDevice physical);

[[nodiscard]]
bool ZHLN_MeshShaderLimitsSufficient(const ZHLN_MeshShaderLimits* ZHLN_RESTRICT limits);

[[nodiscard]]
VkResult ZHLN_CreateDevice(const ZHLN_DeviceDesc* ZHLN_RESTRICT desc, ZHLN_Device* ZHLN_RESTRICT out);

typedef struct ZHLN_SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR       formats[64];
    VkPresentModeKHR         present_modes[8];
    uint32_t                 format_count;
    uint32_t                 present_mode_count;
} ZHLN_SwapchainSupport;

typedef struct ZHLN_SwapchainSupportDesc {
    const VkPhysicalDevice physical;
    const VkSurfaceKHR     surface;
} ZHLN_SwapchainSupportDesc;

typedef struct ZHLN_SwapchainDesc {
    const ZHLN_Device* const ZHLN_RESTRICT             device;
    const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
    const VkSurfaceKHR                                 surface;
    const uint32_t                                     width;
    const uint32_t                                     height;
    const bool                                         vsync;
    const VkPresentModeKHR                             present_mode;
    const bool                                         enable_present_timing;
    const VkSwapchainKHR                               old_swapchain;
} ZHLN_SwapchainDesc;

typedef struct ZHLN_Swapchain {
    VkSwapchainKHR   handle;
    VkImage          images[8];
    VkImageView      views[8];
    uint32_t         image_count;
    VkFormat         format;
    VkExtent2D       extent;
    VkPresentModeKHR present_mode;
} ZHLN_Swapchain;

[[nodiscard]]
ZHLN_SwapchainSupport ZHLN_QuerySwapchainSupport(const ZHLN_SwapchainSupportDesc* ZHLN_RESTRICT desc);

[[nodiscard]]
ZHLN_Swapchain ZHLN_CreateSwapchain(const ZHLN_SwapchainDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroySwapchain(VkDevice device, ZHLN_Swapchain* ZHLN_RESTRICT swapchain);

typedef struct ZHLN_FrameSync {
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkSemaphore compute_timeline;
    VkFence     in_flight;
} ZHLN_FrameSync;

typedef struct ZHLN_FrameSyncDesc {
    const VkDevice device;
    const uint32_t frame_count;
} ZHLN_FrameSyncDesc;

[[nodiscard]]
bool ZHLN_CreateFrameSync(const ZHLN_FrameSyncDesc* desc, ZHLN_FrameSync* ZHLN_RESTRICT outSync);

void ZHLN_DestroyFrameSync(VkDevice device, ZHLN_FrameSync* ZHLN_RESTRICT sync, uint32_t frameCount);

typedef struct ZHLN_CommandPool {
    VkCommandPool   pool;
    uint32_t        count;
    VkCommandBuffer buffers[256];
} ZHLN_CommandPool;

[[nodiscard]]
bool ZHLN_CreateCommandPool(VkDevice device, uint32_t queueFamily, ZHLN_CommandPool* ZHLN_RESTRICT outPool);

[[nodiscard]]
VkResult ZHLN_AllocateCommandBuffers(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool, uint32_t count);

void ZHLN_ResetCommandPool(VkDevice device, const ZHLN_CommandPool* ZHLN_RESTRICT pool);
void ZHLN_DestroyCommandPool(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool);

typedef struct ZHLN_AcquireDesc {
    const VkSwapchainKHR swapchain;
    const VkSemaphore    image_available;
    const uint64_t       timeout_ns;
} ZHLN_AcquireDesc;

typedef struct ZHLN_PresentDesc {
    const VkQueue          present_queue;
    const VkSwapchainKHR   swapchain;
    const VkSemaphore      render_finished;
    const uint32_t         image_index;
    const VkPresentId2KHR* present_id;
} ZHLN_PresentDesc;

void ZHLN_WaitAndResetFence(VkDevice device, VkFence fence);

[[nodiscard]]
VkResult ZHLN_AcquireImage(VkDevice device, const ZHLN_AcquireDesc* ZHLN_RESTRICT desc, uint32_t* outImageIndex);

[[nodiscard]]
VkResult ZHLN_QueueSubmit(
    VkQueue                                        queue,
    uint32_t                                       cmd_count,
    const VkCommandBufferSubmitInfo* ZHLN_RESTRICT cmds,
    uint32_t                                       wait_count,
    const VkSemaphoreSubmitInfo* ZHLN_RESTRICT     waits,
    uint32_t                                       signal_count,
    const VkSemaphoreSubmitInfo* ZHLN_RESTRICT     signals,
    VkFence                                        fence
);

void ZHLN_SubmitFrame(VkQueue graphicsQueue, const ZHLN_FrameSync* ZHLN_RESTRICT sync, VkCommandBuffer cmd);

[[nodiscard]]
VkResult ZHLN_PresentFrame(const ZHLN_PresentDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_ShaderDesc {
    const uint32_t*              code;
    const size_t                 size;
    [[maybe_unused]] const char* entry_point;
} ZHLN_ShaderDesc;

// Borrowed SPIR-V: code must remain alive through pipeline creation and any
// reflection performed on these stages. No Vulkan shader-module handle is owned.
typedef struct ZHLN_Shader {
    const uint32_t*       code;
    size_t                size;
    VkShaderStageFlagBits stage;
    char                  entry_point[64];
    uint32_t              view_mask;
} ZHLN_Shader;

typedef struct ZHLN_ShaderStages {
    ZHLN_Shader task;
    ZHLN_Shader mesh;
    ZHLN_Shader vert;
    ZHLN_Shader frag;
} ZHLN_ShaderStages;

typedef struct ZHLN_ShaderStagesDesc {
    const ZHLN_ShaderDesc vert;
    const ZHLN_ShaderDesc frag;
    const ZHLN_ShaderDesc task;
    const ZHLN_ShaderDesc mesh;
} ZHLN_ShaderStagesDesc;

[[nodiscard]]
uint32_t ZHLN_DetectShaderViewMask(const ZHLN_ShaderDesc* ZHLN_RESTRICT desc);

// Builds handle-free stage metadata, including entry points and view masks.
[[nodiscard]]
bool ZHLN_InitShaderStages(const ZHLN_ShaderStagesDesc* ZHLN_RESTRICT desc, ZHLN_ShaderStages* ZHLN_RESTRICT out);

static constexpr auto ZHLN_MAX_SHADER_STAGES = 3;

static constexpr auto ZHLN_MAX_COLOR_ATTACHMENTS = 8;

// Both output arrays must hold ZHLN_MAX_SHADER_STAGES elements and remain alive
// until vkCreateGraphicsPipelines returns (pNext points into outModuleInfos).
[[nodiscard]] uint32_t ZHLN_PopulateShaderStageInfos(
    const ZHLN_ShaderStages* ZHLN_RESTRICT               stages,
    VkPipelineShaderStageCreateInfo* ZHLN_RESTRICT       outStages,
    VkShaderModuleCreateInfo* ZHLN_RESTRICT              outModuleInfos,
    const VkSpecializationInfo*                          specInfo,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* vsMapping,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* psMapping
);

typedef struct ZHLN_PipelineLayoutDesc {
    const VkDescriptorSetLayout* const ZHLN_RESTRICT set_layouts;
    const uint32_t                                   set_layout_count;
    const VkPushConstantRange* const ZHLN_RESTRICT   push_constants;
    const uint32_t                                   push_constant_count;
} ZHLN_PipelineLayoutDesc;

[[nodiscard]]
VkPipelineLayout ZHLN_CreatePipelineLayout(VkDevice device, const ZHLN_PipelineLayoutDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout);

typedef struct ZHLN_StencilState {
    VkStencilOpState front;
    VkStencilOpState back;
} ZHLN_StencilState;

typedef struct ZHLN_GraphicsPipelineDesc {
    const ZHLN_ShaderStages* const ZHLN_RESTRICT stages;
    const VkPipelineLayout                       layout;
    const VkPipelineCache                        pipeline_cache;

    const bool                                                 descriptor_heap;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* const vs_mapping;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* const ps_mapping;

    const VkVertexInputBindingDescription* const ZHLN_RESTRICT   vertex_bindings;
    const VkVertexInputAttributeDescription* const ZHLN_RESTRICT vertex_attributes;
    const uint32_t                                               vertex_binding_count;
    const uint32_t                                               attribute_count;

    const VkFormat* const ZHLN_RESTRICT color_formats;
    const uint32_t                      color_format_count;
    const VkFormat                      depth_format;
    const VkPrimitiveTopology           topology;
    const VkPolygonMode                 polygon_mode;
    const VkCullModeFlags               cull_mode;
    const VkFrontFace                   front_face;

    const bool                  depth_test;
    const bool                  depth_write;
    const bool                  blend_enable;
    const bool                  additive_blend;
    const uint32_t              view_mask;
    const VkSpecializationInfo* specialization_info;

    const ZHLN_StencilState* const stencil;
    const bool                     color_write_enable;
} ZHLN_GraphicsPipelineDesc;

// Requires the enabled maintenance5 feature; SPIR-V is chained directly to
// each VkPipelineShaderStageCreateInfo during pipeline creation.
[[nodiscard]]
VkPipeline ZHLN_CreateGraphicsPipeline(VkDevice device, const ZHLN_GraphicsPipelineDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyPipeline(VkDevice device, VkPipeline pipeline);

void ZHLN_DestroyPipelineCache(VkDevice device, VkPipelineCache cache);

typedef struct ZHLN_RenderPassDesc {
    const VkImageView target_views[4];
    const uint32_t    target_count;
    const VkImageView depth_view;
    const VkImageView stencil_view;
    const VkExtent2D  extent;
    const float       clear_color[4];
    const float       clear_depth;
    const bool        use_secondaries;
} ZHLN_RenderPassDesc;

typedef struct ZHLN_ImageBarrierDesc {
    const VkImage               image;
    const VkAccessFlags2        src_access;
    const VkAccessFlags2        dst_access;
    const VkImageLayout         src_layout;
    const VkImageLayout         dst_layout;
    const VkPipelineStageFlags2 src_stage;
    const VkPipelineStageFlags2 dst_stage;
    const VkImageAspectFlags    aspect;
    const uint32_t              base_mip;
    const uint32_t              mip_count;
} ZHLN_ImageBarrierDesc;

void ZHLN_BeginRendering(VkCommandBuffer cmd, const ZHLN_RenderPassDesc* ZHLN_RESTRICT desc);
void ZHLN_EndRendering(VkCommandBuffer cmd);

typedef struct ZHLN_FrameSubmitDesc {
    const VkQueue         graphicsQueue;
    const VkQueue         presentQueue;
    const VkCommandBuffer cmd;
    const VkSemaphore     imageAvailable;
    const VkSemaphore     renderFinished;
    const VkFence         inFlight;
    const VkSwapchainKHR  swapchain;
    const uint32_t        imageIndex;
    const VkSemaphore     stagingSemaphore;
    const VkSemaphore     computeSemaphore;
    const uint64_t        stagingWaitValue;
    const uint64_t        computeWaitValue;
} ZHLN_FrameSubmitDesc;

[[nodiscard]]
VkResult ZHLN_SubmitAndPresent(const ZHLN_FrameSubmitDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_SecondaryCmdDesc {
    const VkFormat color_format;
    const VkFormat depth_format;
    // Match the primary's stencil attachment format, or VK_FORMAT_UNDEFINED
    // when the primary does not bind one (even if depth uses a combined format).
    const VkFormat stencil_format;
} ZHLN_SecondaryCmdDesc;

void     ZHLN_BeginSecondaryCommandBuffer(VkCommandBuffer cmd, const ZHLN_SecondaryCmdDesc* ZHLN_RESTRICT desc);
VkResult ZHLN_AllocateSecondaryCommandBuffers(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool, uint32_t count);

[[nodiscard]]
VkResult ZHLN_WaitAndResetFrame(VkDevice device, VkFence inFlightFence, const ZHLN_CommandPool* ZHLN_RESTRICT pool);

void ZHLN_BeginCommandBuffer(VkCommandBuffer cmd);
void ZHLN_EndCommandBuffer(VkCommandBuffer cmd);

[[nodiscard]]
VkResult ZHLN_WaitAndAcquireImage(
    VkDevice                              device,
    VkSwapchainKHR                        swapchain,
    const ZHLN_FrameSync* ZHLN_RESTRICT   sync,
    const ZHLN_CommandPool* ZHLN_RESTRICT pool,
    uint32_t*                             outImageIndex
);

void ZHLN_PushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, const void* ZHLN_RESTRICT data, uint32_t size);

#ifndef __cplusplus
#define ZHLN_Push(cmd, layout, stages, value) ZHLN_PushConstants(cmd, layout, stages, &(value), sizeof(value))
#endif

const char* ZHLN_VkResultString(VkResult result);

typedef struct ZHLN_BufferCopyDesc {
    const VkBuffer     src;
    const VkBuffer     dst;
    const VkDeviceSize size;
    const VkDeviceSize src_offset;
    const VkDeviceSize dst_offset;
} ZHLN_BufferCopyDesc;

void ZHLN_CmdCopyBuffer(VkCommandBuffer cmd, const ZHLN_BufferCopyDesc* ZHLN_RESTRICT desc);

void ZHLN_CmdPipelineBarrier(
    VkCommandBuffer                             cmd,
    uint32_t                                    memoryCount,
    const VkMemoryBarrier2* ZHLN_RESTRICT       memory,
    uint32_t                                    bufferCount,
    const VkBufferMemoryBarrier2* ZHLN_RESTRICT buffers,
    uint32_t                                    imageCount,
    const VkImageMemoryBarrier2* ZHLN_RESTRICT  images
);

void ZHLN_CmdImageBarrier(VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_BufferImageCopyDesc {
    const VkBuffer      buffer;
    const VkImage       image;
    const VkImageLayout layout;
    const uint32_t      width;
    const uint32_t      height;
    const VkDeviceSize  buffer_offset;
    const uint32_t      mip_level;
    const uint32_t      base_array_layer;
} ZHLN_BufferImageCopyDesc;

void ZHLN_CmdCopyBufferToImage(VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc* ZHLN_RESTRICT desc);

[[nodiscard]]
VkSemaphore ZHLN_CreateSemaphore(VkDevice device);
void        ZHLN_DestroySemaphore(VkDevice device, VkSemaphore semaphore);

typedef struct ZHLN_ImageViewDesc {
    const VkImage            image;
    const VkFormat           format;
    const VkImageAspectFlags aspect;
    const uint32_t           mip_levels;
    const uint32_t           array_layers;
    const VkImageViewType    view_type;
    const uint32_t           base_array_layer;
    const uint32_t           base_mip;
} ZHLN_ImageViewDesc;

[[nodiscard]]
VkResult ZHLN_CreateImageView(VkDevice device, const ZHLN_ImageViewDesc* ZHLN_RESTRICT desc, VkImageView* ZHLN_RESTRICT outView);

void ZHLN_DestroyImageView(VkDevice device, VkImageView view);

[[nodiscard]]
VkSampler ZHLN_CreateSampler(VkDevice device, const VkSamplerCreateInfo* desc);
void      ZHLN_DestroySampler(VkDevice device, VkSampler sampler);

typedef struct ZHLN_ComputePipelineDesc {
    const ZHLN_ShaderDesc       shader;
    const VkPipelineLayout      layout;
    const VkPipelineCache       pipeline_cache;
    const VkSpecializationInfo* specialization_info;

    const bool                                                 descriptor_heap;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* const cs_mapping;
} ZHLN_ComputePipelineDesc;

// Also requires maintenance5; shader.code must live until this call returns.
[[nodiscard]]
VkPipeline ZHLN_CreateComputePipeline(VkDevice device, const ZHLN_ComputePipelineDesc* ZHLN_RESTRICT desc);

void ZHLN_CmdDispatch(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

// Leaves every mip shader-readable; shader_read_stage must include the first consumer's stage.
void ZHLN_GenerateMipmaps(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height, uint32_t mip_levels, VkPipelineStageFlags2 shader_read_stage);

typedef struct ZHLN_MemoryBarrierDesc {
    const VkPipelineStageFlags2 src_stage;
    const VkAccessFlags2        src_access;
    const VkPipelineStageFlags2 dst_stage;
    const VkAccessFlags2        dst_access;
} ZHLN_MemoryBarrierDesc;

void ZHLN_CmdMemoryBarrier(VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc* ZHLN_RESTRICT desc);

VkDeviceAddress ZHLN_GetBufferDeviceAddress(VkDevice device, VkBuffer buffer);

typedef enum ZHLN_AccelerationStructureType : uint8_t { ZHLN_AS_TYPE_TOP_LEVEL = 0, ZHLN_AS_TYPE_BOTTOM_LEVEL = 1 } ZHLN_AccelerationStructureType;

typedef struct ZHLN_AccelerationStructureSizes {
    VkDeviceSize acceleration_structure_size;
    VkDeviceSize build_scratch_size;
    VkDeviceSize update_scratch_size;
} ZHLN_AccelerationStructureSizes;

typedef struct ZHLN_BlasGeometryDesc {
    VkDeviceAddress vertex_data;
    uint32_t        vertex_stride;
    uint32_t        max_vertex;
    VkFormat        vertex_format;
    VkDeviceAddress index_data;
    VkIndexType     index_type;
} ZHLN_BlasGeometryDesc;

typedef struct ZHLN_TlasGeometryDesc {
    VkDeviceAddress instance_data;
} ZHLN_TlasGeometryDesc;

void ZHLN_GetBlasSizes(
    VkDevice                                       device,
    const ZHLN_BlasGeometryDesc* ZHLN_RESTRICT     desc,
    uint32_t                                       primitiveCount,
    ZHLN_AccelerationStructureSizes* ZHLN_RESTRICT outSizes
);
void ZHLN_GetTlasSizes(VkDevice device, uint32_t instanceCount, ZHLN_AccelerationStructureSizes* ZHLN_RESTRICT outSizes);

[[nodiscard]]
VkAccelerationStructureKHR ZHLN_CreateAS(VkDevice device, VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type);
void                       ZHLN_DestroyAS(VkDevice device, VkAccelerationStructureKHR as);
[[nodiscard]]
VkDeviceAddress ZHLN_GetASAddress(VkDevice device, VkAccelerationStructureKHR as);

void ZHLN_CmdBuildBlas(
    VkCommandBuffer                            cmd,
    const ZHLN_BlasGeometryDesc* ZHLN_RESTRICT desc,
    VkAccelerationStructureKHR                 dstAs,
    VkDeviceAddress                            scratch,
    uint32_t                                   primitiveCount
);
void ZHLN_CmdBuildTlas(
    VkCommandBuffer                            cmd,
    const ZHLN_TlasGeometryDesc* ZHLN_RESTRICT desc,
    VkAccelerationStructureKHR                 dstAs,
    VkDeviceAddress                            scratch,
    uint32_t                                   instanceCount
);

#ifdef __cplusplus
}
#endif
