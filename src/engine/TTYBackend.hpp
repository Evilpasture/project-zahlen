// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/TTYBackend.hpp
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace ZHLN {
class InputContext;
}

namespace ZHLN::TTYBackend {

// Probes if the system actually has a /dev/tty we can take over
bool IsSupported();

// Takes over the terminal, disables the blinking cursor, and sets up epoll
void* Init(uint32_t width, uint32_t height);

// Restores the terminal to text mode and cleans up
void Shutdown(void* context);

bool IsRunning(void* context);

void ProcessEvents(void* context, InputContext* input);

// Required extensions for VK_KHR_display
std::vector<std::string_view> GetRequiredInstanceExtensions();

// MUST be called by the crash handler to un-freeze the terminal on a segfault
void EmergencyRestore();

} // namespace ZHLN::TTYBackend
