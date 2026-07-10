// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/legacy/PassCache.hpp
#pragma once

#include <array>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk11 {

// Helper to hash-combine values (matches your PHI constant)
template <class T>
inline void HashCombine(std::size_t& seed, const T& v) {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct RenderPassKey {
    std::vector<VkFormat>            colorFormats;
    std::vector<VkAttachmentLoadOp>  colorLoadOps;
    std::vector<VkAttachmentStoreOp> colorStoreOps;
    VkFormat                         depthFormat  = VK_FORMAT_UNDEFINED;
    VkAttachmentLoadOp               depthLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp              depthStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

    bool operator==(const RenderPassKey& other) const {
        return colorFormats == other.colorFormats && colorLoadOps == other.colorLoadOps && colorStoreOps == other.colorStoreOps &&
               depthFormat == other.depthFormat && depthLoadOp == other.depthLoadOp && depthStoreOp == other.depthStoreOp;
    }
};

struct FramebufferKey {
    VkRenderPass             renderPass = VK_NULL_HANDLE;
    std::vector<VkImageView> attachments;
    uint32_t                 width  = 0;
    uint32_t                 height = 0;

    bool operator==(const FramebufferKey& other) const {
        return renderPass == other.renderPass && attachments == other.attachments && width == other.width && height == other.height;
    }
};

struct RenderPassKeyHash {
    std::size_t operator()(const RenderPassKey& key) const {
        std::size_t seed = 0;
        for (auto f: key.colorFormats)
            HashCombine(seed, static_cast<int>(f));
        for (auto o: key.colorLoadOps)
            HashCombine(seed, static_cast<int>(o));
        for (auto o: key.colorStoreOps)
            HashCombine(seed, static_cast<int>(o));
        HashCombine(seed, static_cast<int>(key.depthFormat));
        HashCombine(seed, static_cast<int>(key.depthLoadOp));
        HashCombine(seed, static_cast<int>(key.depthStoreOp));
        return seed;
    }
};

struct FramebufferKeyHash {
    std::size_t operator()(const FramebufferKey& key) const {
        std::size_t seed = 0;
        HashCombine(seed, reinterpret_cast<std::uintptr_t>(key.renderPass));
        for (auto v: key.attachments)
            HashCombine(seed, reinterpret_cast<std::uintptr_t>(v));
        HashCombine(seed, key.width);
        HashCombine(seed, key.height);
        return seed;
    }
};

class PassCache {
  public:
    static void Init(VkDevice device) {
        _device = device;
    }

    static void Cleanup() {
        if (_device != VK_NULL_HANDLE) {
            for (auto& [key, rp]: _renderPasses) {
                vkDestroyRenderPass(_device, rp, nullptr);
            }
            for (auto& [key, fb]: _framebuffers) {
                vkDestroyFramebuffer(_device, fb, nullptr);
            }
            _renderPasses.clear();
            _framebuffers.clear();
            _device = VK_NULL_HANDLE;
        }
    }

    static auto GetOrCreateRenderPass(const RenderPassKey& key) -> VkRenderPass {
        auto it = _renderPasses.find(key);
        if (it != _renderPasses.end()) {
            return it->second;
        }

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference>   colorRefs;
        VkAttachmentReference                depthRef {};

        for (size_t i = 0; i < key.colorFormats.size(); ++i) {
            attachments.push_back(
                {.flags          = 0,
                 .format         = key.colorFormats[i],
                 .samples        = VK_SAMPLE_COUNT_1_BIT,
                 .loadOp         = key.colorLoadOps[i],
                 .storeOp        = key.colorStoreOps[i],
                 .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
            );
            colorRefs.push_back({.attachment = static_cast<uint32_t>(attachments.size() - 1), .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        }

        const bool hasDepth = (key.depthFormat != VK_FORMAT_UNDEFINED);
        if (hasDepth) {
            attachments.push_back(
                {.flags          = 0,
                 .format         = key.depthFormat,
                 .samples        = VK_SAMPLE_COUNT_1_BIT,
                 .loadOp         = key.depthLoadOp,
                 .storeOp        = key.depthStoreOp,
                 .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}
            );
            depthRef = {.attachment = static_cast<uint32_t>(attachments.size() - 1), .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        }

        VkSubpassDescription subpass {
            .flags                   = 0,
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputAttachmentCount    = 0,
            .pInputAttachments       = nullptr,
            .colorAttachmentCount    = static_cast<uint32_t>(colorRefs.size()),
            .pColorAttachments       = colorRefs.empty() ? nullptr : colorRefs.data(),
            .pResolveAttachments     = nullptr,
            .pDepthStencilAttachment = hasDepth ? &depthRef : nullptr,
            .preserveAttachmentCount = 0,
            .pPreserveAttachments    = nullptr
        };

        std::array<VkSubpassDependency, 2> dependencies {};
        dependencies[0] = {
            .srcSubpass      = VK_SUBPASS_EXTERNAL,
            .dstSubpass      = 0,
            .srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask   = 0,
            .dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
        };
        if (hasDepth) {
            dependencies[1] = {
                .srcSubpass      = VK_SUBPASS_EXTERNAL,
                .dstSubpass      = 0,
                .srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                .dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                .srcAccessMask   = 0,
                .dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
            };
        }

        VkRenderPassCreateInfo createInfo {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .attachmentCount = static_cast<uint32_t>(attachments.size()),
            .pAttachments    = attachments.data(),
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .dependencyCount = hasDepth ? 2u : 1u,
            .pDependencies   = dependencies.data()
        };

        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(_device, &createInfo, nullptr, &rp);

        _renderPasses[key] = rp;
        return rp;
    }

    static auto GetOrCreateFramebuffer(const FramebufferKey& key) -> VkFramebuffer {
        auto it = _framebuffers.find(key);
        if (it != _framebuffers.end()) {
            return it->second;
        }

        VkFramebufferCreateInfo createInfo {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .renderPass      = key.renderPass,
            .attachmentCount = static_cast<uint32_t>(key.attachments.size()),
            .pAttachments    = key.attachments.data(),
            .width           = key.width,
            .height          = key.height,
            .layers          = 1
        };

        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(_device, &createInfo, nullptr, &fb);

        _framebuffers[key] = fb;
        return fb;
    }

  private:
    inline static VkDevice                                                              _device = VK_NULL_HANDLE;
    inline static std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash>    _renderPasses;
    inline static std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> _framebuffers;
};

} // namespace ZHLN::Vk11
