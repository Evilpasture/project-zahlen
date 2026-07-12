// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "../cube_app/math.hpp"
#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"
#include "SamplerBuilder.hpp"
#include "SponzaSpecifics.hpp"
#include "Texture.hpp"
#include "Vertex.hpp"
#include "demo_utils/DemoWindow.hpp"
// clang-format off
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
// clang-format on
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>
#include <utility>
#include <tuple>

ZHLN_REFLECT_VERTEX(ZHLN::Vk::Vertex, pos, norm, tangent, uv0, uv1);

// ============================================================================
// Types & RAII Wrappers (Enclosed inside anonymous namespace for internal linkage)
// ============================================================================
namespace {
struct GltfData {
    cgltf_data* ptr = nullptr;
    ~GltfData() {
        if (ptr != nullptr) {
            cgltf_free(ptr);
        }
    }

    GltfData()                                   = default;
    GltfData(const GltfData&)                    = delete;
    auto operator=(const GltfData&) -> GltfData& = delete;
    GltfData(GltfData&& other) noexcept: ptr(std::exchange(other.ptr, nullptr)) {
    }
    auto operator=(GltfData&& other) noexcept -> GltfData& {
        if (this != &other) {
            if (ptr != nullptr) {
                cgltf_free(ptr);
            }
            ptr = std::exchange(other.ptr, nullptr);
        }
        return *this;
    }

    [[nodiscard]] static GltfData Load(const std::string& path) {
        GltfData      d;
        cgltf_options opts {};
        if (cgltf_parse_file(&opts, path.c_str(), &d.ptr) != cgltf_result_success || cgltf_load_buffers(&opts, d.ptr, path.c_str()) != cgltf_result_success) {
            if (d.ptr != nullptr) {
                cgltf_free(d.ptr);
                d.ptr = nullptr;
            }
        }
        return d;
    }

    [[nodiscard]] bool Valid() const {
        return ptr != nullptr;
    }
    cgltf_data* operator->() const {
        return ptr;
    }
};

struct MeshPrimitive {
    uint32_t indexCount;
    uint32_t firstIndex;
    int      materialIndex;
};

struct DrawCall {
    MeshPrimitive* mesh;
    Mat4           worldMatrix;
};

struct MaterialCreativeWork {
    uint32_t albedoIdx;
    uint32_t normalIdx;
    uint32_t pbrIdx;
    uint32_t lightmapIdx;
    uint32_t emissiveIdx;
};

struct AppFrameData {
    ZHLN::Vk::Passes::ShadowConfig  shadowData;
    ZHLN::Vk::Passes::PBRMainConfig pbrData;
    ZHLN::Vk::Passes::FXAAConfig    fxaaData;
};

// Bindless Scene Layout
using SceneLayout = ZHLN::Vk::DescriptorLayout<
    ZHLN::Vk::BindlessSampledImageSlot<0, 4096>,
    ZHLN::Vk::SamplerSlot<1>,
    ZHLN::Vk::SampledImageSlot<2>,
    ZHLN::Vk::SamplerSlot<3>,
    ZHLN::Vk::SamplerSlot<4>,
    ZHLN::Vk::StorageBufferSlot<5, VK_SHADER_STAGE_FRAGMENT_BIT>>;

using FXAALayout = ZHLN::Vk::DescriptorLayout<ZHLN::Vk::SampledImageSlot<0>, ZHLN::Vk::SamplerSlot<1>>;

// ============================================================================
// Helpers & Path Resolution (Removed redundant static definitions inside anonymous namespace)
// ============================================================================

std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::streamsize       size = file.tellg();
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

bool FileExists(const std::string& path) {
    bool exists = std::filesystem::exists(path);
    if (!exists) {
        std::println(stderr, "ERROR: File not found: {}", std::filesystem::absolute(path).string());
    }
    return exists;
}

struct CreativeWorkPaths {
    std::string assetPrefix;
    std::string shaderPrefix;
    std::string gltf;
    std::string vertSpv;
    std::string fragSpv;
    std::string shadowVertSpv;
    std::string shadowFragSpv;

    [[nodiscard]] bool Validate() const {
        return FileExists(gltf) && FileExists(vertSpv) && FileExists(fragSpv) && FileExists(shadowVertSpv) && FileExists(shadowFragSpv);
    }
};

CreativeWorkPaths ResolvePaths() {
    CreativeWorkPaths p;
    p.assetPrefix = "resources/assets/main_sponza/";
#ifdef SHADER_DIR
    p.shaderPrefix = SHADER_DIR;
#else
    p.shaderPrefix = "./";
#endif

    if (std::filesystem::exists("build/src/render/sponza.hlsl.VSMain.spv")) {
        p.shaderPrefix = "build/src/render/";
    } else if (std::filesystem::exists("../../../resources")) {
        p.assetPrefix  = "../../../resources/assets/main_sponza/";
        p.shaderPrefix = "./";
    } else {
        p.shaderPrefix = "./";
    }

    p.gltf          = p.assetPrefix + "NewSponza_Main_glTF_003.gltf";
    p.vertSpv       = p.shaderPrefix + "sponza.hlsl.VSMain.spv";
    p.fragSpv       = p.shaderPrefix + "sponza.hlsl.PSMain.spv";
    p.shadowVertSpv = p.shaderPrefix + "shadow.hlsl.VSMain.spv";
    p.shadowFragSpv = p.shaderPrefix + "shadow.hlsl.PSMain.spv";
    return p;
}

// ============================================================================
// Scene loading
// ============================================================================

struct Scene {
    std::vector<ZHLN::Vk::Vertex> vertices;
    std::vector<uint32_t>         indices;
    std::vector<MeshPrimitive>    primitives;
    std::vector<DrawCall>         drawCalls;
};

Scene BuildScene(cgltf_data* data) {
    Scene                            scene;
    std::vector<std::vector<size_t>> mesh_prim_indices(data->meshes_count);

    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        for (cgltf_size j = 0; j < data->meshes[i].primitives_count; ++j) {
            cgltf_primitive* prim = &data->meshes[i].primitives[j];

            auto first_index   = static_cast<uint32_t>(scene.indices.size());
            auto vertex_offset = static_cast<uint32_t>(scene.vertices.size());

            for (cgltf_size k = 0; k < prim->indices->count; ++k) {
                scene.indices.push_back(cgltf_accessor_read_index(prim->indices, k) + vertex_offset);
            }

            size_t vertex_count = prim->attributes[0].data->count;
            size_t start_vert   = scene.vertices.size();
            scene.vertices.resize(scene.vertices.size() + vertex_count);

            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                cgltf_attribute* attr = &prim->attributes[a];
                for (cgltf_size v = 0; v < vertex_count; ++v) {
                    if (attr->type == cgltf_attribute_type_position) {
                        cgltf_accessor_read_float(attr->data, v, scene.vertices[start_vert + v].pos.data(), 3);
                    } else if (attr->type == cgltf_attribute_type_normal) {
                        cgltf_accessor_read_float(attr->data, v, scene.vertices[start_vert + v].norm.data(), 3);
                    } else if (attr->type == cgltf_attribute_type_tangent) {
                        cgltf_accessor_read_float(attr->data, v, scene.vertices[start_vert + v].tangent.data(), 4);
                    } else if (attr->type == cgltf_attribute_type_texcoord) {
                        if (attr->index == 0) { // UV0
                            cgltf_accessor_read_float(attr->data, v, scene.vertices[start_vert + v].uv0.data(), 2);
                        } else if (attr->index == 1) { // UV1
                            cgltf_accessor_read_float(attr->data, v, scene.vertices[start_vert + v].uv1.data(), 2);
                        }
                    }
                }
            }

            mesh_prim_indices[i].push_back(scene.primitives.size());
            scene.primitives.push_back({
                .indexCount    = static_cast<uint32_t>(prim->indices->count),
                .firstIndex    = first_index,
                .materialIndex = (prim->material != nullptr) ? static_cast<int>(prim->material - data->materials) : -1,
            });
        }
    }

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        cgltf_node* node = &data->nodes[i];
        if (node->mesh == nullptr) {
            continue;
        }

        std::array<float, 16> matrix {};
        cgltf_node_transform_world(node, matrix.data());
        Mat4 world_mat {};
        std::ranges::copy(matrix, world_mat.data.begin());

        size_t mesh_idx = node->mesh - data->meshes;
        for (size_t prim_idx: mesh_prim_indices[mesh_idx]) {
            scene.drawCalls.push_back({.mesh = &scene.primitives[prim_idx], .worldMatrix = world_mat});
        }
    }

    return scene;
}

template <VkFormat F>
ZHLN::Vk::TextureCreativeWork UploadTextureFromFile(ZHLN::Vk::Allocator& allocator, const ZHLN::Vk::Context& ctx, const VkImageCreateInfo& baseInfo, const std::string& fullPath) {
    int tw = 0;
    int th = 0;
    int tc = 0;
    stbi_uc* pixels = stbi_load(fullPath.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
    if (!pixels) {
        std::println(stderr, "Failed to load: {}", fullPath);
        return {};
    }

    VkImageCreateInfo info = baseInfo;
    info.extent            = {.width = static_cast<uint32_t>(tw), .height = static_cast<uint32_t>(th), .depth = 1};

    ZHLN::Vk::TextureCreativeWork result = ZHLN::Vk::UploadTexture<F>(allocator, ctx, info, pixels);
    stbi_image_free(pixels);
    return result;
}

struct FpsCounter {
    std::chrono::high_resolution_clock::time_point lastUpdate = std::chrono::high_resolution_clock::now();
    uint32_t                                       frames     = 0;

    std::string Tick(size_t drawCallCount) {
        ++frames;
        auto  now     = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastUpdate).count();
        if (elapsed < 1.0F) {
            return {};
        }

        float fps  = static_cast<float>(frames) / elapsed;
        frames     = 0;
        lastUpdate = now;
        return std::format("ZHLN Engine | FPS: {:.1F} ({:.2F} ms) | Draw calls: {}", fps, 1000.0F / fps, drawCallCount);
    }
};

#ifdef __APPLE__
constexpr bool isMac = true;
#else
constexpr bool isMac = false;
#endif

} // namespace

// ============================================================================
// main
// ============================================================================

auto main() -> int {
    using namespace ZHLN;
    const CreativeWorkPaths paths = ResolvePaths();

    auto run_demo = [&]() -> std::expected<void, std::string> {
        // 1. OS Window Creation & RAII Scope Guard
        ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(1280, 720, "ZHLN Engine - Sponza Bindless");
        if (win.os_window == nullptr) {
            return std::unexpected("Failed to initialize OS window.");
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
        inst_exts_builder.Require(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        inst_exts_builder.OptionalIf(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, isMac);

        return inst_exts_builder.Build()
            .transform_error([](ZHLN::Error err) {
                return "Failed to build instance extensions: " + std::string(err.Message());
            })
            .and_then([&](const Vk::ExtensionResult& instExts) -> std::expected<void, std::string> {
                // 3. Create Vulkan Instance
                VkInstance instance = Vk::CreateInstance("ZHLN Engine - Sponza Bindless", VK_MAKE_API_VERSION(0, 1, 0, 0), instExts, true);
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

                std::span<const std::string_view> inst_exts_span = instExts;
                bool has_maint1 = std::ranges::any_of(inst_exts_span, [](std::string_view name) {
                    return name == VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
                });

                // 6. Build Device Extensions Monadically via Builder
                return Vk::ExtensionBuilder::ForDevice(physical.handle)
                    .Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                    .RequireIf(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, has_maint1)
                    .RequireIf(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, has_maint1)
                    .OptionalIf("VK_KHR_portability_subset", isMac)
                    .Build()
                    .transform_error([instance, raw_surface](ZHLN::Error err) {
                        vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                        vkDestroyInstance(instance, nullptr);
                        return "Failed to build device extensions: " + std::string(err.Message());
                    })
                    .and_then([&, instance, raw_surface, physical, has_maint1](const Vk::ExtensionResult& devExts) -> std::expected<void, std::string> {
                        
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
                                            .template Require<VkPhysicalDeviceVulkan13Features>([has_maint1, &swap_maint](auto& f) {
                                                f.pNext = has_maint1 ? &swap_maint : nullptr;
                                                f.synchronization2 = VK_TRUE;
                                                f.dynamicRendering = VK_TRUE;
                                                f.shaderDemoteToHelperInvocation = VK_TRUE;
                                            })
                                            .template Require<VkPhysicalDeviceVulkan12Features>([](auto& f) {
                                                f.descriptorIndexing                           = VK_TRUE;
                                                f.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
                                                f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                                                f.descriptorBindingPartiallyBound              = VK_TRUE;
                                                f.runtimeDescriptorArray                       = VK_TRUE;
                                                f.bufferDeviceAddress                          = VK_TRUE;
                                                f.drawIndirectCount                            = VK_TRUE;
                                                f.timelineSemaphore                            = VK_TRUE;
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

                        // 8. Create Context
                        auto ctx = Vk::Context::Create(instance, raw_surface, physical, dev_desc);
                        if (!ctx) {
                            vkDestroySurfaceKHR(instance, raw_surface, nullptr);
                            vkDestroyInstance(instance, nullptr);
                            return std::unexpected("Failed to create Vulkan logical device / Context.");
                        }

                        // 9. RAII Surface Holder
                        Vk::Surface surface(instance, raw_surface);

                        // 10. Allocator Setup
                        Vk::Allocator allocator;
                        if (!allocator.Init(ctx)) {
                            return std::unexpected("Failed to initialize Vulkan Memory Allocator.");
                        }

                        if (!paths.Validate()) {
                            return std::unexpected("Path validation unsatisfied. Verify asset paths.");
                        }

                        // --- Load glTF ---
                        std::println("Loading Sponza...");
                        auto gltf = GltfData::Load(paths.gltf);
                        if (!gltf.Valid()) {
                            return std::unexpected("Failed to load glTF Sponza scene.");
                        }
                        Scene scene = BuildScene(gltf.ptr);

                        std::vector<ZHLN::Vk::Passes::Light> scene_lights;
                        std::array<float, 3>                 sun_pos = {0.0F, 0.0F, 0.0F};

                        for (cgltf_size i = 0; i < gltf->nodes_count; ++i) {
                            cgltf_node* node = &gltf->nodes[i];
                            if (node->light == nullptr) {
                                continue;
                            }

                            std::string name = (node->name != nullptr) ? node->name : "";

                            if (node->light->type == cgltf_light_type_directional || name.contains("SUN")) {
                                sun_pos = {node->translation[0], node->translation[1], node->translation[2]};
                                continue;
                            }

                            if (name.contains("HDRI") || name.contains("SKY")) {
                                continue;
                            }

                            std::array<float, 16> matrix {};
                            cgltf_node_transform_world(node, matrix.data());

                            ZHLN::Vk::Passes::Light l {};
                            l.type     = (uint32_t) node->light->type;
                            l.color[0] = node->light->color[0];
                            l.color[1] = node->light->color[1];
                            l.color[2] = node->light->color[2];
                            l.intensity = (node->light->intensity <= 0.0F) ? 2.0F : node->light->intensity;
                            l.range     = (node->light->range <= 0.0F) ? 10.0F : node->light->range;
                            l.position[0] = matrix[12];
                            l.position[1] = matrix[13] - 0.5F;
                            l.position[2] = matrix[14];

                            scene_lights.push_back(l);
                        }

                        // Setup VBO / IBO
                        ZHLN::Vk::CommandPool setup_pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
                        if (!setup_pool.Allocate(1)) {
                            return std::unexpected("Failed to allocate setup command pool.");
                        }
                        VkCommandBuffer setup_cmd = setup_pool[0];
                        ZHLN_BeginCommandBuffer(setup_cmd);

                        auto vbo = ZHLN::Vk::Buffer::Create(
                            allocator.Get(), scene.vertices.size() * sizeof(ZHLN::Vk::Vertex), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY
                        );
                        auto ibo = ZHLN::Vk::Buffer::Create(
                            allocator.Get(), scene.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY
                        );

                        auto staging_vbo = ZHLN::Vk::UploadToBuffer(allocator.Get(), setup_cmd, vbo, scene.vertices.data(), scene.vertices.size() * sizeof(ZHLN::Vk::Vertex));
                        auto staging_ibo = ZHLN::Vk::UploadToBuffer(allocator.Get(), setup_cmd, ibo, scene.indices.data(), scene.indices.size() * sizeof(uint32_t));

                        auto light_buffer = ZHLN::Vk::Buffer::Create(
                            allocator.Get(), scene_lights.size() * sizeof(ZHLN::Vk::Passes::Light), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY
                        );

                        auto staging_lights =
                            ZHLN::Vk::UploadToBuffer(allocator.Get(), setup_cmd, light_buffer, scene_lights.data(), scene_lights.size() * sizeof(ZHLN::Vk::Passes::Light));

                        ZHLN_EndCommandBuffer(setup_cmd);
                        VkCommandBufferSubmitInfo setup_cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = setup_cmd};
                        VkSubmitInfo2             setup_submit  = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &setup_cmd_info};
                        vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setup_submit, VK_NULL_HANDLE);
                        vkQueueWaitIdle(ctx.GraphicsQueue());

                        // Dummy 1x1 textures
                        VkImageCreateInfo tex_base_info = {
                            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType     = VK_IMAGE_TYPE_2D,
                            .format        = VK_FORMAT_R8G8B8A8_UNORM,
                            .mipLevels     = 1,
                            .arrayLayers   = 1,
                            .samples       = VK_SAMPLE_COUNT_1_BIT,
                            .tiling        = VK_IMAGE_TILING_OPTIMAL,
                            .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
                        };

                        uint32_t          white_pixel = 0xFFFFFFFF;
                        VkImageCreateInfo dummy_info   = tex_base_info;
                        dummy_info.extent              = {.width = 1, .height = 1, .depth = 1};

                        ZHLN::Vk::TextureCreativeWork dummy_tex = ZHLN::Vk::UploadTexture<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx, dummy_info, &white_pixel);
                        uint32_t                      black_pixel = 0xFF000000;
                        ZHLN::Vk::TextureCreativeWork black_tex    = ZHLN::Vk::UploadTexture<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx, dummy_info, &black_pixel);

                        // Textures Upload
                        std::vector<ZHLN::Vk::TextureCreativeWork> textures(gltf->images_count);
                        std::println("Uploading {} textures...", gltf->images_count);
                        for (cgltf_size i = 0; i < gltf->images_count; ++i) {
                            ZHLN::Demo::ProcessEvents(win);
                            std::string full_path = paths.assetPrefix + gltf->images[i].uri;

                            if (full_path.contains("BaseColor")) {
                                textures[i] = UploadTextureFromFile<VK_FORMAT_R8G8B8A8_SRGB>(allocator, ctx, tex_base_info, full_path);
                            } else {
                                textures[i] = UploadTextureFromFile<VK_FORMAT_R8G8B8A8_UNORM>(allocator, ctx, tex_base_info, full_path);
                            }
                            std::println("  [{}/{}] {}", i + 1, gltf->images_count, gltf->images[i].uri);
                        }

                        // Presentation Context
                        ZHLN::Vk::PresentationContext presentation;
                        if (!presentation.Init(ctx, allocator, surface.Get(), win.width, win.height, true)) {
                            return std::unexpected("Failed to initialize Presentation Context.");
                        }

                        auto scene_color = ZHLN::Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
                            allocator, ctx, presentation.swapchain.Get().extent, {.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
                        );

                        constexpr uint32_t shadow_res = 4096;
                        auto               shadow_map  = ZHLN::Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                            allocator, ctx, {.width = shadow_res, .height = shadow_res}, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
                        );

                        // Samplers
                        auto default_sampler_res = ZHLN::Vk::SamplerBuilder {}.Linear().Repeat().Anisotropy(8.0F).Build(ctx.Device());
                        if (!default_sampler_res) {
                            return std::unexpected("Failed to build default sampler: " + std::string(default_sampler_res.error().Message()));
                        }
                        auto default_sampler = std::move(*default_sampler_res);

                        auto shadow_sampler_res = ZHLN::Vk::SamplerBuilder {}.Linear().ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).DepthCompare().Build(ctx.Device());
                        if (!shadow_sampler_res) {
                            return std::unexpected("Failed to build shadow sampler: " + std::string(shadow_sampler_res.error().Message()));
                        }
                        auto shadow_sampler = std::move(*shadow_sampler_res);

                        auto lightmap_sampler_res = ZHLN::Vk::SamplerBuilder {}.Linear().ClampToEdge().Build(ctx.Device());
                        if (!lightmap_sampler_res) {
                            return std::unexpected("Failed to build lightmap sampler: " + std::string(lightmap_sampler_res.error().Message()));
                        }
                        auto lightmap_sampler = std::move(*lightmap_sampler_res);

                        // Bindless Layout
                        auto desc_layout = SceneLayout::CreateLayout(ctx.Device());
                        auto desc_pool   = SceneLayout::CreatePool(ctx.Device(), 1);
                        VkDescriptorSet global_set = SceneLayout::Allocate(ctx.Device(), desc_pool.Get(), desc_layout.Get());

                        SceneLayout::Write(
                            ctx.Device(), global_set, ZHLN::Vk::SkipWrite {}, ZHLN::Vk::SamplerWrite {default_sampler.Get()}, ZHLN::Vk::ImageWrite {.view = shadow_map.view.Get()},
                            ZHLN::Vk::SamplerWrite {shadow_sampler.Get()}, ZHLN::Vk::SamplerWrite {lightmap_sampler.Get()}, ZHLN::Vk::BufferWrite {.buffer = light_buffer.Handle()}
                        );

                        ZHLN::Vk::BindlessRegistry<SceneLayout, 0> bindless;
                        bindless.Init(ctx.Device(), global_set);

                        uint32_t fallback_tex_idx = bindless.RegisterImage(dummy_tex.view.Get());
                        uint32_t black_tex_idx    = bindless.RegisterImage(black_tex.view.Get());

                        std::vector<uint32_t> bindless_texture_indices(gltf->images_count, fallback_tex_idx);
                        for (cgltf_size i = 0; i < gltf->images_count; ++i) {
                            if (textures[i].view.Valid()) {
                                bindless_texture_indices[i] = bindless.RegisterImage(textures[i].view.Get());
                            } else {
                                bindless_texture_indices[i] = fallback_tex_idx;
                            }
                        }

                        std::vector<MaterialCreativeWork> materials(gltf->materials_count);
                        for (cgltf_size i = 0; i < gltf->materials_count; ++i) {
                            cgltf_material* mat = &gltf->materials[i];

                            materials[i].albedoIdx   = fallback_tex_idx;
                            materials[i].normalIdx   = fallback_tex_idx;
                            materials[i].pbrIdx      = fallback_tex_idx;
                            materials[i].lightmapIdx = fallback_tex_idx;
                            materials[i].emissiveIdx = black_tex_idx;

                            if (mat->has_emissive_strength || (mat->emissive_texture.texture != nullptr)) {
                                if ((mat->emissive_texture.texture != nullptr) && (mat->emissive_texture.texture->image != nullptr)) {
                                    int img_idx               = static_cast<int>(mat->emissive_texture.texture->image - gltf->images);
                                    materials[i].emissiveIdx = bindless_texture_indices[img_idx];
                                }
                            }

                            if ((mat->name != nullptr) && std::string(mat->name).contains("master")) {
                                materials[i].emissiveIdx = black_tex_idx;
                            }

                            if (mat->has_pbr_metallic_roughness && (mat->pbr_metallic_roughness.base_color_texture.texture != nullptr)) {
                                cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
                                if (tex->image != nullptr) {
                                    int img_idx             = static_cast<int>(tex->image - gltf->images);
                                    materials[i].albedoIdx = bindless_texture_indices[img_idx];
                                }
                            }

                            if (mat->has_pbr_metallic_roughness && (mat->pbr_metallic_roughness.metallic_roughness_texture.texture != nullptr)) {
                                cgltf_texture* tex = mat->pbr_metallic_roughness.metallic_roughness_texture.texture;
                                if (tex->image != nullptr) {
                                    int img_idx          = static_cast<int>(tex->image - gltf->images);
                                    materials[i].pbrIdx = bindless_texture_indices[img_idx];
                                }
                            }

                            if (mat->normal_texture.texture != nullptr) {
                                cgltf_texture* tex = mat->normal_texture.texture;
                                if (tex->image != nullptr) {
                                    int img_idx             = static_cast<int>(tex->image - gltf->images);
                                    materials[i].normalIdx = bindless_texture_indices[img_idx];
                                }
                            }

                            for (uint32_t t = 0; t < gltf->images_count; ++t) {
                                std::string img_name = gltf->images[t].uri;
                                if (img_name.contains("Lightmap")) {
                                    materials[i].lightmapIdx = bindless_texture_indices[t];
                                    break;
                                }
                            }
                        }
                        MaterialCreativeWork fallback_mat = {.albedoIdx = fallback_tex_idx, .normalIdx = fallback_tex_idx};

                        // --- Post processing setup ---
                        auto fxaa_desc_layout = FXAALayout::CreateLayout(ctx.Device());
                        auto fxaa_desc_pool   = FXAALayout::CreatePool(ctx.Device(), 1);
                        VkDescriptorSet fxaa_set        = FXAALayout::Allocate(ctx.Device(), fxaa_desc_pool.Get(), fxaa_desc_layout.Get());

                        // --- Monadic compilation of pipelines and layout building ---
                        return ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), "fxaa.hlsl.VSMain.spv", "fxaa.hlsl.PSMain.spv", "VSMain", "PSMain")
                            .transform_error([](ZHLN::Error err) {
                                return "Failed to compile FXAA ShaderStages: " + std::string(err.Message());
                            })
                            .and_then([&, fxaa_set, fxaa_desc_layout = std::move(fxaa_desc_layout), fxaa_desc_pool = std::move(fxaa_desc_pool)](Vk::ShaderStages fxaaShaders) mutable {
                                return Vk::PipelineLayoutBuilder(ctx.Device())
                                    .AddDescriptorSetLayout(fxaa_desc_layout.Get())
                                    .Build()
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build FXAA Pipeline Layout: {}", err.Message());
                                    })
                                    .transform([fxaa_shaders = std::move(fxaaShaders)](Vk::PipelineLayout fxaaLayout) mutable {
                                        return std::make_pair(std::move(fxaa_shaders), std::move(fxaaLayout));
                                    });
                            })
                            .and_then([&, fxaa_set](std::pair<Vk::ShaderStages, Vk::PipelineLayout> fxaaPair) mutable {
                                return ZHLN::Vk::PipelineBuilder {}
                                    .Shaders(fxaaPair.first)
                                    .Layout(fxaaPair.second.Get())
                                    .ColorFormats({VK_FORMAT_B8G8R8_SRGB})
                                    .NoDepth()
                                    .CullNone()
                                    .Build(ctx.Device())
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build FXAA Graphics Pipeline: {}", err.Message());
                                    })
                                    .transform([fxaa_layout = std::move(fxaaPair.second)](Vk::Pipeline fxaaPipeline) mutable {
                                        return std::make_pair(std::move(fxaaPipeline), std::move(fxaa_layout));
                                    });
                            })
                            .and_then([&](std::pair<Vk::Pipeline, Vk::PipelineLayout> fxaaState) mutable {
                                return ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), paths.vertSpv, paths.fragSpv, "VSMain", "PSMain")
                                    .transform_error([](ZHLN::Error err) {
                                        return "Failed to compile main PBR ShaderStages: " + std::string(err.Message());
                                    })
                                    .transform([fxaa_state = std::move(fxaaState)](Vk::ShaderStages shaders) mutable {
                                        return std::make_tuple(std::move(fxaa_state.first), std::move(fxaa_state.second), std::move(shaders));
                                    });
                            })
                            .and_then([&](std::tuple<Vk::Pipeline, Vk::PipelineLayout, Vk::ShaderStages> state) mutable {
                                return ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), paths.shadowVertSpv, paths.shadowFragSpv, "VSMain", "PSMain")
                                    .transform_error([](ZHLN::Error err) {
                                        return "Failed to compile Shadow ShaderStages: " + std::string(err.Message());
                                    })
                                    .transform([state = std::move(state)](Vk::ShaderStages shadowShaders) mutable {
                                        auto& [fxaa_pipeline, fxaa_layout, shaders] = state;
                                        return std::make_tuple(
                                            std::move(fxaa_pipeline), std::move(fxaa_layout),
                                            std::move(shaders), std::move(shadowShaders)
                                        );
                                    });
                            })
                            .and_then([&](std::tuple<Vk::Pipeline, Vk::PipelineLayout, Vk::ShaderStages, Vk::ShaderStages> state) mutable {
                                return Vk::PipelineLayoutBuilder(ctx.Device())
                                    .AddDescriptorSetLayout(desc_layout.Get())
                                    .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ZHLN::Vk::Passes::PBRPushConstants))
                                    .Build()
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build Main Pipeline Layout: {}", err.Message());
                                    })
                                    .transform([state = std::move(state)](Vk::PipelineLayout pipelineLayout) mutable {
                                        auto& [fxaa_pipeline, fxaa_layout, shaders, shadow_shaders] = state;
                                        return std::make_tuple(
                                            std::move(fxaa_pipeline), std::move(fxaa_layout),
                                            std::move(shaders), std::move(shadow_shaders),
                                            std::move(pipelineLayout)
                                        );
                                    });
                            })
                            .and_then([&](std::tuple<Vk::Pipeline, Vk::PipelineLayout, Vk::ShaderStages, Vk::ShaderStages, Vk::PipelineLayout> state) mutable {
                                return Vk::PipelineLayoutBuilder(ctx.Device())
                                    .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT, sizeof(Mat4))
                                    .Build()
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build Shadow Pipeline Layout: {}", err.Message());
                                    })
                                    .transform([state = std::move(state)](Vk::PipelineLayout shadowLayout) mutable {
                                        auto& [fxaa_pipeline, fxaa_layout, shaders, shadow_shaders, pipeline_layout] = state;
                                        return std::make_tuple(
                                            std::move(fxaa_pipeline), std::move(fxaa_layout),
                                            std::move(shaders), std::move(shadow_shaders),
                                            std::move(pipeline_layout), std::move(shadowLayout)
                                        );
                                    });
                            })
                            .and_then([&](std::tuple<Vk::Pipeline, Vk::PipelineLayout, Vk::ShaderStages, Vk::ShaderStages, Vk::PipelineLayout, Vk::PipelineLayout> state) mutable {
                                auto& [fxaa_pipeline, fxaa_layout, shaders, shadow_shaders, pipeline_layout, shadow_layout] = state;

                                return ZHLN::Vk::PipelineBuilder {}
                                    .Shaders(shaders)
                                    .Layout(pipeline_layout.Get())
                                    .template Vertex<ZHLN::Vk::Vertex>()
                                    .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                                    .DepthFormat(VK_FORMAT_D32_SFLOAT)
                                    .WindingCW()
                                    .CullBack()
                                    .Build(ctx.Device())
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build Main PBR Graphics Pipeline: {}", err.Message());
                                    })
                                    .transform([state = std::move(state)](Vk::Pipeline pipeline) mutable {
                                        auto& [fxaa_pipeline, fxaa_layout, shaders, shadow_shaders, pipeline_layout, shadow_layout] = state;
                                        return std::make_tuple(
                                            std::move(fxaa_pipeline), std::move(fxaa_layout),
                                            std::move(shadow_shaders), std::move(pipeline_layout),
                                            std::move(shadow_layout), std::move(pipeline)
                                        );
                                    });
                            })
                            .and_then([&, ctx = std::move(ctx), fxaa_set](std::tuple<Vk::Pipeline, Vk::PipelineLayout, Vk::ShaderStages, Vk::PipelineLayout, Vk::PipelineLayout, Vk::Pipeline> state) mutable {
                                auto& [fxaa_pipeline, fxaa_pipeline_layout, shadow_shaders, pipeline_layout, shadow_layout, pipeline] = state;

                                return ZHLN::Vk::PipelineBuilder {}
                                    .Shaders(shadow_shaders)
                                    .Layout(shadow_layout.Get())
                                    .template Vertex<ZHLN::Vk::Vertex>()
                                    .DepthOnly()
                                    .CullNone()
                                    .Build(ctx.Device())
                                    .transform_error([](ZHLN::Error err) {
                                        return std::format("Failed to build Shadow Graphics Pipeline: {}", err.Message());
                                    })
                                    .and_then([&, state = std::move(state), ctx = std::move(ctx), fxaa_set](Vk::Pipeline shadowPipeline) mutable -> std::expected<void, std::string> {
                                        auto& [fxaa_pipeline, fxaa_pipeline_layout, shadow_shaders, pipeline_layout, shadow_layout, pipeline] = state;

                                        // --- Frame loop resources ---
                                        auto sync  = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
                                        auto pools = ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1});

                                        uint32_t frame_index = 0;
                                        win.resized          = true;
                                        FpsCounter fps_counter;
                                        auto       start_time = std::chrono::high_resolution_clock::now();

                                        std::println("Rendering started (Bindless Architecture).");

                                        while (win.running) {
                                            ZHLN::Demo::ProcessEvents(win);
                                            if (win.width == 0 || win.height == 0) {
                                                continue;
                                            }

                                            if (win.resized) {
                                                if (!presentation.Rebuild(win.width, win.height)) {
                                                    continue;
                                                }

                                                scene_color = ZHLN::Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
                                                    allocator, ctx, presentation.swapchain.Get().extent, {.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
                                                );

                                                FXAALayout::Write(ctx.Device(), fxaa_set, ZHLN::Vk::ImageWrite {.view = scene_color.view.Get()}, ZHLN::Vk::SamplerWrite {default_sampler.Get()});

                                                win.resized = false;
                                            }

                                            if (!presentation.swapchain.Valid() || presentation.swapchain.Get().extent.width == 0) {
                                                continue;
                                            }

                                            float time = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - start_time).count();

                                            float cam_x  = std::sin(time * 0.1F) * 12.0F;
                                            float cam_y  = 2.5F;
                                            float cam_z  = 0.0F;
                                            float look_x = cam_x + (std::cos(time * 0.1F) * 2.0F);

                                            Mat4 view     = LookAt({cam_x, cam_y, cam_z}, {look_x, cam_y, 0.0F}, {0.0F, 1.0F, 0.0F});
                                            Mat4 proj     = Perspective(1.0472F, (float) win.width / (float) win.height, 0.1F, 1000.0F);
                                            Mat4 view_proj = Multiply(proj, view);

                                            Mat4 light_view = LookAt({sun_pos[0], sun_pos[1], sun_pos[2]}, {0.0F, 2.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
                                            Mat4 light_proj = Ortho(-100.0F, 100.0F, -100.0F, 100.0F, 0.1F, 400.0F);

                                            Mat4 shadow_proj_view = Multiply(light_proj, light_view);

                                            Mat4 bias_matrix       = {{
                                                0.5F, 0.0F, 0.0F, 0.0F,
                                                0.0F, 0.5F, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F,
                                                0.5F, 0.5F, 0.0F, 1.0F
                                            }};
                                            Mat4 light_space_biased = Multiply(bias_matrix, shadow_proj_view);

                                            if (auto title = fps_counter.Tick(scene.drawCalls.size()); !title.empty()) {
                                                ZHLN::Demo::UpdateWindowTitle(win, title.c_str());
                                            }

                                            const ZHLN_FrameSync& frame_sync = sync[frame_index];
                                            VkCommandBuffer       cmd        = pools.Cmd(frame_index);
                                            ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, pools[frame_index]);

                                            uint32_t         image_index = 0;
                                            ZHLN_AcquireDesc acq = {.swapchain = presentation.swapchain.Get().handle, .image_available = frame_sync.image_available, .timeout_ns = UINT64_MAX};
                                            if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
                                                win.resized = true;
                                                continue;
                                            }

                                            ZHLN_BeginCommandBuffer(cmd);

                                            std::vector<ZHLN::Vk::Passes::PBRDrawCall> pbr_calls;
                                            pbr_calls.reserve(scene.drawCalls.size());

                                            for (const auto& dc: scene.drawCalls) {
                                                const auto& mat = (dc.mesh->materialIndex >= 0) ? materials[dc.mesh->materialIndex] : fallback_mat;
                                                pbr_calls.push_back(
                                                    {.worldMatrix = dc.worldMatrix.data,
                                                     .albedoIdx   = mat.albedoIdx,
                                                     .normalIdx   = mat.normalIdx,
                                                     .pbrIdx      = mat.pbrIdx,
                                                     .lightmapIdx = mat.lightmapIdx,
                                                     .emissiveIdx = mat.emissiveIdx,
                                                     .indexCount  = dc.mesh->indexCount,
                                                     .firstIndex  = dc.mesh->firstIndex}
                                                );
                                            }

                                            std::array<float, 3> light_pos    = {40.0F, 60.0F, 40.0F};
                                            std::array<float, 3> light_target = {0.0F, 10.0F, 0.0F};

                                            float dir_x = light_target[0] - light_pos[0];
                                            float dir_y = light_target[1] - light_pos[1];
                                            float dir_z = light_target[2] - light_pos[2];
                                            float len  = std::sqrt((dir_x * dir_x) + (dir_y * dir_y) + (dir_z * dir_z));

                                            ZHLN::Vk::Passes::PBRSceneContext scene_ctx = {
                                                .viewProj         = view_proj.data,
                                                .lightSpaceMatrix = light_space_biased.data,
                                                .shadowProjView   = shadow_proj_view.data,
                                                .camPos           = {cam_x, cam_y, cam_z, 1.0F},
                                                .lightDir         = {dir_x / len, dir_y / len, dir_z / len, 0.0F},
                                                .lightCount       = static_cast<uint32_t>(scene_lights.size()),
                                                .globalSet        = global_set,
                                                .vbo              = vbo.Handle(),
                                                .ibo              = ibo.Handle()
                                            };

                                            AppFrameData frame_data = {
                                                .shadowData = {.pipeline = shadowPipeline.Get(), .layout = shadow_layout.Get(), .drawCalls = &pbr_calls, .scene = &scene_ctx},
                                                .pbrData    = {.pipeline = pipeline.Get(), .layout = pipeline_layout.Get(), .drawCalls = &pbr_calls, .scene = &scene_ctx},
                                                .fxaaData   = {.pipeline = fxaa_pipeline.Get(), .layout = fxaa_pipeline_layout.Get(), .set = fxaa_set}
                                            };

                                            auto shadow_u = shadow_map.State();
                                            auto color_u  = scene_color.State();
                                            auto depth_u  = presentation.depthTarget.State();

                                            ZHLN::Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_u = {
                                                .handle = presentation.swapchain.Get().images[image_index],
                                                .view   = presentation.swapchain.Get().views[image_index],
                                                .extent = presentation.swapchain.Get().extent,
                                                .aspect = VK_IMAGE_ASPECT_COLOR_BIT
                                            };

                                            auto shadow_att = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, shadow_u);
                                            ZHLN::Vk::DynamicPass({.width = shadow_res, .height = shadow_res})
                                                .AddDepth(shadow_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0F)
                                                .Execute(cmd, [&]() { ZHLN::Vk::Passes::DrawShadows(cmd, frame_data.shadowData); });

                                            auto shadow_ro = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, shadow_att);

                                            auto color_att = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, color_u);
                                            auto depth_att = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, depth_u);

                                            ZHLN::Vk::DynamicPass(presentation.swapchain.Get().extent)
                                                .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {.r = 0.05F, .g = 0.05F, .b = 0.07F, .a = 1.0F})
                                                .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0F)
                                                .Execute(cmd, [&]() { ZHLN::Vk::Passes::DrawPBR(cmd, frame_data.pbrData); });

                                            auto color_ro = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, color_att);

                                            auto swap_att = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, swap_u);

                                            ZHLN::Vk::DynamicPass(presentation.swapchain.Get().extent)
                                                .AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE)
                                                .Execute(cmd, [&]() { ZHLN::Vk::Passes::DrawFXAA(cmd, frame_data.fxaaData); });

                                            (void) ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swap_att);

                                            ZHLN_EndCommandBuffer(cmd);

                                            ZHLN_FrameSubmitDesc submit_desc = {
                                                .graphicsQueue    = ctx.GraphicsQueue(),
                                                .presentQueue     = ctx.PresentQueue(),
                                                .cmd              = cmd,
                                                .imageAvailable   = frame_sync.image_available,
                                                .renderFinished   = presentation.presentSemaphores[image_index],
                                                .inFlight         = frame_sync.in_flight,
                                                .swapchain        = presentation.swapchain.Get().handle,
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
