#ifdef __APPLE__
#define VK_USE_PLATFORM_METAL_EXT
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../RenderCore.hpp"
#include "DemoWindow.hpp"

// Delegate interface
@interface ZHLNDemoDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, assign) ZHLN::Demo::WindowState* state;
@end

@implementation ZHLNDemoDelegate
- (void)windowWillClose:(NSNotification *)notification {
    if (self.state) self.state->running = false;
    [NSApp stop:nil];
}
- (void)windowDidResize:(NSNotification *)notification {
    if (!self.state) return;
    NSWindow* win = [notification object];
    NSView* view = [win contentView];
    CAMetalLayer* layer = (CAMetalLayer*)[view layer];
    
    CGFloat scale = [win backingScaleFactor];
    CGSize size = [view bounds].size;
    layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
    
    self.state->width = (uint32_t)(size.width * scale);
    self.state->height = (uint32_t)(size.height * scale);
    self.state->resized = true;
}
@end

namespace ZHLN::Demo {

WindowState InitWindow(uint32_t width, uint32_t height, const char* title) {
    WindowState state;
    state.width = width;
    state.height = height;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    
    ZHLNDemoDelegate* delegate = [[ZHLNDemoDelegate alloc] init];
    delegate.state = &state; // WARNING: Dangerous in real apps, fine for this synchronous demo
    [NSApp setDelegate:delegate];
    
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
        
    [window setTitle:[NSString stringWithUTF8String:title]];
    [window setDelegate:delegate];
    
    NSView* view = [window contentView];
    [view setWantsLayer:YES];
    CAMetalLayer* metalLayer = [CAMetalLayer layer];
	[view setLayer:metalLayer];

	[window setAcceptsMouseMovedEvents:YES];
    
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [window makeFirstResponder:[window contentView]];
    [NSApp finishLaunching];
    
    state.os_window = (__bridge void*)window;
    state.metal_layer = (__bridge void*)metalLayer;
    return state;
}

void ProcessEvents(WindowState& state) {
    @autoreleasepool {
        NSEvent* event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            
            if (event.type == NSEventTypeMouseMoved || event.type == NSEventTypeLeftMouseDragged) {
                NSWindow* win = [event window];
                if (win) {
                    NSPoint loc = [event locationInWindow];
                    CGFloat scale = [win backingScaleFactor];
                    state.mouse_x = (float)(loc.x * scale);
                    // Invert Y: Cocoa is Bottom-Left, Vulkan is Top-Left
                    state.mouse_y = (float)(state.height - (loc.y * scale)); 
                }
            }

            [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }
}

void DestroyWindow(WindowState& state) {
    // macOS NSWindow closes automatically
}

std::vector<const char*> GetRequiredInstanceExtensions() {
    return { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME };
}

VkSurfaceKHR CreateSurface(VkInstance instance, const WindowState& state) {
    VkMetalSurfaceCreateInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = static_cast<const CAMetalLayer*>(state.metal_layer)
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vkCreateMetalSurfaceEXT(instance, &info, nullptr, &surface);
    return surface;
}

} // namespace ZHLN::Demo
#endif