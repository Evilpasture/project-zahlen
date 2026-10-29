// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <cstdint>
#include <Zahlen/Core/Platform.hpp>

namespace ZHLN {
class Window;
}

namespace ZHLN::Platform {

void SetHighPriority();

void Init();

void FocusWindow(Window& window);

float GetDisplayScale(Window& window);

void Sleep(uint32_t milliseconds);

}
