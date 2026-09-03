// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/graphics/HostBlitSwapchain.cpp
//
// ============================================================================
// HostBlit — offscreen host presenter (black-box plugin)
// ============================================================================
//
// WHY THIS EXISTS
//   On macOS there is no native Vulkan WSI: glfwGetRequiredInstanceExtensions()
//   returns 0 extensions for anything except MoltenVK, so the engine cannot
//   create a VkSurfaceKHR/VkSwapchainKHR there (VK_KHR_surface & friends are
//   simply absent from the Lavapipe ICD). The engine still renders fine
//   headlessly into offscreen VkImages; this file turns any such image into
//   pixels on a real GLFW window through a plain OpenGL 2.1 context.
//
//   Vulkan side: the image is copied into this plugin's OWN host-visible
//   staging buffer (private command pool, private fence, private memory) and
//   the buffer is mapped with vkMapMemory. Nothing in the engine's command
//   streams, fences, heaps or swapchain objects is touched — the only Vulkan
//   object this file ever references beyond its own is the VkImage you hand
//   it, and that image is only read (COPY src) and transitioned straight
//   back to the layout it came in with.
//
//   OpenGL side: the mapped pixels are blitted with glDrawPixels into the
//   framebuffer of a GLFW window that has an actual GL context. The engine
//   creates its windows with GLFW_CLIENT_API = GLFW_NO_API, so if the window
//   you pass has no context, the plugin opens and owns its own 2.1 window
//   instead. Nothing is shared with the engine's windows.
//
// ISOLATION CONTRACT
//   * 100% of the window blit logic lives in this translation unit.
//   * No engine headers except the renderer's own Vk::Image RAII type.
//   * Never references the real Vulkan swapchain/present path; if a native
//     swapchain exists, this file must simply not be called.
//   * No static initializers; inert until Init() is called.
//
// HOW TO WIRE IT UP (call site is yours; paste these declarations there)
//
//   namespace ZHLN::HostBlit {
//   [[nodiscard]] bool Init(VkPhysicalDevice gpu, VkDevice device,
//                           VkQueue queue, uint32_t queueFamily) noexcept;
//   [[nodiscard]] bool Present(const ZHLN::Vk::Image& src, GLFWwindow* window,
//                              uint32_t width, uint32_t height,
//                              VkFormat format = VK_FORMAT_B8G8R8A8_SRGB,
//                              VkImageLayout srcLayout =
//                                  VK_IMAGE_LAYOUT_GENERAL) noexcept;
//   void Shutdown() noexcept;
//   } // namespace ZHLN::HostBlit
//
//   // once, after the Vulkan device exists:
//   ZHLN::HostBlit::Init(physicalDevice, device, graphicsQueue, queueFamily);
//   // every frame, AFTER the engine submitted the work that rendered `image`:
//   if (!ZHLN::HostBlit::Present(image, glfwWindow, 1280, 720)) break;
//   // on exit:
//   ZHLN::HostBlit::Shutdown();
//
// THREADING
//   Call Init/Present/Shutdown from one thread — the same thread that owns
//   `queue` submissions. Present submits to `queue` and blocks on its own
//   fence, which also guarantees the engine's prior rendering is complete.
//
// ============================================================================

#include "Rendering.hpp" // Vulkan core (PCH of the render module)
#include "Allocator.hpp" // ZHLN::Vk::Image (handle-only RAII wrapper)

#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h> // legacy 2.1 API: glDrawPixels & friends
#else
#include <GL/gl.h>
#endif

#include <cstdio>
#include <cstdint>

// GL 1.2 imaging constants — present in every GL 2.1 header we target, but
// pinned here so the file compiles even against a minimal GL 1.1 gl.h.
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif

namespace ZHLN::HostBlit {
namespace {

// ---------------------------------------------------------------------------
// Plugin-private state. Everything Vulkan here is created by this file.
// ---------------------------------------------------------------------------
struct State {
    // Plumbing handed to Init().
    VkPhysicalDevice gpu         = VK_NULL_HANDLE;
    VkDevice         device      = VK_NULL_HANDLE;
    VkQueue          queue       = VK_NULL_HANDLE;
    uint32_t         queueFamily = 0;

    // Private Vulkan objects.
    VkCommandPool   cmdPool   = VK_NULL_HANDLE;
    VkCommandBuffer cmd       = VK_NULL_HANDLE;
    VkFence         fence     = VK_NULL_HANDLE;
    VkBuffer        staging   = VK_NULL_HANDLE;
    VkDeviceMemory  memory    = VK_NULL_HANDLE;
    void*           mapped    = nullptr;
    VkDeviceSize    capacity  = 0;

    // GL presentation window (owned only when the caller's window has no
    // GL context, which is the case for every engine window: GLFW_NO_API).
    GLFWwindow* glWindow  = nullptr;
    bool        ownsGlfw  = false; // did this file call glfwInit()?

    bool ready = false;
} g;

void Log(const char* msg) {
    std::fprintf(stderr, "Zahlen: [HostBlit] %s\n", msg);
}

// Find a HOST_VISIBLE|HOST_COHERENT memory type (HOST_CACHED preferred —
// trivially satisfied on Lavapipe, whose memory is unified anyway).
bool FindHostMemoryType(uint32_t typeBits, bool preferCached, uint32_t& out) noexcept {
    VkPhysicalDeviceMemoryProperties props {};
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &props);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
        const bool usable = (typeBits & (1u << i)) != 0 &&
                            (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                            (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (!usable) continue;
        if ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
            out = i;
            return true;
        }
        if (fallback == UINT32_MAX) fallback = i;
    }
    if (fallback != UINT32_MAX) {
        out = fallback;
        return true;
    }
    (void)preferCached;
    return false;
}

void DestroyStaging() noexcept {
    if (g.staging != VK_NULL_HANDLE) {
        if (g.mapped != nullptr) vkUnmapMemory(g.device, g.memory);
        vkDestroyBuffer(g.device, g.staging, nullptr);
        vkFreeMemory(g.device, g.memory, nullptr);
        g.staging = VK_NULL_HANDLE;
        g.memory  = VK_NULL_HANDLE;
        g.mapped  = nullptr;
        g.capacity = 0;
    }
}

bool EnsureStaging(VkDeviceSize bytes) noexcept {
    if (bytes <= g.capacity) return true;
    DestroyStaging();

    const VkBufferCreateInfo bi {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext       = nullptr,
        .flags       = 0,
        .size        = bytes,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
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
    if (!FindHostMemoryType(req.memoryTypeBits, true, typeIndex)) {
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
    if (vkBindBufferMemory(g.device, g.staging, g.memory, 0) != VK_SUCCESS ||
        vkMapMemory(g.device, g.memory, 0, req.size, 0, &g.mapped) != VK_SUCCESS) {
        Log("Could not bind/map the host staging buffer.");
        DestroyStaging();
        return false;
    }
    g.capacity = bytes;
    return true;
}

// Copy mip 0 / layer 0 of `image` into the mapped staging buffer and block
// until the pixels are CPU-visible. The image is transitioned back to the
// layout the caller declared, so the engine never observes a stray layout.
bool ReadBackPixels(VkImage image, uint32_t width, uint32_t height,
                    VkImageLayout srcLayout) noexcept {
    if (vkResetFences(g.device, 1, &g.fence) != VK_SUCCESS) return false;

    const VkCommandBufferBeginInfo begin {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (vkBeginCommandBuffer(g.cmd, &begin) != VK_SUCCESS) return false;

    const VkImageSubresourceRange range {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                       VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
        const VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = srcAccess,
            .dstAccessMask       = dstAccess,
            .oldLayout           = from,
            .newLayout           = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = image,
            .subresourceRange    = range,
        };
        vkCmdPipelineBarrier(g.cmd, srcStage, dstStage, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    VkBufferImageCopy region {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(g.cmd, image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           g.staging, 1, &region);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    if (vkEndCommandBuffer(g.cmd) != VK_SUCCESS) return false;

    const VkSubmitInfo si {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .waitSemaphoreCount   = 0,
        .pWaitSemaphores      = nullptr,
        .pWaitDstStageMask    = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &g.cmd,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores    = nullptr,
    };
    if (vkQueueSubmit(g.queue, 1, &si, g.fence) != VK_SUCCESS) return false;
    return vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

// Pick a GL window with a usable context: the caller's window when it has
// one, otherwise a plugin-owned 2.1 window sized to the caller's window.
GLFWwindow* ResolveWindow(GLFWwindow* requested, uint32_t width, uint32_t height) noexcept {
    if (requested != nullptr &&
        glfwGetWindowAttrib(requested, GLFW_CLIENT_API) != GLFW_NO_API) {
        return requested;
    }
    if (g.glWindow != nullptr && !glfwWindowShouldClose(g.glWindow)) {
        return g.glWindow;
    }
    if (g.glWindow != nullptr) { // user closed the plugin window
        glfwDestroyWindow(g.glWindow);
        g.glWindow = nullptr;
    }
    if (!glfwInit()) {
        Log("glfwInit failed; cannot open a host presentation window.");
        return nullptr;
    }
    g.ownsGlfw = true;

    int fbW = static_cast<int>(width), fbH = static_cast<int>(height);
    if (requested != nullptr) {
        glfwGetFramebufferSize(requested, &fbW, &fbH); // Retina-aware
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_NATIVE_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    g.glWindow = glfwCreateWindow(fbW > 0 ? fbW : 1280, fbH > 0 ? fbH : 720,
                                  "Zahlen — Host Blit", nullptr, nullptr);
    if (g.glWindow == nullptr) {
        Log("glfwCreateWindow (OpenGL 2.1) failed.");
        return nullptr;
    }
    return g.glWindow;
}

// Legacy-GL blit: raster pos at top-left + negative zoom flips the
// top-down Vulkan readback into GL's bottom-up framebuffer and scales it
// to the window in the same step. Nearest filtering — this is a debug
// presenter, not a scaler.
void GlBlit(uint32_t width, uint32_t height, VkFormat format) noexcept {
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(glfwGetCurrentContext(), &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0) return;

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
    default: // B8G8R8A8_UNORM / _SRGB and anything else 4x8 little-endian
        break;
    }

    glViewport(0, 0, fbW, fbH);
    glDisable(GL_DEPTH_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glRasterPos2i(0, fbH);
    glPixelZoom(static_cast<float>(fbW) / static_cast<float>(width),
                -static_cast<float>(fbH) / static_cast<float>(height));
    glDrawPixels(static_cast<int>(width), static_cast<int>(height),
                 glFormat, glType, g.mapped);
    glPixelZoom(1.0f, 1.0f);
    glFlush();
}

} // namespace

// ---------------------------------------------------------------------------
// Public surface (declarations mirrored in the banner comment above)
// ---------------------------------------------------------------------------
void Shutdown() noexcept;

[[nodiscard]] bool Init(VkPhysicalDevice gpu, VkDevice device,
                        VkQueue queue, uint32_t queueFamily) noexcept {
    if (g.ready) return true;
    if (gpu == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        Log("Init needs a valid physical device, device and queue.");
        return false;
    }
    g.gpu         = gpu;
    g.device      = device;
    g.queue       = queue;
    g.queueFamily = queueFamily;

    const VkCommandPoolCreateInfo pi {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };
    if (vkCreateCommandPool(device, &pi, nullptr, &g.cmdPool) != VK_SUCCESS) {
        Log("vkCreateCommandPool failed.");
        return false;
    }
    const VkCommandBufferAllocateInfo ai {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = g.cmdPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(device, &ai, &g.cmd) != VK_SUCCESS) {
        Log("vkAllocateCommandBuffers failed.");
        Shutdown();
        return false;
    }
    const VkFenceCreateInfo fi {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    if (vkCreateFence(device, &fi, nullptr, &g.fence) != VK_SUCCESS) {
        Log("vkCreateFence failed.");
        Shutdown();
        return false;
    }
    g.ready = true;
    Log("Initialized (offscreen host presenter; no engine state touched).");
    return true;
}

[[nodiscard]] bool Present(const ZHLN::Vk::Image& src, GLFWwindow* window,
                           uint32_t width, uint32_t height,
                           VkFormat format, VkImageLayout srcLayout) noexcept {
    if (!g.ready || !src.Valid() || width == 0 || height == 0) return false;

    GLFWwindow* target = ResolveWindow(window, width, height);
    if (target == nullptr) return false;
    if (g.ownsGlfw) glfwPollEvents(); // keep the plugin window responsive
    if (glfwWindowShouldClose(target)) return false;

    // 4 bytes/px covers every supported format (RGB8 rows are ≤ RGBA8 size).
    if (!EnsureStaging(static_cast<VkDeviceSize>(width) * height * 4)) return false;
    if (!ReadBackPixels(src.Handle(), width, height, srcLayout)) {
        Log("Readback copy failed; frame skipped.");
        return !glfwWindowShouldClose(target);
    }

    glfwMakeContextCurrent(target);
    GlBlit(width, height, format);
    glfwSwapBuffers(target);
    return !glfwWindowShouldClose(target);
}

void Shutdown() noexcept {
    if (g.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g.device);
        DestroyStaging();
        if (g.fence != VK_NULL_HANDLE) vkDestroyFence(g.device, g.fence, nullptr);
        if (g.cmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(g.device, g.cmdPool, nullptr);
    }
    if (g.glWindow != nullptr) glfwDestroyWindow(g.glWindow);
    if (g.ownsGlfw) glfwTerminate();
    g = State {};
}

} // namespace ZHLN::HostBlit
