// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <cstdint>
#include <Zahlen/Core/Platform.hpp>

namespace ZHLN {
class Window;
}

// Engine-internal (src/engine/Platform.hpp is on the engine's private include
// path), but the hosts in app/ and utils/ call Init() to bring the platform up
// before an Engine exists, so the definition is exported.
namespace ZHLN::Platform {

void SetHighPriority();

ZHLN_API void Init();

void FocusWindow(Window& window);

float GetDisplayScale(Window& window);

void Sleep(uint32_t milliseconds);

}
