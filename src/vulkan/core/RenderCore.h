/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file RenderCore.h
 * @brief Project-Zahlen's Zero-overHead vuLkan abstractioN layer (ZHLN): a C23
 *        interface to Vulkan instance management and hardware selection.
 */

#pragma once
#include <stdbool.h>
// Volk owns the Vulkan headers from here on: it defines VK_NO_PROTOTYPES and includes
// <vulkan/vulkan.h> itself, so every vk* name below refers to Volk's dispatch pointers,
// not the loader's prototypes. Include volk.h before any direct <vulkan/*.h>.
#include <volk.h>

#ifndef ZHLN_RESTRICT
#define ZHLN_RESTRICT __restrict
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- INSTANCE MANAGEMENT */

static constexpr auto maxInstanceExtensions = 128;

typedef void (*ZHLN_DebugHookFn)(void* userdata, VkDebugUtilsMessageSeverityFlagBitsEXT severity);

/*
 * Diagnostics forwarding. The C layer is stateless by design (see RENDER.md) and owns no
 * counters: the C++ Vk::Instance owns one of these, and the debug-messenger pUserData
 * carries it back to the C callback, which forwards error severities to the hook.
 */
typedef struct ZHLN_DebugForwarding {
    ZHLN_DebugHookFn hook;     /* NULL: no counting, logging only */
    void*            userdata; /* the Vk::Instance that owns the counters */
} ZHLN_DebugForwarding;

typedef enum ZHLN_ValidationMode : uint8_t { ZHLN_VALIDATION_OFF = 0, ZHLN_VALIDATION_ON = 1, ZHLN_VALIDATION_GPU = 2 } ZHLN_ValidationMode;

/* Configuration for Vulkan instance initialization. */
typedef struct ZHLN_InstanceDesc {
    char                                      app_name[64];
    const uint32_t                            version;
    uint32_t                                  extension_count;
    const VkDebugUtilsMessageSeverityFlagsEXT severity_flags;
    const char* const*                        extensions;
    const ZHLN_ValidationMode                 validation_mode;
    /* Diagnostics owner (C++ side); may be NULL when no counting is wanted.
       Used as pUserData by both the pNext and the persistent messenger. */
    ZHLN_DebugForwarding* debug;
} ZHLN_InstanceDesc;

/* Default instance configuration. */
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

/*
 * Acquires the Vulkan loader through Volk, which loads it at runtime: until this
 * succeeds every global-level vk* pointer is NULL. ZHLN_CreateInstance calls it, as does
 * every helper that may touch Vulkan before an instance exists; a second call is a no-op
 * returning VK_SUCCESS.
 */
[[nodiscard]]
VkResult ZHLN_EnsureVulkanLoader(void);

/* Creates a Vulkan instance with a debug messenger chained into pNext; VK_NULL_HANDLE on
 * critical failure. */
[[nodiscard]]
VkInstance ZHLN_CreateInstance(const ZHLN_InstanceDesc* ZHLN_RESTRICT desc);

/* --- DEVICE SELECTION */

/* Snapshot of a physical device's capabilities. */
typedef struct ZHLN_PhysicalDeviceInfo {
    VkPhysicalDevice                  handle;
    VkPhysicalDeviceProperties2       properties;
    VkPhysicalDeviceFeatures2         features;
    VkPhysicalDeviceMemoryProperties2 memory;
    uint32_t                          graphics_family;
    uint32_t                          present_family;
    uint32_t                          transfer_family; /**< dedicated transfer, when there is one */
    uint32_t                          compute_family;
    bool                              has_graphics;
    bool                              has_present; /**< presentation supported on the queried surface */
    bool                              has_transfer;
    bool                              has_compute;
} ZHLN_PhysicalDeviceInfo;

/* Ranks hardware candidates: >0 is preferred, <0 rejects the device entirely. */
typedef int32_t (*ZHLN_DeviceScoreFn)(const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT info, const void* const ZHLN_RESTRICT userdata);

/* Constraints and logic for selecting the optimal GPU. */
typedef struct ZHLN_DeviceSelectDesc {
    const VkInstance         instance;       /**< Required: Active Vulkan instance */
    const VkSurfaceKHR       surface;        /**< Optional: Surface for present support checks */
    const ZHLN_DeviceScoreFn score_fn;       /**< Optional: Scoring logic (null = discrete GPU preference) */
    const void*              score_userdata; /**< Context passed to the scoring function */
} ZHLN_DeviceSelectDesc;

/* Queries all GPUs and selects the best candidate; check .handle == VK_NULL_HANDLE for
 * failure. */
[[nodiscard]]
ZHLN_PhysicalDeviceInfo ZHLN_SelectPhysicalDevice(const ZHLN_DeviceSelectDesc* ZHLN_RESTRICT desc);

/* --- DEVICE CREATION */

typedef struct ZHLN_DeviceDesc {
    const ZHLN_PhysicalDeviceInfo* const ZHLN_RESTRICT physical;
    const char* const* const                           extensions;
    const uint32_t                                     extension_count;
    const VkPhysicalDeviceFeatures2*                   features; // nullptr = nothing extra requested
    const bool                                         enable_validation;
} ZHLN_DeviceDesc;

typedef struct ZHLN_Device {
    VkDevice handle;
    VkQueue  graphics_queue;
    VkQueue  present_queue;
    VkQueue  transfer_queue; /**< dedicated async transfer queue */
    VkQueue  compute_queue;

    // --- VK_EXT_descriptor_heap (Volk globals after volkLoadDevice; NULL when unsupported)
    PFN_vkCmdBindResourceHeapEXT      pfn_cmd_bind_resource_heap;
    PFN_vkCmdBindSamplerHeapEXT       pfn_cmd_bind_sampler_heap;
    PFN_vkCmdPushDataEXT              pfn_cmd_push_data;
    PFN_vkWriteResourceDescriptorsEXT pfn_write_resource_descriptors;
    PFN_vkWriteSamplerDescriptorsEXT  pfn_write_sampler_descriptors;
    bool                              descriptor_heap_enabled;

    // --- VK_EXT_mesh_shader (Volk globals after volkLoadDevice; NULL when absent)
    PFN_vkCmdDrawMeshTasksEXT              pfn_cmd_draw_mesh_tasks;
    PFN_vkCmdDrawMeshTasksIndirectEXT      pfn_cmd_draw_mesh_tasks_indirect;
    PFN_vkCmdDrawMeshTasksIndirectCountEXT pfn_cmd_draw_mesh_tasks_indirect_count;
    bool                                   mesh_shader_enabled;
} ZHLN_Device;

/* --- VK_EXT_mesh_shader hardware limits */

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

/* --- DEBUG MESSENGER & ERROR FORWARDING */

/*
 * Creates the persistent debug messenger. REQUIRED for runtime messages: the create-info
 * chained into VkInstanceCreateInfo only covers instance creation/destruction, so without
 * this the engine's debug callback never runs. The hook/userdata pair receives
 * error-severity notifications; counting stays the C++ owner's business.
 */
[[nodiscard]]
VkDebugUtilsMessengerEXT ZHLN_CreateDebugMessenger(VkInstance instance, VkDebugUtilsMessageSeverityFlagsEXT severity, ZHLN_DebugForwarding* debug);

void ZHLN_DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger);

/* Queries VkPhysicalDeviceMeshShaderPropertiesEXT; `supported` is false when the device
 * does not advertise VK_EXT_mesh_shader, in which case every limit reads back as zero. */
[[nodiscard]]
ZHLN_MeshShaderLimits ZHLN_QueryMeshShaderLimits(VkPhysicalDevice physical);

/* True when the device satisfies the limits the Zahlen task/mesh shaders were written
 * against (see resources/shaders/basic_mesh.slang). */
[[nodiscard]]
bool ZHLN_MeshShaderLimitsSufficient(const ZHLN_MeshShaderLimits* ZHLN_RESTRICT limits);

[[nodiscard]]
ZHLN_Device ZHLN_CreateDevice(const ZHLN_DeviceDesc* ZHLN_RESTRICT desc);

/* --- DESCRIPTOR HEAPS (VK_EXT_descriptor_heap)
 *
 * The engine binds one resource heap and one sampler heap per command buffer
 * segment instead of descriptor sets. Heaps are plain device-addressable
 * buffers; descriptors are produced on the host with the vkWrite*DescriptorsEXT
 * commands and the application copies them into the heap memory.
 *
 * State-model note: recording any heap or push-data command invalidates all
 * legacy descriptor-set state and vkCmdPushConstants in that command buffer
 * (and vice versa), so heap-using passes must (re)bind the heaps before use
 * and push all per-draw data through vkCmdPushDataEXT.
 *
 * After volkLoadDevice, the Volk vk* globals are snapshotted onto
 * ZHLN_Device; ZHLN::Vk::Context forwards to them (see Context.hpp).
 */

/* --- SWAPCHAIN */

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
    const VkSwapchainKHR                               old_swapchain; // VK_NULL_HANDLE on first create
} ZHLN_SwapchainDesc;

typedef struct ZHLN_Swapchain {
    VkSwapchainKHR handle;
    VkImage        images[8];
    VkImageView    views[8];
    uint32_t       image_count;
    VkFormat       format;
    VkExtent2D     extent;
} ZHLN_Swapchain;

[[nodiscard]]
ZHLN_SwapchainSupport ZHLN_QuerySwapchainSupport(const ZHLN_SwapchainSupportDesc* ZHLN_RESTRICT desc);

[[nodiscard]]
ZHLN_Swapchain ZHLN_CreateSwapchain(const ZHLN_SwapchainDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroySwapchain(VkDevice device, ZHLN_Swapchain* ZHLN_RESTRICT swapchain);

/* --- SYNC PRIMITIVES */

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

// out_sync must point to an array of at least desc->frame_count
[[nodiscard]]
bool ZHLN_CreateFrameSync(const ZHLN_FrameSyncDesc* desc, ZHLN_FrameSync* ZHLN_RESTRICT outSync);

void ZHLN_DestroyFrameSync(VkDevice device, ZHLN_FrameSync* ZHLN_RESTRICT sync, uint32_t frameCount);

/* --- COMMAND POOL AND BUFFERS */

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

/* --- FRAME LOOP STRUCTURE */

/*
 * The frame verbs below return the Vulkan call's own VkResult, unmapped:
 * VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR and VK_ERROR_DEVICE_LOST keep their own
 * meaning instead of being collapsed into a generic error, and VK_SUCCESS being 0 matches
 * the engine's "0 means no error" convention, so the C layer needs no vocabulary of its
 * own.
 */

typedef struct ZHLN_AcquireDesc {
    const VkSwapchainKHR swapchain;
    const VkSemaphore    image_available;
    const uint64_t       timeout_ns; // UINT64_MAX = wait forever
} ZHLN_AcquireDesc;

typedef struct ZHLN_PresentDesc {
    const VkQueue        present_queue;
    const VkSwapchainKHR swapchain;
    const VkSemaphore    render_finished;
    const uint32_t       image_index;
} ZHLN_PresentDesc;

void ZHLN_WaitAndResetFence(VkDevice device, VkFence fence);

/* No image is vended on failure; whatever vkAcquireNextImageKHR returned is
 * what comes back. */
[[nodiscard]]
VkResult ZHLN_AcquireImage(VkDevice device, const ZHLN_AcquireDesc* ZHLN_RESTRICT desc, uint32_t* outImageIndex);

/** One vkQueueSubmit2. Counts may be zero; pointers are unused then. */
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

/* --- SHADER MANAGEMENT */

typedef struct ZHLN_ShaderDesc {
    const uint32_t*              code; /**< SPIR-V bytecode */
    const size_t                 size; /**< size in bytes */
    [[maybe_unused]] const char* entry_point; /**< Optional: if NULL spirv_reflect reads the module
                                             entry point; only when that fails is a
                                             stage-conventional name (VSMain/PSMain/CSMain) used */
} ZHLN_ShaderDesc;

typedef struct ZHLN_Shader {
    VkShaderModule        handle;
    VkShaderStageFlagBits stage;
    char                  entry_point[64];
    uint32_t              view_mask;
} ZHLN_Shader;

typedef struct ZHLN_ShaderStages {
    // VK_EXT_mesh_shader: when `mesh.handle` is non-null the pipeline is a mesh
    // pipeline. `task` is optional (a mesh shader may be dispatched directly),
    // `vert` is then unused but deliberately kept so a single ZHLN_ShaderStages
    // can carry both the legacy and the mesh path for the same material.
    ZHLN_Shader task; // VK_SHADER_STAGE_TASK_BIT_EXT
    ZHLN_Shader mesh; // VK_SHADER_STAGE_MESH_BIT_EXT
    ZHLN_Shader vert;
    ZHLN_Shader frag;
} ZHLN_ShaderStages;

typedef struct ZHLN_ShaderStagesDesc {
    const VkDevice        device;
    const ZHLN_ShaderDesc vert;
    const ZHLN_ShaderDesc frag;
    const ZHLN_ShaderDesc task; // optional (VK_EXT_mesh_shader)
    const ZHLN_ShaderDesc mesh; // optional (VK_EXT_mesh_shader)
} ZHLN_ShaderStagesDesc;

[[nodiscard]]
uint32_t ZHLN_DetectShaderViewMask(const ZHLN_ShaderDesc* ZHLN_RESTRICT desc);

[[nodiscard]]
VkShaderModule ZHLN_CreateShaderModule(VkDevice device, const ZHLN_ShaderDesc* ZHLN_RESTRICT desc);

// Convenience: creates both stages and returns them paired, destroys both on any failure
[[nodiscard]]
bool ZHLN_CreateShaderStages(const ZHLN_ShaderStagesDesc* ZHLN_RESTRICT desc, ZHLN_ShaderStages* ZHLN_RESTRICT out);

void ZHLN_DestroyShaderModule(VkDevice device, VkShaderModule module);
void ZHLN_DestroyShaderStages(VkDevice device, ZHLN_ShaderStages* ZHLN_RESTRICT stages);

// Populates the VkPipelineShaderStageCreateInfo entries the pipeline builder needs;
// out_stages must point to an array of ZHLN_MAX_SHADER_STAGES. In heap mode each stage's
// pNext receives its VkShaderDescriptorSetAndBindingMappingInfoEXT, remapping legacy
// set/binding decorations onto the bound heaps.
//
// VK_EXT_mesh_shader: when the mesh stage is present, task+mesh replace the vertex stage
// entirely (a pipeline may not contain both), and they receive `vs_mapping` so the `scene`
// parameter block resolves as it does for vertex/fragment.
static constexpr auto ZHLN_MAX_SHADER_STAGES = 3;

// The color attachments a graphics pipeline may declare: the blend state is a fixed array
// in ZHLN_CreateGraphicsPipeline, so a descriptor asking for more is rejected there rather
// than quietly blended by fewer states than it declared.
static constexpr auto ZHLN_MAX_COLOR_ATTACHMENTS = 8;

[[nodiscard]] uint32_t ZHLN_PopulateShaderStageInfos(
    const ZHLN_ShaderStages* ZHLN_RESTRICT               stages,
    VkPipelineShaderStageCreateInfo* ZHLN_RESTRICT       outStages,
    const VkSpecializationInfo*                          specInfo,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* vsMapping,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* psMapping
);

/* --- PIPELINE LAYOUT */

typedef struct ZHLN_PipelineLayoutDesc {
    const VkDescriptorSetLayout* const ZHLN_RESTRICT set_layouts;
    const uint32_t                                   set_layout_count;
    const VkPushConstantRange* const ZHLN_RESTRICT   push_constants;
    const uint32_t                                   push_constant_count;
} ZHLN_PipelineLayoutDesc;

[[nodiscard]]
VkPipelineLayout ZHLN_CreatePipelineLayout(VkDevice device, const ZHLN_PipelineLayoutDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout);

/* --- GRAPHICS PIPELINE */

// The stencil state of both faces. The presence of this struct in a descriptor *is* the
// enable, because Vulkan ignores `front`/`back` while `stencilTestEnable` is false: a
// separate flag could claim a test the pipeline then silently does not have.
typedef struct ZHLN_StencilState {
    VkStencilOpState front;
    VkStencilOpState back;
} ZHLN_StencilState;

typedef struct ZHLN_GraphicsPipelineDesc {
    const ZHLN_ShaderStages* const ZHLN_RESTRICT stages;
    const VkPipelineLayout                       layout;
    // Optional driver-side pipeline cache; VK_NULL_HANDLE compiles without recording it.
    const VkPipelineCache pipeline_cache;

    // --- VK_EXT_descriptor_heap (binding-interface mapping)
    // When descriptor_heap is true the pipeline is created with
    // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT, `layout` must be VK_NULL_HANDLE
    // (VUID-VkGraphicsPipelineCreateInfo-flags-11311), each stage maps its legacy
    // set/binding decorations onto heap offsets through its mapping chain, and push
    // constants are replaced by vkCmdPushDataEXT.
    const bool                                                 descriptor_heap;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* const vs_mapping;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* const ps_mapping;
    // VK_EXT_mesh_shader: task and mesh consume the same `scene` block as the vertex
    // stage, so they reuse `vs_mapping` (chained into their pNext by
    // ZHLN_PopulateShaderStageInfos).

    const VkVertexInputBindingDescription* const ZHLN_RESTRICT   vertex_bindings;
    const VkVertexInputAttributeDescription* const ZHLN_RESTRICT vertex_attributes;
    const uint32_t                                               vertex_binding_count;
    const uint32_t                                               attribute_count;

    const VkFormat* const ZHLN_RESTRICT color_formats;
    const uint32_t                      color_format_count;
    const VkFormat                      depth_format; // VK_FORMAT_UNDEFINED = no depth
    const VkPrimitiveTopology           topology;     // default: VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    const VkPolygonMode                 polygon_mode; // default: VK_POLYGON_MODE_FILL
    const VkCullModeFlags               cull_mode;    // default: VK_CULL_MODE_BACK_BIT
    const VkFrontFace                   front_face;   // default: VK_FRONT_FACE_COUNTER_CLOCKWISE

    const bool                  depth_test;
    const bool                  depth_write;
    const bool                  blend_enable;   // basic src_alpha / one_minus_src_alpha if true
    const bool                  additive_blend; // Explicitly route additive blend configuration
    const uint32_t              view_mask;      // Explicit Multiview mask (0 = disabled)
    const VkSpecializationInfo* specialization_info;

    // --- CSG Extensions
    // NULL = no stencil test; non-NULL = the test is on with both faces carrying the state
    // it names. The depth format must then have a stencil aspect: a state installed over a
    // stencil-less attachment is a creation failure, not a pipeline that draws without it.
    const ZHLN_StencilState* const stencil;
    const bool                     color_write_enable; // false writes masks to stencil only
} ZHLN_GraphicsPipelineDesc;

[[nodiscard]]
VkPipeline ZHLN_CreateGraphicsPipeline(VkDevice device, const ZHLN_GraphicsPipelineDesc* ZHLN_RESTRICT desc);

void ZHLN_DestroyPipeline(VkDevice device, VkPipeline pipeline);

// Destroys a pipeline cache. Safe to call with VK_NULL_HANDLE.
void ZHLN_DestroyPipelineCache(VkDevice device, VkPipelineCache cache);

/* --- RENDERING */

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
    const VkImageAspectFlags    aspect; // e.g. VK_IMAGE_ASPECT_COLOR_BIT
    const uint32_t              base_mip;
    const uint32_t              mip_count; //  (use VK_REMAINING_MIP_LEVELS for all)
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
    const VkSemaphore     stagingSemaphore; /**< timeline semaphore, transfer queue */
    const VkSemaphore     computeSemaphore; /**< timeline semaphore, compute queue */
    const uint64_t        stagingWaitValue; /**< timeline value to wait on */
    const uint64_t        computeWaitValue;
} ZHLN_FrameSubmitDesc;

[[nodiscard]]
VkResult ZHLN_SubmitAndPresent(const ZHLN_FrameSubmitDesc* ZHLN_RESTRICT desc);

/* --- FRAME HELPERS */

typedef struct ZHLN_SecondaryCmdDesc {
    const VkFormat color_format;
    const VkFormat depth_format;
} ZHLN_SecondaryCmdDesc;

void     ZHLN_BeginSecondaryCommandBuffer(VkCommandBuffer cmd, const ZHLN_SecondaryCmdDesc* ZHLN_RESTRICT desc);
VkResult ZHLN_AllocateSecondaryCommandBuffers(VkDevice device, ZHLN_CommandPool* ZHLN_RESTRICT pool, uint32_t count);

[[nodiscard]]
VkResult ZHLN_WaitAndResetFrame(VkDevice device, VkFence inFlightFence, const ZHLN_CommandPool* ZHLN_RESTRICT pool);

// Wraps vkBeginCommandBuffer with one-time-submit flag for frame recording
void ZHLN_BeginCommandBuffer(VkCommandBuffer cmd);
void ZHLN_EndCommandBuffer(VkCommandBuffer cmd);

/* --- FRAME LOOP COHESION */

/* Waits for the in-flight fence, resets it, and acquires the next swapchain image. */
[[nodiscard]]
VkResult ZHLN_WaitAndAcquireImage(
    VkDevice                              device,
    VkSwapchainKHR                        swapchain,
    const ZHLN_FrameSync* ZHLN_RESTRICT   sync,
    const ZHLN_CommandPool* ZHLN_RESTRICT pool,
    uint32_t*                             outImageIndex
);

/* --- PUSH CONSTANT HELPERS */

void ZHLN_PushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, const void* ZHLN_RESTRICT data, uint32_t size);

// Typed convenience macro so C doesn't spell out sizeof every time
#ifndef __cplusplus
#define ZHLN_Push(cmd, layout, stages, value) ZHLN_PushConstants(cmd, layout, stages, &(value), sizeof(value))
#endif

/* --- ERROR HELPERS */

const char* ZHLN_VkResultString(VkResult result);

/* --- EXECUTION HELPERS */

typedef struct ZHLN_BufferCopyDesc {
    const VkBuffer     src;
    const VkBuffer     dst;
    const VkDeviceSize size;
    const VkDeviceSize src_offset;
    const VkDeviceSize dst_offset;
} ZHLN_BufferCopyDesc;

/* Buffer-to-buffer copy. */
void ZHLN_CmdCopyBuffer(VkCommandBuffer cmd, const ZHLN_BufferCopyDesc* ZHLN_RESTRICT desc);

/* One vkCmdPipelineBarrier2. Counts may be zero; the pointers are unused then. */
void ZHLN_CmdPipelineBarrier(
    VkCommandBuffer                             cmd,
    uint32_t                                    memoryCount,
    const VkMemoryBarrier2* ZHLN_RESTRICT       memory,
    uint32_t                                    bufferCount,
    const VkBufferMemoryBarrier2* ZHLN_RESTRICT buffers,
    uint32_t                                    imageCount,
    const VkImageMemoryBarrier2* ZHLN_RESTRICT  images
);

/* One image pipeline barrier (synchronization2). */
void ZHLN_CmdImageBarrier(VkCommandBuffer cmd, const ZHLN_ImageBarrierDesc* ZHLN_RESTRICT desc);

typedef struct ZHLN_BufferImageCopyDesc {
    const VkBuffer      buffer;
    const VkImage       image;
    const VkImageLayout layout;
    const uint32_t      width;
    const uint32_t      height;
    const VkDeviceSize  buffer_offset;    // 0 for tightly packed
    const uint32_t      mip_level;        // 0 for base
    const uint32_t      base_array_layer; // 0 for non-array
} ZHLN_BufferImageCopyDesc;

/* Copies buffer data into an image (e.g. texture upload). */
void ZHLN_CmdCopyBufferToImage(VkCommandBuffer cmd, const ZHLN_BufferImageCopyDesc* ZHLN_RESTRICT desc);

/* --- SEMAPHORE HELPERS */

[[nodiscard]]
VkSemaphore ZHLN_CreateSemaphore(VkDevice device);
void        ZHLN_DestroySemaphore(VkDevice device, VkSemaphore semaphore);

/* --- IMAGE VIEW HELPERS */

typedef struct ZHLN_ImageViewDesc {
    const VkImage            image;
    const VkFormat           format;
    const VkImageAspectFlags aspect;
    const uint32_t           mip_levels;   /* default 1 */
    const uint32_t           array_layers; /* default 1 */
    const VkImageViewType    view_type;
    const uint32_t           base_array_layer;
    const uint32_t           base_mip; /* targeted base mip level (default 0) */
} ZHLN_ImageViewDesc;

[[nodiscard]]
VkResult ZHLN_CreateImageView(VkDevice device, const ZHLN_ImageViewDesc* ZHLN_RESTRICT desc, VkImageView* ZHLN_RESTRICT outView);

void ZHLN_DestroyImageView(VkDevice device, VkImageView view);

/* --- SAMPLER HELPERS */
[[nodiscard]]
VkSampler ZHLN_CreateSampler(VkDevice device, const VkSamplerCreateInfo* desc);
void      ZHLN_DestroySampler(VkDevice device, VkSampler sampler);

/* --- COMPUTE PIPELINE */

typedef struct ZHLN_ComputePipelineDesc {
    const ZHLN_ShaderDesc  shader;
    const VkPipelineLayout layout;
    // Optional driver-side pipeline cache; see ZHLN_GraphicsPipelineDesc.
    const VkPipelineCache       pipeline_cache;
    const VkSpecializationInfo* specialization_info;

    // VK_EXT_descriptor_heap binding-interface mapping (see ZHLN_GraphicsPipelineDesc).
    const bool                                                 descriptor_heap;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* const cs_mapping;
} ZHLN_ComputePipelineDesc;

[[nodiscard]]
VkPipeline ZHLN_CreateComputePipeline(VkDevice device, const ZHLN_ComputePipelineDesc* ZHLN_RESTRICT desc);

void ZHLN_CmdDispatch(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

/* --- MESH SHADING (VK_EXT_mesh_shader)
 *
 * After volkLoadDevice the Volk vkCmdDrawMeshTasks* globals are snapshotted onto
 * ZHLN_Device. These wrappers are no-ops when the extension is unavailable, so callers
 * only check ZHLN_Device::mesh_shader_enabled when choosing a pipeline, never around the
 * draw itself.
 */

void ZHLN_CmdDrawMeshTasks(const ZHLN_Device* ZHLN_RESTRICT device, VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

void ZHLN_CmdDrawMeshTasksIndirect(
    const ZHLN_Device* ZHLN_RESTRICT device,
    VkCommandBuffer                  cmd,
    VkBuffer                         buffer,
    VkDeviceSize                     offset,
    uint32_t                         drawCount,
    uint32_t                         stride
);

void ZHLN_CmdDrawMeshTasksIndirectCount(
    const ZHLN_Device* ZHLN_RESTRICT device,
    VkCommandBuffer                  cmd,
    VkBuffer                         buffer,
    VkDeviceSize                     offset,
    VkBuffer                         countBuffer,
    VkDeviceSize                     countBufferOffset,
    uint32_t                         maxDrawCount,
    uint32_t                         stride
);

/* --- MIPMAPPING */

/**
 * @brief Generates mipmaps for a color image using linear blits.
 * Transitions image to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
 */
void ZHLN_GenerateMipmaps(VkCommandBuffer cmd, VkImage image, int32_t width, int32_t height, uint32_t mip_levels);

/* --- MEMORY BARRIERS */

typedef struct ZHLN_MemoryBarrierDesc {
    const VkPipelineStageFlags2 src_stage;
    const VkAccessFlags2        src_access;
    const VkPipelineStageFlags2 dst_stage;
    const VkAccessFlags2        dst_access;
} ZHLN_MemoryBarrierDesc;

void ZHLN_CmdMemoryBarrier(VkCommandBuffer cmd, const ZHLN_MemoryBarrierDesc* ZHLN_RESTRICT desc);

/* --- HARDWARE RAY TRACING */

VkDeviceAddress ZHLN_GetBufferDeviceAddress(VkDevice device, VkBuffer buffer);

typedef struct ZHLN_RayTracingContext {
    VkDevice                                       device;
    PFN_vkGetAccelerationStructureBuildSizesKHR    get_build_sizes;
    PFN_vkCreateAccelerationStructureKHR           create_as;
    PFN_vkCmdBuildAccelerationStructuresKHR        build_as;
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_address;
    PFN_vkDestroyAccelerationStructureKHR          destroy_as;
} ZHLN_RayTracingContext;

[[nodiscard]]
bool ZHLN_InitRayTracingContext(VkDevice device, ZHLN_RayTracingContext* ZHLN_RESTRICT outCtx);

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
    const ZHLN_RayTracingContext* ZHLN_RESTRICT    ctx,
    const ZHLN_BlasGeometryDesc* ZHLN_RESTRICT     desc,
    uint32_t                                       primitiveCount,
    ZHLN_AccelerationStructureSizes* ZHLN_RESTRICT outSizes
);
void ZHLN_GetTlasSizes(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, uint32_t instanceCount, ZHLN_AccelerationStructureSizes* ZHLN_RESTRICT outSizes);

[[nodiscard]]
VkAccelerationStructureKHR
     ZHLN_CreateAS(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type);
void ZHLN_DestroyAS(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, VkAccelerationStructureKHR as);
[[nodiscard]]
VkDeviceAddress ZHLN_GetASAddress(const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx, VkAccelerationStructureKHR as);

void ZHLN_CmdBuildBlas(
    const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx,
    VkCommandBuffer                             cmd,
    const ZHLN_BlasGeometryDesc* ZHLN_RESTRICT  desc,
    VkAccelerationStructureKHR                  dstAs,
    VkDeviceAddress                             scratch,
    uint32_t                                    primitiveCount
);
void ZHLN_CmdBuildTlas(
    const ZHLN_RayTracingContext* ZHLN_RESTRICT ctx,
    VkCommandBuffer                             cmd,
    const ZHLN_TlasGeometryDesc* ZHLN_RESTRICT  desc,
    VkAccelerationStructureKHR                  dstAs,
    VkDeviceAddress                             scratch,
    uint32_t                                    instanceCount
);

#ifdef __cplusplus
}
#endif
