#pragma once

#include "RenderCore.h"

struct HeadlessCtx {
    VkInstance              instance = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo physical = {};
    ZHLN_Device             device   = {};

    bool valid() const { return device.handle != VK_NULL_HANDLE; }
};

HeadlessCtx MakeHeadlessCtx();
