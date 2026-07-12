// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "demo_utils/DemoWindow.hpp"
#include "math.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>
#include <utility>

// ----------------------------------------------------------------------------
// Vulkan App
// ----------------------------------------------------------------------------
namespace {
std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    const size_t          file_size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));
    file.seekg(0);
    file.read(static_cast<char*>(static_cast<void*>(buffer.data())), static_cast<std::streamsize>(file_size));
    return buffer;
}

#ifdef __APPLE__
constexpr bool isMac = true;
#else
constexpr bool isMac = false;
#endif

} // namespace

// Define the Descriptor Layout using the TMP Builder
using CubeLayout = ZHLN::Vk::DescriptorLayout<
    ZHLN::Vk::SampledImageSlot<0>, // Texture
    ZHLN::Vk::SamplerSlot<1>       // Sampler
    >;

auto main() -> int {
    using namespace ZHLN;

    auto run_demo = [&]() -> std::expected<void, std::string> {
        // 1. OS Window Creation & RAII Scope Guard
        ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(800, 600, "ZHLN Engine - Cube");
        if (win.os_window == nullptr) {
            return std::unexpected("Failed to create OS window.");
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
                VkInstance instance = Vk::Context::Builder()
                    .AppName("ZHLN Engine - Cube")
                    .AppVersion(VK_MAKE_API_VERSION(0, 1, 0, 0))
                    .InstanceExtensions(instExts)
                    .EnableValidation(true)
                    .BuildInstance();

                if (instance == VK_NULL_HANDLE) {
                    return std::unexpected("Failed to create Vulkan Instance.");
                }

                // 4. Create Vulkan Surface (Raw Handle)
                VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(instance, win);
                if (raw_surface == VK_NULL_HANDLE) {
                    vkDestroyInstance(instance, nullptr);
                    return std::unexpected("Failed to create Vulkan Surface.");
                }

                // 5. Select Physical Device via Builder
                ZHLN_PhysicalDeviceInfo physical = Vk::Context::Builder()
                    .Instance(instance)
                    .Surface(raw_surface)
                    .SelectPhysicalDevice();

                if (physical.handle == VK_NULL_HANDLE) {
                    vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                    vkDestroyInstance(instance, nullptr);
                    return std::unexpected("Failed to select a suitable physical device.");
                }

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
                        if (!allocator.Init(ctx)) {
                            return std::unexpected("Failed to initialize Vulkan Memory Allocator.");
                        }

                        ZHLN::Vk::Swapchain     swapchain(ctx.Device(), {});
                        auto                    sync  = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
                        auto                    pools = ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});
                        ZHLN::Vk::SemaphorePool present_semaphores;

                        // =========================================================================
                        // Cube Texture Creation & Upload
                        // =========================================================================
                        const uint32_t    tex_w       = 256;
                        const uint32_t    tex_h       = 256;
                        const auto        cube_pixels = ZHLN::Texture::GenerateGrassTexture<tex_w, tex_h>();

                        VkImageCreateInfo tex_info = {
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
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        };

                        ZHLN::Vk::Image cube_texture_image = ZHLN::Vk::Image::Create(allocator.Get(), tex_info, VMA_MEMORY_USAGE_GPU_ONLY);
                        if (!cube_texture_image.Valid()) {
                            return std::unexpected("Failed to create cube texture image.");
                        }

                        ZHLN::Vk::CommandPool setup_pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
                        if (!setup_pool.Allocate(1)) {
                            return std::unexpected("Failed to allocate setup command pool.");
                        }

                        VkCommandBuffer setup_cmd = setup_pool[0];
                        ZHLN_BeginCommandBuffer(setup_cmd);

                        ZHLN::Vk::Buffer staging_buffer =
                            ZHLN::Vk::Buffer::Create(allocator.Get(), cube_pixels.size() * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
                        std::memcpy(staging_buffer.Map().data, cube_pixels.data(), cube_pixels.size() * sizeof(uint32_t));

                        ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(setup_cmd, cube_texture_image.Handle());

                        ZHLN_BufferImageCopyDesc copy_desc = {
                            .buffer = staging_buffer.Handle(), .image = cube_texture_image.Handle(), .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .width = tex_w, .height = tex_h
                        };
                        ZHLN::Vk::CopyBufferToImage(setup_cmd, copy_desc);

                        ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(setup_cmd, cube_texture_image.Handle());
                        ZHLN_EndCommandBuffer(setup_cmd);

                        VkCommandBufferSubmitInfo setup_cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = setup_cmd};
                        VkSubmitInfo2             setup_submit  = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &setup_cmd_info};
                        vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setup_submit, VK_NULL_HANDLE);
                        vkQueueWaitIdle(ctx.GraphicsQueue());

                        auto cube_texture_view = ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), cube_texture_image.Handle());

                        auto cube_sampler_res = ZHLN::Vk::SamplerBuilder {}.Linear().Repeat().Build(ctx.Device());
                        if (!cube_sampler_res) {
                            return std::unexpected("Failed to build cube sampler: " + std::string(cube_sampler_res.error().Message()));
                        }
                        auto cube_sampler = std::move(*cube_sampler_res);

                        auto cube_desc_layout = CubeLayout::CreateLayout(ctx.Device());
                        auto cube_desc_pool   = CubeLayout::CreatePool(ctx.Device(), 1);

                        VkDescriptorSet cube_descriptor_set = CubeLayout::Allocate(ctx.Device(), cube_desc_pool.Get(), cube_desc_layout.Get());
                        CubeLayout::Write(
                            ctx.Device(), cube_descriptor_set, ZHLN::Vk::ImageWrite {.view = cube_texture_view.Get(), .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            ZHLN::Vk::SamplerWrite {.sampler = cube_sampler.Get()}
                        );

                        // =========================================================================
                        // Monadic Pipeline Generation
                        // =========================================================================
                        return ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), "cube.hlsl.VSMain.spv", "cube.hlsl.PSMain.spv", "VSMain", "PSMain")
                            .transform_error([](ZHLN::Error err) {
                                return "Failed to compile cube ShaderStages: " + std::string(err.Message());
                            })
                            .and_then([&, cube_desc_layout = std::move(cube_desc_layout)](Vk::ShaderStages shaders) mutable {
                                VkDescriptorSetLayout raw_layout = cube_desc_layout.Get();
                                return Vk::PipelineLayoutBuilder(ctx.Device())
                                    .AddDescriptorSetLayout(raw_layout)
                                    .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT, sizeof(Mat4))
                                    .Build()
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to compile Pipeline Layout: {}", err.Message());
                                    })
                                    .transform([shaders = std::move(shaders)](Vk::PipelineLayout layout) mutable {
                                        return std::make_pair(std::move(shaders), std::move(layout));
                                    });
                            })
                            .and_then([&, ctx = std::move(ctx), cube_descriptor_set, cube_desc_pool = std::move(cube_desc_pool)](std::pair<Vk::ShaderStages, Vk::PipelineLayout> pair) mutable -> std::expected<void, std::string> {
                                auto& [shaders, layout] = pair;

                                return Vk::PipelineBuilder {}
                                    .Shaders(shaders)
                                    .Layout(layout.Get())
                                    .ColorFormats({VK_FORMAT_B8G8R8A8_SRGB})
                                    .DepthFormat(VK_FORMAT_D32_SFLOAT)
                                    .DepthTest(true)
                                    .DepthWrite(true)
                                    .CullBack()
                                    .Build(ctx.Device())
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build Graphics Pipeline: {}", err.Message());
                                    })
                                    .and_then([&, ctx = std::move(ctx), layout = std::move(layout), cube_descriptor_set](Vk::Pipeline&& pipeline) mutable -> std::expected<void, std::string> {
                                        
                                        ZHLN::Vk::Image     depth_image;
                                        ZHLN::Vk::ImageView depth_view;
                                        bool                depth_initialized = false;

                                        auto rebuild = [&]() -> bool {
                                            vkDeviceWaitIdle(ctx.Device());
                                            depth_initialized = false;

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
                                            if (!swapchain.Rebuild(s_desc)) {
                                                return false;
                                            }

                                            present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);

                                            VkImageCreateInfo img_info = {
                                                .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                                .imageType     = VK_IMAGE_TYPE_2D,
                                                .format        = VK_FORMAT_D32_SFLOAT,
                                                .extent        = {.width = win.width, .height = win.height, .depth = 1},
                                                .mipLevels     = 1,
                                                .arrayLayers   = 1,
                                                .samples       = VK_SAMPLE_COUNT_1_BIT,
                                                .tiling        = VK_IMAGE_TILING_OPTIMAL,
                                                .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                                .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
                                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                            };

                                            depth_image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
                                            depth_view  = ZHLN::Vk::CreateView<VK_FORMAT_D32_SFLOAT>(ctx.Device(), depth_image.Handle());
                                            return depth_view.Valid();
                                        };

                                        auto when            = std::chrono::high_resolution_clock::now();
                                        win.resized          = true;
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
                                                if (!rebuild()) {
                                                    break;
                                                }
                                                win.resized = false;
                                            }

                                            if (!swapchain.Valid() || swapchain.Get().extent.width == 0) {
                                                continue;
                                            }

                                            const auto  now     = std::chrono::high_resolution_clock::now();
                                            const float elapsed = std::chrono::duration<float>(now - when).count();

                                            const Mat4 model = Multiply(RotateY(elapsed * 0.75F), RotateX(elapsed * 0.45F));
                                            const Mat4 view  = LookAt({2.5F, 2.0F, 2.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
                                            const Mat4 proj  = Perspective(1.0472F, static_cast<float>(win.width) / static_cast<float>(win.height), 0.1F, 10.0F);
                                            const Mat4 mvp   = Multiply(proj, Multiply(view, model));

                                            auto record_cb = [&](VkCommandBuffer cmd, uint32_t imageIndex) {
                                                VkImage img = swapchain.Get().images[imageIndex];
                                                ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img);

                                                if (depth_view.Valid() && !depth_initialized) {
                                                    ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                                                        cmd, depth_image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                                                    );
                                                    depth_initialized = true;
                                                }

                                                ZHLN_RenderPassDesc pass = {
                                                    .target_views = {swapchain.Get().views[imageIndex]},
                                                    .depth_view   = depth_view.Get(),
                                                    .extent       = swapchain.Get().extent,
                                                    .clear_color  = {0.05F, 0.05F, 0.08F, 1.0F},
                                                    .clear_depth  = 1.0F,
                                                };

                                                {
                                                    ZHLN::Vk::ScopedRendering render(cmd, pass);
                                                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

                                                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.Get(), 0, 1, &cube_descriptor_set, 0, nullptr);

                                                    ZHLN::Vk::Push(cmd, layout.Get(), VK_SHADER_STAGE_VERTEX_BIT, mvp);
                                                    vkCmdDraw(cmd, static_cast<uint32_t>(cube_indices.size()), 1, 0, 0);
                                                }

                                                ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, img);
                                            };

                                            const ZHLN_FrameSync& frame_sync = sync[frame_index];
                                            ZHLN_CommandPool&     pool       = pools[frame_index];
                                            VkCommandBuffer       cmd        = pools.Cmd(frame_index);

                                            ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pool);

                                            uint32_t         image_index = 0;
                                            ZHLN_AcquireDesc acq        = {.swapchain = swapchain.Get().handle, .image_available = frame_sync.image_available, .timeout_ns = UINT64_MAX};
                                            if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
                                                win.resized = true;
                                                continue;
                                            }

                                            ZHLN_BeginCommandBuffer(cmd);
                                            record_cb(cmd, image_index);
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
