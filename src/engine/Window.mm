// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/Window.hpp>
#import <AppKit/AppKit.h>

namespace ZHLN {

void Window::Focus() {
    // Native Objective-C syntax for focus
    [NSApp activateIgnoringOtherApps:YES];
}

} // namespace ZHLN