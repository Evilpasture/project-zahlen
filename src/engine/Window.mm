#include "engine/Window.hpp"
#import <AppKit/AppKit.h>

namespace ZHLN {

void Window::Focus() {
    // Native Objective-C syntax for focus
    [NSApp activateIgnoringOtherApps:YES];
}

} // namespace ZHLN