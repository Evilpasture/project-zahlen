// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN {
struct WindowInputReceiver;
}

namespace ZHLN::TTYBackend {

bool IsSupported();

void* Init(uint32_t width, uint32_t height);

void Shutdown(void* context);

bool IsRunning(void* context);

void ProcessEvents(void* context, const WindowInputReceiver& receiver);

std::vector<std::string_view> GetRequiredInstanceExtensions();

void EmergencyRestore();

}
