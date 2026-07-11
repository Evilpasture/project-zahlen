// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "demo_utils/DemoWindow.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>

// ----------------------------------------------------------------------------
// Vulkan App (Enclosed inside anonymous namespace for internal linkage)
// ----------------------------------------------------------------------------
namespace {
struct CRTPushConstants {
    float                time;
    float                scale;
    float                res_x;
    float                res_y;
    std::array<float, 4> background;
    std::array<float, 2> mousePos;
};

using CRTLayout = ZHLN::Vk::DescriptorLayout<
    ZHLN::Vk::SampledImageSlot<0>, // Texture
    ZHLN::Vk::SamplerSlot<1>       // Sampler
    >;

[[nodiscard]] auto LoadSpirv(const std::filesystem::path& path) -> std::vector<uint32_t> {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize == 0 || fileSize % sizeof(uint32_t) != 0) {
        return {}; // Corrupt or empty SPIR-V
    }

    // 1. Read directly into a char buffer (No casts required!)
    std::vector<char> byteBuffer(fileSize);
    file.seekg(0);
    file.read(byteBuffer.data(), static_cast<std::streamsize>(fileSize));

    // 2. Transfer bytes legally via memcpy
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    std::memcpy(buffer.data(), byteBuffer.data(), fileSize);

    return buffer;
}
} // namespace

int main() {
    // 1. OS Window Creation
    ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(1024, 768, "ZHLN - CRT Shader");
    if (win.os_window == nullptr) {
        std::println(stderr, "Failed to create OS window. Exiting.");
        return -1;
    }

    // 2. Context Setup
    ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
    auto              inst_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
    inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

    bool has_maint1 = ZHLN::Vk::IsInstanceExtensionSupported(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (has_maint1) {
        inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }

#ifdef __APPLE__
    inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
    inst_desc.extensions      = inst_exts.data();
    inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, .swapchainMaintenance1 = VK_TRUE
    };

    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = has_maint1 ? &swap_maint : nullptr,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE
    };

    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &feat13, .bufferDeviceAddress = VK_TRUE
    };

    VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat12};

    std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (has_maint1) {
        dev_exts.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        dev_exts.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    }
#ifdef __APPLE__
    dev_exts.push_back("VK_KHR_portability_subset");
#endif

    ZHLN_DeviceDesc dev_desc = {
        .extensions = dev_exts.data(), .extension_count = static_cast<uint32_t>(dev_exts.size()), .features = &feat2, .enable_validation = true
    };

    auto ctx = ZHLN::Vk::Context::Create(
        inst_desc, ZHLN_DeviceSelectDesc {.instance = VK_NULL_HANDLE, .surface = VK_NULL_HANDLE, .score_fn = nullptr, .score_userdata = nullptr}, dev_desc
    );

    if (!ctx) {
        std::println(stderr, "FATAL: Failed to create Vulkan Context.");
        return -1;
    }

    // 3. Surface & Allocator
    VkSurfaceKHR      raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
    ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

    ZHLN::Vk::Allocator allocator;
    if (!allocator.Init(ctx)) {
        return -1;
    }

    ZHLN::Vk::Swapchain     swapchain(ctx.Device(), {});
    auto                    sync  = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
    auto                    pools = ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});
    ZHLN::Vk::SemaphorePool present_semaphores;

    // =========================================================================
    // 4. Texture Creation & Upload
    // =========================================================================
    const uint32_t TEX_W  = 512;
    const uint32_t TEX_H  = 512;
    static auto    pixels = ZHLN::Texture::GenerateTVInterrupt<TEX_W, TEX_H>();

    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = {.width = TEX_W, .height = TEX_H, .depth = 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    auto textureImage = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
    if (!textureImage.Valid()) {
        return -1;
    }

    ZHLN::Vk::CommandPool setupPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
    if (!setupPool.Allocate(1)) {
        return -1;
    }
    VkCommandBuffer setupCmd = setupPool[0];

    ZHLN_BeginCommandBuffer(setupCmd);
    auto stagingBuffer = ZHLN::Vk::Buffer::Create(allocator.Get(), pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    memcpy(stagingBuffer.Map().data, pixels.data(), pixels.size() * 4);

    ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(setupCmd, textureImage.Handle());

    ZHLN_BufferImageCopyDesc copyDesc = {
        .buffer = stagingBuffer.Handle(), .image = textureImage.Handle(), .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .width = TEX_W, .height = TEX_H
    };
    ZHLN::Vk::CopyBufferToImage(setupCmd, copyDesc);

    ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(setupCmd, textureImage.Handle());
    ZHLN_EndCommandBuffer(setupCmd);

    VkCommandBufferSubmitInfo setupCmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = setupCmd};
    VkSubmitInfo2             setupSubmit  = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &setupCmdInfo};
    vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setupSubmit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.GraphicsQueue());

    // View & Sampler RAII Helpers
    auto textureView = ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), textureImage.Handle());

    auto sampler_res = ZHLN::Vk::SamplerBuilder {}.Linear().Repeat().Build(ctx.Device());
    if (!sampler_res) {
        std::println(stderr, "FATAL: Failed to build sampler: {}", sampler_res.error());
        return -1;
    }
    auto sampler = std::move(*sampler_res);

    // =========================================================================
    // 5. Descriptor Sets
    // =========================================================================
    auto descLayout = CRTLayout::CreateLayout(ctx.Device());
    auto descPool   = CRTLayout::CreatePool(ctx.Device(), 1);

    VkDescriptorSet descriptorSet = CRTLayout::Allocate(ctx.Device(), descPool.Get(), descLayout.Get());
    CRTLayout::Write(
        ctx.Device(), descriptorSet, ZHLN::Vk::ImageWrite {.view = textureView.Get(), .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        ZHLN::Vk::SamplerWrite {.sampler = sampler.Get()}
    );

    // =========================================================================
    // 6. Pipeline
    // =========================================================================
    auto shaders = ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), "crt.hlsl.VSMain.spv", "crt.hlsl.PSMain.spv", "VSMain", "PSMain");
    if (!shaders.Valid()) {
        std::println(stderr, "FATAL: Failed to create CRT shader stages.");
        return -1;
    }

    VkPushConstantRange   push_range = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(CRTPushConstants)};
    VkDescriptorSetLayout rawLayout  = descLayout.Get();

    ZHLN_PipelineLayoutDesc  pLayoutDesc = {.set_layouts = &rawLayout, .set_layout_count = 1, .push_constants = &push_range, .push_constant_count = 1};
    ZHLN::Vk::PipelineLayout pipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &pLayoutDesc));

    auto pipelineRes = ZHLN::Vk::PipelineBuilder {}
                           .Shaders(shaders)
                           .Layout(pipelineLayout.Get())
                           .ColorFormats({VK_FORMAT_B8G8R8A8_SRGB})
                           .NoDepth() // <-- Fullscreen quad, no depth required
                           .CullNone()
                           .Build(ctx.Device());

    if (!pipelineRes) {
        std::println(stderr, "FATAL: Failed to create CRT graphics pipeline.");
        return -1;
    }

    // =========================================================================
    // 7. Render Loop
    // =========================================================================
    uint32_t frame_index = 0;
    win.resized          = true;
    auto startTime       = std::chrono::high_resolution_clock::now();

    while (win.running) {
        ZHLN::Demo::ProcessEvents(win);

        if (win.width == 0 || win.height == 0) {
            continue;
        }

        if (win.resized) {
            vkDeviceWaitIdle(ctx.Device());
            ZHLN_Device raw_dev = {
                .handle = ctx.Device(), .graphics_queue = ctx.GraphicsQueue(), .present_queue = ctx.PresentQueue(), .transfer_queue = ctx.TransferQueue()
            };
            ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();
            ZHLN_SwapchainDesc      s_desc   = {
                .device        = &raw_dev,
                .physical      = &raw_phys,
                .surface       = surface.Get(),
                .width         = win.width,
                .height        = win.height,
                .vsync         = true,
                .old_swapchain = swapchain.Get().handle
            };
            swapchain.Rebuild(s_desc);
            present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);
            win.resized = false;
        }

        if (!swapchain.Valid() || swapchain.Get().extent.width == 0) {
            continue;
        }

        auto  now  = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float>(now - startTime).count();

        const ZHLN_FrameSync& frame_sync = sync[frame_index];
        ZHLN_CommandPool&     pool       = pools[frame_index];
        VkCommandBuffer       cmd        = pools.Cmd(frame_index);

        ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pool);

        uint32_t         image_index = 0;
        ZHLN_AcquireDesc acq         = {.swapchain = swapchain.Get().handle, .image_available = frame_sync.image_available, .timeout_ns = UINT64_MAX};
        if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
            win.resized = true;
            continue;
        }

        ZHLN_BeginCommandBuffer(cmd);
        VkImage img = swapchain.Get().images[image_index];
        ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img);

        ZHLN_RenderPassDesc pass = {
            .target_views = {swapchain.Get().views[image_index]}, .depth_view = VK_NULL_HANDLE, .extent = swapchain.Get().extent, .clear_color = {0, 0, 0, 1}
        };
        {
            ZHLN::Vk::ScopedRendering render(cmd, pass);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineRes->Get());

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0, 1, &descriptorSet, 0, nullptr);

            float mx = win.width > 0 ? (win.mouse_x / (float) win.width) : 0.5f;
            float my = win.height > 0 ? (win.mouse_y / (float) win.height) : 0.5f;

            CRTPushConstants pc = {
                .time       = time,
                .scale      = 1.0f,
                .res_x      = (float) win.width,
                .res_y      = (float) win.height,
                .background = {0, 0, 0, 1},
                .mousePos   = {mx, my},
            };
            ZHLN::Vk::Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_FRAGMENT_BIT, pc);

            vkCmdDraw(cmd, 3, 1, 0, 0); // Draw fullscreen triangle
        }

        ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, img);
        ZHLN_EndCommandBuffer(cmd);

        ZHLN_FrameSubmitDesc submitDesc = {
            .graphicsQueue    = ctx.GraphicsQueue(),
            .presentQueue     = ctx.PresentQueue(),
            .cmd              = cmd,
            .imageAvailable   = frame_sync.image_available,
            .renderFinished   = present_semaphores[image_index],
            .inFlight         = frame_sync.in_flight,
            .swapchain        = swapchain.Get().handle,
            .imageIndex       = image_index,
            .stagingSemaphore = VK_NULL_HANDLE,
            .stagingWaitValue = 0
        };

        if (ZHLN::Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok) {
            win.resized = true;
        }

        frame_index = (frame_index + 1) % 3;
    }

    vkDeviceWaitIdle(ctx.Device());
    ZHLN::Demo::DestroyWindow(win);
    return 0;
}
