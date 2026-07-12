// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "demo_utils/DemoWindow.hpp"
#include <print>
#include <utility>

auto main() -> int {
    using namespace ZHLN;
#ifdef __APPLE__
    static constexpr bool isMac = true;
#else
    static constexpr bool is_mac = false;
#endif

    auto run_demo = [&]() -> std::expected<void, std::string> {
        // 1. OS Window Creation & RAII Scope Guard
        auto win = Demo::InitWindow(1280, 720, "ZHLN - Triangle Demo");
        if (!win.running) {
            return std::unexpected("Failed to initialize OS window.");
        }

        struct WindowGuard {
            Demo::WindowState& w;
            ~WindowGuard() {
                Demo::DestroyWindow(w);
            }
        } guard {win};

        // 2. Build Instance Extensions Monadically
        auto inst_exts_builder = Vk::ExtensionBuilder::ForInstance();
        for (const auto* ext: Demo::GetRequiredInstanceExtensions()) {
            inst_exts_builder.Require(ext);
        }
        inst_exts_builder.OptionalIf(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, is_mac);

        return inst_exts_builder.Build().transform_error(
                                            [](ZHLN::Error err) { return "Failed to build instance extensions: " + std::string(err.Message()); }
        ).and_then([&](auto&& instExts) -> std::expected<void, std::string> {
            // 3. Create Vulkan Instance
            VkInstance instance = Vk::CreateInstance("ZHLN - Triangle Demo", VK_MAKE_API_VERSION(0, 1, 0, 0), instExts, true);
            if (instance == VK_NULL_HANDLE) {
                return std::unexpected("Failed to create Vulkan Instance.");
            }

            // 4. Create Vulkan Surface (Raw Handle)
            VkSurfaceKHR raw_surface = Demo::CreateSurface(instance, win);
            if (raw_surface == VK_NULL_HANDLE) {
                vkDestroyInstance(instance, nullptr);
                return std::unexpected("Failed to create Vulkan Surface.");
            }

            // 5. Select Physical Device
            ZHLN_PhysicalDeviceInfo physical = Vk::SelectDevice(instance, raw_surface);
            if (physical.handle == VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                vkDestroyInstance(instance, nullptr);
                return std::unexpected("Failed to select a suitable physical device.");
            }

            // 6. Build Device Extensions Monadically
            return Vk::ExtensionBuilder::ForDevice(physical.handle)
                .Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                .OptionalIf("VK_KHR_portability_subset", is_mac)
                .Build()
                .transform_error([instance, raw_surface](ZHLN::Error err) {
                    vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                    vkDestroyInstance(instance, nullptr);
                    return "Failed to build device extensions: " + std::string(err.Message());
                })
                .and_then([&, instance, raw_surface, physical](auto&& devExts) -> std::expected<void, std::string> {
                    // 7. Feature Chain Setup
                    auto features = Vk::FeatureChainBuilder(VK_NULL_HANDLE)
                                        .Require<VkPhysicalDeviceVulkan13Features>([](auto& f) {
                                            f.synchronization2 = VK_TRUE;
                                            f.dynamicRendering = VK_TRUE;
                                        })
                                        .Build();

                    ZHLN_DeviceDesc dev_desc = {
                        .extensions        = devExts.data(),
                        .extension_count   = static_cast<uint32_t>(devExts.size()),
                        .features          = features.GetRoot(),
                        .enable_validation = true
                    };

                    // 8. Create Logical Device / Context
                    auto ctx = Vk::Context::Create(instance, raw_surface, physical, dev_desc);
                    if (!ctx) {
                        vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                        vkDestroyInstance(instance, nullptr);
                        return std::unexpected("Failed to create Vulkan logical device.");
                    }

                    // 9. RAII Surface Holder
                    // Declared after ctx so that surface is destroyed before ctx (preventing use-after-free on instance destruction)
                    Vk::Surface surface(instance, raw_surface);

                    // 10. Allocator Setup
                    Vk::Allocator allocator;
                    if (!allocator.Init(ctx)) {
                        return std::unexpected("Failed to initialize Vulkan Memory Allocator.");
                    }

                    // 11. Presentation Setup
                    Vk::PresentationContext pres;
                    if (!pres.Init(ctx, allocator, surface.Get(), win.width, win.height, true)) {
                        return std::unexpected("Failed to initialize Presentation Context.");
                    }

                    auto sync  = Vk::FrameSync<3>::Create(ctx.Device());
                    auto pools = Vk::CommandPools<3>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});

                    // 12. Shader & Pipeline Compilation
                    return Vk::ShaderStages::FromFiles(ctx.Device(), "triangle.hlsl.VSMain.spv", "triangle.hlsl.PSMain.spv", "VSMain", "PSMain")
                        .transform_error([](ZHLN::Error err) { return "Shader compilation failed: " + std::string(err.Message()); })
                        .and_then([&](auto&& shaders) {
                            return Vk::PipelineLayoutBuilder(ctx.Device())
                                .Build()
                                .transform_error([](ZHLN::Error err) { return std::format("Pipeline layout compilation failed: {}", err.Message()); })
                                .transform([shaders = std::forward<decltype(shaders)>(shaders)](auto&& layout) mutable {
                                    return std::make_pair(std::move(shaders), std::forward<decltype(layout)>(layout));
                                });
                        })
                        .and_then([&, ctx = std::move(ctx)](auto&& pair) mutable -> std::expected<void, std::string> {
                            auto& [shaders, layout] = pair;

                            return Vk::PipelineBuilder {}
                                .Shaders(shaders)
                                .Layout(layout.Get())
                                .ColorFormats({pres.swapchain.Get().format})
                                .NoDepth()
                                .CullNone()
                                .Build(ctx.Device())
                                .transform_error([](ZHLN::Error err) {
                                    return std::format("Graphics pipeline compilation failed. Error Code: {}", err.Message());
                                })
                                .and_then([&](auto&& pipeline) -> std::expected<void, std::string> {
                                    // 13. Rebuild Callback
                                    auto rebuild = [&]() {
                                        auto _      = pres.Rebuild(win.width, win.height);
                                        win.resized = false;
                                    };

                                    uint32_t frame_index = 0;
                                    win.resized          = true;

                                    // 14. Concise Render Loop
                                    while (win.running) {
                                        Demo::ProcessEvents(win);
                                        if (win.width == 0 || win.height == 0) {
                                            continue;
                                        }
                                        if (win.resized) {
                                            rebuild();
                                        }
                                        if (!pres.swapchain.Valid() || pres.swapchain.Get().extent.width == 0) {
                                            continue;
                                        }

                                        Vk::DrawFrameDesc<3> frame_desc = {
                                            .ctx = ctx, .swapchain = pres.swapchain, .sync = sync, .pools = pools, .presentSemaphores = pres.presentSemaphores
                                        };

                                        Vk::DrawFrame(
                                            frame_desc, frame_index,
                                            [&](VkCommandBuffer cmd, uint32_t imageIndex) {
                                                Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_image = {
                                                    .handle = pres.swapchain.Get().images[imageIndex],
                                                    .view   = pres.swapchain.Get().views[imageIndex],
                                                    .extent = pres.swapchain.Get().extent,
                                                    .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .format = pres.swapchain.Get().format
                                                };

                                                auto swap_att = Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, swap_image);

                                                Vk::DynamicPass(swap_att.extent)
                                                    .AddColor(
                                                        swap_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                                        {.r = 0.01F, .g = 0.01F, .b = 0.02F, .a = 1.0F}
                                                    )
                                                    .Execute(cmd, [&]() {
                                                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
                                                        vkCmdDraw(cmd, 3, 1, 0, 0);
                                                    });

                                                auto _ = Vk::Transition<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swap_att);
                                            },
                                            rebuild
                                        );
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
