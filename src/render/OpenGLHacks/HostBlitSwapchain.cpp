
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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
//   * Borrows the renderer's vocabulary, never its lifetimes. The stateless
//     recording helpers (Vk::ImageBarrier, Vk::CopyImageToBuffer,
//     Vk::CommandBufferGuard), the Vk::Image RAII type and Vk::CommandRing are
//     used because they carry no engine state. The ring instance below is
//     plugin-local, so the command pool, command buffer and fence it owns are
//     still created and destroyed here, as are the staging buffer and the
//     device memory. That matters because the engine tears the RenderContext
//     down before its Window and this plugin must not depend on that ordering.
//   * Never references the real Vulkan swapchain/present path; if a native
//     swapchain exists, this file must simply not be called.
//   * Nothing touches the GPU or the engine before Init() is called. The state
//     below is constant-initialised, but the command ring has a destructor, so
//     the compiler does emit one atexit registration for it. Its Cleanup()
//     early-returns on a null device, so that is inert unless Init() succeeded
//     and Shutdown() never ran.
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

// The host presenter is a macOS-only escape hatch. Keep the real implementation
// entirely behind this branch: non-Apple builds compile only this file's inert
// definitions below and never include an OpenGL or GLFW header.
#if defined(__APPLE__)

#include <GLFW/glfw3.h>
#include <OpenGL/gl.h> // legacy 2.1 API: glDrawPixels & friends
#include <Rendering.hpp> // Vulkan core (PCH of the render module)

#include <cstdint>
#include <cstdio>

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

    // Private Vulkan objects. The ring owns the command pool, the single
    // command buffer and its fence -- capacity 1 is exactly what this path
    // needs, and its Init() reports failure instead of skipping a broken slot.
    // QueueType is a compile-time tag only; the real queue family is whatever
    // Init() was handed.
    Vk::CommandRing<Vk::QueueType::Graphics, 1> ring;
    VkBuffer                                    staging  = VK_NULL_HANDLE;
    VkDeviceMemory                              memory   = VK_NULL_HANDLE;
    void*                                       mapped   = nullptr;
    VkDeviceSize                                capacity = 0;

    // GL presentation window (owned only when the caller's window has no
    // GL context, which is the case for every engine window: GLFW_NO_API).
    // The window is plugin-owned; GLFW itself never is (see ResolveWindow).
    GLFWwindow* glWindow = nullptr;

    bool ready = false;
} g;

void Log(const char* msg) {
    std::fprintf(stderr, "Zahlen: [HostBlit] %s\n", msg);
}

// Find a HOST_VISIBLE|HOST_COHERENT memory type, preferring HOST_CACHED when
// the device offers one — trivially satisfied on Lavapipe, whose memory is
// unified anyway. Falls back to any host-visible coherent type.
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

// Copy mip 0 / layer 0 of `image` into the mapped staging buffer and block
// until the pixels are CPU-visible. The image is transitioned back to the
// layout the caller declared, so the engine never observes a stray layout.
//
// The barriers and the copy go through the renderer's own helpers. They are
// stateless command-recording functions — a VkCommandBuffer and a descriptor,
// nothing else — so using them borrows the renderer's vocabulary without
// borrowing any of its object lifetimes, which is the one thing the isolation
// contract above actually cares about. They are built on synchronization2;
// that is not a new requirement, the instance is already created at
// VK_API_VERSION_1_3.
bool ReadBackPixels(VkImage image, uint32_t width, uint32_t height, VkImageLayout srcLayout) noexcept {
    // Acquire() waits for this slot's previous submission, resets its fence and
    // resets the pool, so the buffer is back in the initial state before the
    // guard below begins it.
    auto [slot, fence]      = g.ring.Acquire();
    const VkCommandBuffer cmd = slot;

    // Scoped so the buffer is closed before it is submitted: the guard ends it
    // on destruction. It does not report vkBegin/vkEndCommandBuffer failures --
    // on a freshly reset one-time-submit buffer those do not fail, and the
    // submit below is still checked, which is where a real failure shows up.
    {
        Vk::CommandBufferGuard recording(cmd);

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
                                      .mip_count  = 1, // mip 0 only; 0 would mean "all remaining"
                                  });
        };

        barrier(
            srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT
        );

        // Tightly packed: the helper sets bufferRowLength to the image width,
        // which is the same layout the staging buffer was sized for.
        Vk::CopyImageToBuffer(cmd, image, g.staging, VkExtent2D {width, height});

        barrier(
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
        );
    }

    const VkSubmitInfo si {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .waitSemaphoreCount   = 0,
        .pWaitSemaphores      = nullptr,
        .pWaitDstStageMask    = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores    = nullptr,
    };
    if (vkQueueSubmit(g.queue, 1, &si, fence) != VK_SUCCESS)
        return false;
    // Still waited on here rather than left to the next Acquire(): GlBlit reads
    // the mapped staging buffer immediately afterwards, in this same frame.
    return vkWaitForFences(g.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

// Pick a GL window with a usable context: the caller's window when it has
// one, otherwise a plugin-owned 2.1 window sized to the caller's window.
GLFWwindow* ResolveWindow(GLFWwindow* requested, uint32_t width, uint32_t height) noexcept {
    if (requested != nullptr && glfwGetWindowAttrib(requested, GLFW_CLIENT_API) != GLFW_NO_API) {
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
    // NOTE: glfwInit() also returns true when the ENGINE already initialized
    // GLFW, so this file must never assume ownership: glfwTerminate() would
    // destroy the engine's windows out from under it (the engine tears the
    // RenderContext — and with it this plugin — down BEFORE its Window).

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
    g.glWindow = glfwCreateWindow(fbW > 0 ? fbW : 1280, fbH > 0 ? fbH : 720, "Zahlen — Host Blit", nullptr, nullptr);
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

    // Set byte alignment dynamically
    glPixelStorei(GL_UNPACK_ALIGNMENT, (glFormat == GL_RGB) ? 1 : 4);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Direct window-space origin (Top-Left start position for inverted zoom)
    glWindowPos2i(0, fbH);

    // Scale to framebuffer + flip Y axis
    glPixelZoom(static_cast<float>(fbW) / static_cast<float>(width), -static_cast<float>(fbH) / static_cast<float>(height));

    // Pump host Vulkan memory straight to screen
    glDrawPixels(static_cast<int>(width), static_cast<int>(height), glFormat, glType, g.mapped);

    glPixelZoom(1.0f, 1.0f);
    // NOTE: no swap here — Present() owns the single per-frame swap. A second
    // glfwSwapBuffers would flip the never-drawn back buffer onto the screen,
    // which on macOS shows as an alternating black frame.
}

} // namespace

// ---------------------------------------------------------------------------
// Public surface (declarations mirrored in the banner comment above)
// ---------------------------------------------------------------------------
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

    // The ring builds the pool, the command buffer and a pre-signalled fence in
    // one call and reports which of them failed. It cleans itself up on the way
    // out, so there is nothing partial left to tear down here.
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

[[nodiscard]] bool Present(const ZHLN::Vk::Image& src, GLFWwindow* window, uint32_t width, uint32_t height, VkFormat format, VkImageLayout srcLayout) noexcept {
    if (!g.ready || !src.Valid() || width == 0 || height == 0)
        return false;

    GLFWwindow* target = ResolveWindow(window, width, height);
    if (target == nullptr)
        return false;
    // Keep the plugin window responsive. Harmless when the engine polls too
    // (events are delivered once); required when the plugin is the only
    // GLFW consumer (e.g. a TTY-mode engine session).
    glfwPollEvents();
    if (glfwWindowShouldClose(target))
        return false;

    // The plugin window was opened at the caller's framebuffer size and must
    // follow its resizes: GlBlit scales the source to whatever the window's
    // current framebuffer is, so a stale size squashes the frame into the
    // old aspect (the "stretched UI" symptom). Same points==pixels convention
    // as at creation. Skip the blit on the catch-up frame.
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

    // 4 bytes/px covers every supported format (RGB8 rows are ≤ RGBA8 size).
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
        // Releases the fence and the command pool. The move-assignment below
        // would reach the same place, but doing it here keeps the teardown
        // ordered against vkDeviceWaitIdle and readable in one spot.
        g.ring.Cleanup();
    }
    if (g.glWindow != nullptr)
        glfwDestroyWindow(g.glWindow);
    // Deliberately NO glfwTerminate(): GLFW may be (and in every engine
    // session IS) owned by the engine, which destroys its Window AFTER this
    // plugin. Terminating here would free the engine's window handles and
    // crash its teardown; at process exit the OS reclaims GLFW anyway.
    g = State {};
}

} // namespace ZHLN::HostBlit

#else

#include "HostBlit.hpp"

namespace ZHLN::HostBlit {

// The target remains available on every platform so build graphs and callers do
// not need platform-specific target checks. Native swapchain presentation is
// used everywhere except macOS, therefore these functions must be inert.
[[nodiscard]] bool Init(VkPhysicalDevice, VkDevice, VkQueue, uint32_t) noexcept {
    return false;
}

[[nodiscard]] bool Present(const ZHLN::Vk::Image&, GLFWwindow*, uint32_t, uint32_t, VkFormat, VkImageLayout) noexcept {
    return false;
}

void Shutdown() noexcept {
}

} // namespace ZHLN::HostBlit

#endif // defined(__APPLE__)
