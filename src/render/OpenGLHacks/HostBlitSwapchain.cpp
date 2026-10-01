
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#if defined(__APPLE__)

#include <GLFW/glfw3.h>
#include <OpenGL/gl.h>
#include <Rendering.hpp>

#include <cstdint>
#include <cstdio>
#include <utility>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif

namespace ZHLN::HostBlit {
namespace {

struct State {
    VkPhysicalDevice gpu         = VK_NULL_HANDLE;
    VkDevice         device      = VK_NULL_HANDLE;
    VkQueue          queue       = VK_NULL_HANDLE;
    uint32_t         queueFamily = 0;

    Vk::CommandRing<Vk::QueueType::Graphics, 1> ring;
    VkBuffer                                    staging  = VK_NULL_HANDLE;
    VkDeviceMemory                              memory   = VK_NULL_HANDLE;
    void*                                       mapped   = nullptr;
    VkDeviceSize                                capacity = 0;

    GLFWwindow* glWindow = nullptr;

    bool ready = false;
} g;

void Log(const char* msg) {
    std::fprintf(stderr, "Zahlen: [HostBlit] %s\n", msg);
}

bool FindHostMemoryType(uint32_t typeBits, uint32_t& out) noexcept {
    VkPhysicalDeviceMemoryProperties props {};
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &props);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
        const bool usable = (typeBits & (1u << i)) != 0 && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (!usable)
            continue;
        if ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
            out = i;
            return true;
        }
        if (fallback == UINT32_MAX)
            fallback = i;
    }
    if (fallback != UINT32_MAX) {
        out = fallback;
        return true;
    }
    return false;
}

void DestroyStaging() noexcept {
    if (g.staging != VK_NULL_HANDLE) {
        if (g.mapped != nullptr)
            vkUnmapMemory(g.device, g.memory);
        vkDestroyBuffer(g.device, g.staging, nullptr);
        vkFreeMemory(g.device, g.memory, nullptr);
        g.staging  = VK_NULL_HANDLE;
        g.memory   = VK_NULL_HANDLE;
        g.mapped   = nullptr;
        g.capacity = 0;
    }
}

bool EnsureStaging(VkDeviceSize bytes) noexcept {
    if (bytes <= g.capacity)
        return true;
    DestroyStaging();

    const VkBufferCreateInfo bi {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = bytes,
        .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
    };
    if (vkCreateBuffer(g.device, &bi, nullptr, &g.staging) != VK_SUCCESS) {
        Log("vkCreateBuffer failed for the host staging buffer.");
        return false;
    }

    VkMemoryRequirements req {};
    vkGetBufferMemoryRequirements(g.device, g.staging, &req);
    uint32_t typeIndex = 0;
    if (!FindHostMemoryType(req.memoryTypeBits, typeIndex)) {
        Log("No HOST_VISIBLE|HOST_COHERENT memory type available.");
        vkDestroyBuffer(g.device, g.staging, nullptr);
        g.staging = VK_NULL_HANDLE;
        return false;
    }

    const VkMemoryAllocateInfo ai {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = req.size,
        .memoryTypeIndex = typeIndex,
    };
    if (vkAllocateMemory(g.device, &ai, nullptr, &g.memory) != VK_SUCCESS) {
        Log("vkAllocateMemory failed for the host staging buffer.");
        vkDestroyBuffer(g.device, g.staging, nullptr);
        g.staging = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(g.device, g.staging, g.memory, 0) != VK_SUCCESS || vkMapMemory(g.device, g.memory, 0, req.size, 0, &g.mapped) != VK_SUCCESS) {
        Log("Could not bind/map the host staging buffer.");
        DestroyStaging();
        return false;
    }
    g.capacity = bytes;
    return true;
}

bool ReadBackPixels(VkImage image, uint32_t width, uint32_t height, VkImageLayout srcLayout) noexcept {
    auto acquired = g.ring.Acquire();
    if (!acquired) { return false; }
    auto slot = *acquired;
    auto recording = Vk::CommandRecorder::Begin(slot.cmd);
    if (!recording) { return false; }
    const VkCommandBuffer cmd = recording->Handle();

    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess) {
        Vk::ImageBarrier(cmd, ZHLN_ImageBarrierDesc {
                                  .image      = image,
                                  .src_access = srcAccess,
                                  .dst_access = dstAccess,
                                  .src_layout = from,
                                  .dst_layout = to,
                                  .src_stage  = srcStage,
                                  .dst_stage  = dstStage,
                                  .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .base_mip   = 0,
                                  .mip_count  = 1,
                              });
    };

    barrier(
        srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT
    );

    Vk::CopyImageToBuffer(cmd, image, g.staging, VkExtent2D {width, height});

    barrier(
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
    );

    auto executable = std::move(*recording).End();
    if (!executable || !g.ring.Submit(g.queue, slot, std::move(*executable))) { return false; }
    return vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

GLFWwindow* ResolveWindow(GLFWwindow* requested, uint32_t width, uint32_t height) noexcept {
    if (requested != nullptr && glfwGetWindowAttrib(requested, GLFW_CLIENT_API) != GLFW_NO_API) {
        return requested;
    }
    if (g.glWindow != nullptr && !glfwWindowShouldClose(g.glWindow)) {
        return g.glWindow;
    }
    if (g.glWindow != nullptr) {
        glfwDestroyWindow(g.glWindow);
        g.glWindow = nullptr;
    }
    if (!glfwInit()) {
        Log("glfwInit failed; cannot open a host presentation window.");
        return nullptr;
    }

    int fbW = static_cast<int>(width), fbH = static_cast<int>(height);
    if (requested != nullptr) {
        glfwGetFramebufferSize(requested, &fbW, &fbH);
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_NATIVE_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    g.glWindow = glfwCreateWindow(fbW > 0 ? fbW : 1280, fbH > 0 ? fbH : 720, "Zahlen — Host Blit", nullptr, nullptr);
    if (g.glWindow == nullptr) {
        Log("glfwCreateWindow (OpenGL 2.1) failed.");
        return nullptr;
    }
    return g.glWindow;
}

void GlBlit(uint32_t width, uint32_t height, VkFormat format) noexcept {
    int fbW = 0;
    int fbH = 0;
    glfwGetFramebufferSize(glfwGetCurrentContext(), &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0 || !g.mapped) {
        return;
    }

    GLenum glFormat = GL_BGRA;
    GLenum glType   = GL_UNSIGNED_INT_8_8_8_8_REV;

    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            glFormat = GL_RGBA;
            glType   = GL_UNSIGNED_BYTE;
            break;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SRGB:
            glFormat = GL_RGB;
            glType   = GL_UNSIGNED_BYTE;
            break;
        default:
            break;
    }

    glViewport(0, 0, fbW, fbH);
    glDisable(GL_DEPTH_TEST);

    glPixelStorei(GL_UNPACK_ALIGNMENT, (glFormat == GL_RGB) ? 1 : 4);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glWindowPos2i(0, fbH);

    glPixelZoom(static_cast<float>(fbW) / static_cast<float>(width), -static_cast<float>(fbH) / static_cast<float>(height));

    glDrawPixels(static_cast<int>(width), static_cast<int>(height), glFormat, glType, g.mapped);

    glPixelZoom(1.0f, 1.0f);
}

}

void Shutdown() noexcept;

[[nodiscard]] bool Init(VkPhysicalDevice gpu, VkDevice device, VkQueue queue, uint32_t queueFamily) noexcept {
    if (g.ready)
        return true;
    if (gpu == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        Log("Init needs a valid physical device, device and queue.");
        return false;
    }
    g.gpu         = gpu;
    g.device      = device;
    g.queue       = queue;
    g.queueFamily = queueFamily;

    if (auto res = g.ring.Init(device, queueFamily); !res) {
        const std::string_view why = ZHLN::Error(res.error()).Message();
        std::fprintf(stderr, "Zahlen: [HostBlit] Command ring init failed: %.*s\n", static_cast<int>(why.size()), why.data());
        Shutdown();
        return false;
    }
    g.ready = true;
    Log("Initialized (offscreen host presenter; no engine state touched).");
    return true;
}

[[nodiscard]] bool Present(const ZHLN::Vk::Image& src, void* nativeWindow, uint32_t width, uint32_t height, VkFormat format, VkImageLayout srcLayout) noexcept {
    if (!g.ready || !src.Valid() || width == 0 || height == 0)
        return false;

    GLFWwindow* target = ResolveWindow(static_cast<GLFWwindow*>(nativeWindow), width, height);
    if (target == nullptr)
        return false;
    glfwPollEvents();
    if (glfwWindowShouldClose(target))
        return false;

    bool catchUp = false;
    if (target == g.glWindow) {
        int curW = 0;
        int curH = 0;
        glfwGetWindowSize(target, &curW, &curH);
        if (curW != static_cast<int>(width) || curH != static_cast<int>(height)) {
            glfwSetWindowSize(target, static_cast<int>(width), static_cast<int>(height));
            catchUp = true;
        }
    }

    if (!EnsureStaging(static_cast<VkDeviceSize>(width) * height * 4))
        return false;
    if (!ReadBackPixels(src.Handle(), width, height, srcLayout)) {
        Log("Readback copy failed; frame skipped.");
        return !glfwWindowShouldClose(target);
    }

    glfwMakeContextCurrent(target);
    if (!catchUp) {
        GlBlit(width, height, format);
    }
    glfwSwapBuffers(target);
    return !glfwWindowShouldClose(target);
}

void Shutdown() noexcept {
    if (g.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g.device);
        DestroyStaging();
        g.ring.Cleanup();
    }
    if (g.glWindow != nullptr)
        glfwDestroyWindow(g.glWindow);
    g = State {};
}

}

#else

#include "HostBlit.hpp"

namespace ZHLN::HostBlit {

[[nodiscard]] bool Init(VkPhysicalDevice, VkDevice, VkQueue, uint32_t) noexcept {
    return false;
}

[[nodiscard]] bool Present(const ZHLN::Vk::Image&, void*, uint32_t, uint32_t, VkFormat, VkImageLayout) noexcept {
    return false;
}

void Shutdown() noexcept {
}

}

#endif
