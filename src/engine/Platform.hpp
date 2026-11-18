// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <cstdint>
#include <Zahlen/Core/Platform.hpp>

namespace ZHLN {
class Window;
}

// Engine-internal platform helpers. Platform::Init() is declared ZHLN_API in
// <Zahlen/Core/Platform.hpp> (included above) so hosts in app/ and utils/ can
// bring the platform up before creating an IEngine; everything below stays hidden.
namespace ZHLN::Platform {

void SetHighPriority();

void FocusWindow(Window& window);

float GetDisplayScale(Window& window);

void Sleep(uint32_t milliseconds);

}
