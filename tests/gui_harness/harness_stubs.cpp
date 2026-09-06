// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link-time stubs for the host-only GUI harness.
//
// These satisfy symbols that live in the Engine / Window / Render half of the
// codebase, which the harness deliberately does not link (no Vulkan, no window,
// no GPU). Every stub below is on a code path the harness never executes:
//
//   * Engine::GetRegistry / Engine::GetWindow / Window::GetSize are only reached
//     from the Context(Engine&) constructor and from the viewport refresh in
//     BeginFrame. The harness uses Context(ECS::Registry&, Extent2D) instead, so
//     _impl->engine stays null and BeginFrame takes the `_impl->viewport` branch.
//   * RenderContext::SubmitUI is only reached from EndFrameAndRender. The harness
//     calls EndFrame(), which runs the same Clay_EndLayout() and stops before
//     vertex generation -- hit-testing happens in Button(), before either.
//   * InternalPanic / InternalWriteLog / LogManual are logging + assert plumbing.
//
// Nothing here participates in GUI layout or hit-testing: those come from the
// real src/gui/GUIContext.cpp and the real extern/clay/clay.h.

#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Window.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace ZHLN {

namespace {
[[noreturn]] void HarnessUnlinked(const char* what) {
    std::fprintf(stderr, "[gui_harness] FATAL: harness called %s, which lives in the unlinked Engine/render half.\n",
                 what);
    std::abort();
}
} // namespace

void InternalWriteLog(uint8_t /*channel*/, const char* file, uint32_t line, std::string_view message) {
    std::fprintf(stderr, "[gui_harness log] %s:%u %.*s\n", file ? file : "?", line, static_cast<int>(message.size()),
                 message.data());
}

void LogManual(std::string_view file, int line, std::string_view message, const char* /*color*/) {
    std::fprintf(stderr, "[gui_harness log] %.*s:%d %.*s\n", static_cast<int>(file.size()), file.data(), line,
                 static_cast<int>(message.size()), message.data());
}

[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message) {
    std::fprintf(stderr, "[gui_harness] PANIC %s:%u %.*s\n", file ? file : "?", line, static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

auto Engine::GetWindow() -> Window& {
    HarnessUnlinked("Engine::GetWindow()");
}

auto Engine::GetRegistry() -> ECS::Registry& {
    HarnessUnlinked("Engine::GetRegistry()");
}

auto Engine::GetRegistry() const -> const ECS::Registry& {
    HarnessUnlinked("Engine::GetRegistry() const");
}

Extent2D Window::GetSize() const {
    HarnessUnlinked("Window::GetSize()");
}

// The harness is strictly single-threaded (one thread drives the whole frame),
// so ZHLN::Mutex contention can never occur and the slow paths are unreachable.
// Real Mutex.cpp needs the fiber scheduler, which the harness does not link.
void Mutex::LockSlow() noexcept {}
void Mutex::UnlockSlow() noexcept {}

void RenderContext::SubmitUI(const UIBatch*, uint32_t, const VertexPosition*, const VertexAttributes*, uint32_t) noexcept {
    HarnessUnlinked("RenderContext::SubmitUI()");
}

} // namespace ZHLN
