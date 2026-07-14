// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "demo_utils/DemoWindow.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>
#include <utility>
#include <cstdio>
#include <cstring>

// ----------------------------------------------------------------------------
// Vulkan App (Enclosed inside anonymous namespace for internal linkage)
// ----------------------------------------------------------------------------
namespace {
struct CRTPushConstants {
    float                time;
    float                scale;
    float                resX;
    float                resY;
    std::array<float, 4> background;
    std::array<float, 2> mousePos;
};

using CRTLayout = ZHLN::Vk::DescriptorLayout<
    ZHLN::Vk::SampledImageSlot<0>, // Texture
    ZHLN::Vk::SamplerSlot<1>       // Sampler
    >;

[[nodiscard]] std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    const auto file_size = static_cast<size_t>(file.tellg());
    if (file_size == 0 || file_size % sizeof(uint32_t) != 0) {
        return {}; // Corrupt or empty SPIR-V
    }

    // 1. Read directly into a char buffer (No casts required!)
    std::vector<char> byte_buffer(file_size);
    file.seekg(0);
    file.read(byte_buffer.data(), static_cast<std::streamsize>(file_size));

    // 2. Transfer bytes legally via memcpy
    std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));
    std::memcpy(buffer.data(), byte_buffer.data(), file_size);

    return buffer;
}

#ifdef __APPLE__
constexpr bool isMac = true;
#else
constexpr bool isMac = false;
#endif

} // namespace

auto main() -> int {
    using namespace ZHLN;

    auto run_demo = [&]() -> std::expected<void, std::string> {
        // 1. OS Window Creation & RAII Scope Guard
        ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(1024, 768, "ZHLN - CRT Shader");
        if (win.os_window == nullptr) {
            return std::unexpected("Failed to create OS window. Exiting.");
        }
        
        struct WindowGuard {
            Demo::WindowState& w;
            ~WindowGuard() { Demo::DestroyWindow(w); }
        } guard{win};

        // 2. Build Instance Extensions Monadically via Builder
        auto inst_exts_builder = Vk::ExtensionBuilder::ForInstance();
        for (const auto* ext : Demo::GetRequiredInstanceExtensions()) {
            inst_exts_builder.Require(ext);
        }
        inst_exts_builder.Require(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        inst_exts_builder.Require(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        bool has_maint1 = ZHLN::Vk::IsInstanceExtensionSupported(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        inst_exts_builder.OptionalIf(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, has_maint1);
        inst_exts_builder.OptionalIf(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, isMac);

        return inst_exts_builder.Build()
            .transform_error([](ZHLN::Error err) {
                return "Failed to build instance extensions: " + std::string(err.Message());
            })
            .and_then([&](const Vk::ExtensionResult& instExts) -> std::expected<void, std::string> {
                // 3. Create Vulkan Instance via Builder
                auto instance_res = Vk::Context::Builder()
                    .AppName("ZHLN - CRT Shader")
                    .AppVersion(VK_MAKE_API_VERSION(0, 1, 0, 0))
                    .InstanceExtensions(instExts)
                    .EnableValidation(true)
                    .BuildInstance();

                if (!instance_res) {
                    return std::unexpected("Failed to create Vulkan Instance: " + std::string(instance_res.error().Message()));
                }
                VkInstance instance = *instance_res;

                // 4. Create Vulkan Surface (Raw Handle)
                VkSurfaceKHR raw_surface = Demo::CreateSurface(instance, win);
                if (raw_surface == VK_NULL_HANDLE) {
                    vkDestroyInstance(instance, nullptr);
                    return std::unexpected("Failed to create Vulkan Surface.");
                }

                // 5. Select Physical Device via Builder
                auto physical_res = Vk::Context::Builder()
                    .Instance(instance)
                    .Surface(raw_surface)
                    .SelectPhysicalDevice();

                if (!physical_res) {
                    vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                    vkDestroyInstance(instance, nullptr);
                    return std::unexpected("Failed to select a suitable physical device: " + std::string(physical_res.error().Message()));
                }
                ZHLN_PhysicalDeviceInfo physical = *physical_res;

                std::span<const std::string_view> inst_exts_span = instExts;
                bool has_maint1_local = std::ranges::any_of(inst_exts_span, [](std::string_view name) {
                    return name == VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
                });

                // 6. Build Device Extensions Monadically via Builder
                return Vk::ExtensionBuilder::ForDevice(physical.handle)
                    .Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                    .RequireIf(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, has_maint1_local)
                    .RequireIf(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, has_maint1_local)
                    .OptionalIf("VK_KHR_portability_subset", isMac)
                    .Build()
                    .transform_error([instance, raw_surface](ZHLN::Error err) {
                        vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                        vkDestroyInstance(instance, nullptr);
                        return "Failed to build device extensions: " + std::string(err.Message());
                    })
                    .and_then([&, instance, raw_surface, physical, has_maint1_local](const Vk::ExtensionResult& devExts) -> std::expected<void, std::string> {
                        
                        // 7. Feature Chain Setup
                        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
                            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
                            .pNext = nullptr,
                            .swapchainMaintenance1 = VK_TRUE
                        };

                        auto features = Vk::FeatureChainBuilder(physical.handle)
                                            .template Require<VkPhysicalDeviceVulkan11Features>([](auto& f) {
                                                f.multiview                          = VK_TRUE;
                                                f.storageBuffer16BitAccess           = VK_TRUE;
                                                f.uniformAndStorageBuffer16BitAccess = VK_TRUE;
                                            })
                                            .template Require<VkPhysicalDeviceVulkan13Features>([has_maint1_local, &swap_maint](auto& f) {
                                                f.pNext = has_maint1_local ? &swap_maint : nullptr;
                                                f.synchronization2 = VK_TRUE;
                                                f.dynamicRendering = VK_TRUE;
                                            })
                                            .template Require<VkPhysicalDeviceVulkan12Features>([](auto& f) {
                                                f.bufferDeviceAddress = VK_TRUE;
                                            })
                                            .template Require<VkPhysicalDeviceFeatures2>([](auto& f) {
                                                f.features.samplerAnisotropy = VK_TRUE;
                                            })
                                            .Build();

                        ZHLN_DeviceDesc dev_desc = {
                            .extensions = devExts.data(),
                            .extension_count = static_cast<uint32_t>(devExts.size()),
                            .features = features.GetRoot(),
                            .enable_validation = true
                        };

                        // 8. Create Context via Builder
                        auto ctx_res = Vk::Context::Builder()
                            .Instance(instance)
                            .Surface(raw_surface)
                            .PhysicalDevice(physical)
                            .DeviceExtensions(devExts)
                            .DeviceFeatures(features.GetRoot())
                            .EnableValidation(true)
                            .Build();

                        if (!ctx_res) {
                            vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                            vkDestroyInstance(instance, nullptr);
                            return std::unexpected("Failed to create Vulkan logical device / Context: " + std::string(ctx_res.error().Message()));
                        }
                        auto ctx = std::move(ctx_res.value());

                        // 9. RAII Surface Holder
                        Vk::Surface surface(instance, raw_surface);

                        // 10. Allocator Setup
                        Vk::Allocator allocator;
                        auto alloc_res = allocator.Init(ctx);
                        if (!alloc_res) {
                            return std::unexpected("Failed to initialize Vulkan Memory Allocator: " + std::string(alloc_res.error().Message()));
                        }

                        ZHLN::Vk::Swapchain     swapchain(ctx.Device(), {});
                        auto                    sync  = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
                        auto                    pools = ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});
                        ZHLN::Vk::SemaphorePool present_semaphores;

                        // =========================================================================
                        // Texture Creation & Upload
                        // =========================================================================
                        const uint32_t tex_w  = 512;
                        const uint32_t tex_h  = 512;
                        const auto     pixels = ZHLN::Texture::GenerateTVInterrupt<tex_w, tex_h>();

                        VkImageCreateInfo img_info = {
                            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType     = VK_IMAGE_TYPE_2D,
                            .format        = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent        = {.width = tex_w, .height = tex_h, .depth = 1},
                            .mipLevels     = 1,
                            .arrayLayers   = 1,
                            .samples       = VK_SAMPLE_COUNT_1_BIT,
                            .tiling        = VK_IMAGE_TILING_OPTIMAL,
                            .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
                        };

                        auto texture_image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
                        if (!texture_image.Valid()) {
                            return std::unexpected("Failed to create texture image.");
                        }

                        ZHLN::Vk::CommandPool setup_pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
                        if (!setup_pool.Allocate(1)) {
                            return std::unexpected("Failed to allocate setup command pool.");
                        }
                        VkCommandBuffer setup_cmd = setup_pool[0];

                        ZHLN_BeginCommandBuffer(setup_cmd);
                        auto staging_buffer = ZHLN::Vk::Buffer::Create(allocator.Get(), pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
                        std::memcpy(staging_buffer.Map().data, pixels.data(), pixels.size() * 4);

                        ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(setup_cmd, texture_image.Handle());

                        ZHLN_BufferImageCopyDesc copy_desc = {
                            .buffer = staging_buffer.Handle(), .image = texture_image.Handle(), .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .width = tex_w, .height = tex_h
                        };
                        ZHLN::Vk::CopyBufferToImage(setup_cmd, copy_desc);

                        ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(setup_cmd, texture_image.Handle());
                        ZHLN_EndCommandBuffer(setup_cmd);

                        VkCommandBufferSubmitInfo setup_cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = setup_cmd};
                        VkSubmitInfo2             setup_submit  = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &setup_cmd_info};
                        vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setup_submit, VK_NULL_HANDLE);
                        vkQueueWaitIdle(ctx.GraphicsQueue());

                        // View & Sampler RAII Helpers
                        auto texture_view = ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), texture_image.Handle());

                        auto sampler_res = ZHLN::Vk::SamplerBuilder {}.Linear().Repeat().Build(ctx.Device());
                        if (!sampler_res) {
                            return std::unexpected("Failed to build sampler: " + std::string(sampler_res.error().Message()));
                        }
                        auto sampler = std::move(*sampler_res);

                        auto desc_layout = CRTLayout::CreateLayout(ctx.Device());
                        auto desc_pool   = CRTLayout::CreatePool(ctx.Device(), 1);

                        VkDescriptorSet descriptor_set = CRTLayout::Allocate(ctx.Device(), desc_pool.Get(), desc_layout.Get());
                        CRTLayout::Write(
                            ctx.Device(), descriptor_set, ZHLN::Vk::ImageWrite {.view = texture_view.Get(), .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            ZHLN::Vk::SamplerWrite {.sampler = sampler.Get()}
                        );

                        // =========================================================================
                        // Monadic Pipeline Generation
                        // =========================================================================
                        return ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), "crt.hlsl.VSMain.spv", "crt.hlsl.PSMain.spv", "VSMain", "PSMain")
                            .transform_error([](ZHLN::Error err) {
                                return "Failed to compile CRT ShaderStages: " + std::string(err.Message());
                            })
                            .and_then([&, desc_layout = std::move(desc_layout)](Vk::ShaderStages shaders) mutable {
                                VkDescriptorSetLayout raw_layout = desc_layout.Get();
                                return Vk::PipelineLayoutBuilder(ctx.Device())
                                    .AddDescriptorSetLayout(raw_layout)
                                    .AddPushConstant(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(CRTPushConstants))
                                    .Build()
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to compile Pipeline Layout: {}", err.Message());
                                    })
                                    .transform([shaders = std::move(shaders)](Vk::PipelineLayout layout) mutable {
                                        return std::make_pair(std::move(shaders), std::move(layout));
                                    });
                            })
                            .and_then([&, ctx = std::move(ctx), descriptor_set, desc_pool = std::move(desc_pool)](std::pair<Vk::ShaderStages, Vk::PipelineLayout> pair) mutable -> std::expected<void, std::string> {
                                auto& [shaders, layout] = pair;

                                return Vk::PipelineBuilder {}
                                    .Shaders(shaders)
                                    .Layout(layout.Get())
                                    .ColorFormats({VK_FORMAT_B8G8R8_SRGB})
                                    .NoDepth() // <-- Fullscreen quad, no depth required
                                    .CullNone()
                                    .Build(ctx.Device())
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build Graphics Pipeline: {}", err.Message());
                                    })
                                    .and_then([&, ctx = std::move(ctx), layout = std::move(layout), descriptor_set](auto&& pipeline) mutable -> std::expected<void, std::string> {
                                        
                                        auto start_time       = std::chrono::high_resolution_clock::now();
                                        win.resized           = true;
                                        uint32_t frame_index = 0;

                                        // ---------------------------------------------------------
                                        // Main Loop
                                        // ---------------------------------------------------------
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
                                            float time = std::chrono::duration<float>(now - start_time).count();

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
                                                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

                                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.Get(), 0, 1, &descriptor_set, 0, nullptr);

                                                float mx = win.width > 0 ? (win.mouse_x / (float) win.width) : 0.5F;
                                                float my = win.height > 0 ? (win.mouse_y / (float) win.height) : 0.5F;

                                                CRTPushConstants pc = {
                                                    .time       = time,
                                                    .scale      = 1.0F,
                                                    .resX      = (float) win.width,
                                                    .resY      = (float) win.height,
                                                    .background = {0, 0, 0, 1},
                                                    .mousePos   = {mx, my},
                                                };
                                                ZHLN::Vk::Push(cmd, layout.Get(), VK_SHADER_STAGE_FRAGMENT_BIT, pc);

                                                vkCmdDraw(cmd, 3, 1, 0, 0); // Draw fullscreen triangle
                                            }

                                            ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, img);
                                            ZHLN_EndCommandBuffer(cmd);

                                            ZHLN_FrameSubmitDesc submit_desc = {
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

                                            if (ZHLN::Vk::SubmitAndPresent(submit_desc) != ZHLN_FrameResult_Ok) {
                                                win.resized = true;
                                            }

                                            frame_index = (frame_index + 1) % 3;
                                        }

                                        vkDeviceWaitIdle(ctx.Device());
                                        return {};
                                    });
                            });
                    });
            });
    };

    auto result = run_demo();
    if (!result) {
        std::println(stderr, "Error during execution: {}", result.error());
        return -1;
    }

    return 0;
}
