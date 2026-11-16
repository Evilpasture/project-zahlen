// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IEngine.hpp
//
// The root of the public API. One object implements this and every capability
// interface, so Graphics() and Query<IGraphicsDevice>() reach the same
// subobject of the same allocation and a capability costs nothing to obtain.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/COM/HostTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>

namespace ZHLN::COM {

struct IGraphicsDevice;
struct IEntities;
struct ISimulation;
struct IAudioDevice;
struct IAssetLibrary;
struct IActionContext;
struct IState;
struct IDebugDraw;

struct IEngine : IObject {
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;
    virtual void               ProcessEvents() noexcept = 0;
    // Asks the platform host to close. IsRunning() reports false from the
    // next ProcessEvents(); nothing else a consumer can call ever made it
    // false, so without this no COM-driven loop could terminate.
    virtual void               RequestClose() noexcept = 0;
    [[nodiscard]] virtual Result<void> Tick(float dt) noexcept = 0;
    virtual void                       ClearScene() noexcept = 0;

    // Borrowed, not retained. These are the same object as this IEngine, so
    // they live exactly as long as the reference you already hold and must not
    // be Released. Use Query<T>() / Ref<T>::As<T>() when you want to own one
    // independently; that path adds a reference the way COM expects.
    [[nodiscard]] virtual IGraphicsDevice* Graphics() noexcept = 0;
    [[nodiscard]] virtual IEntities*       Entities() noexcept = 0;
    [[nodiscard]] virtual ISimulation*     Simulation() noexcept = 0;
    [[nodiscard]] virtual IAudioDevice*    Audio() noexcept = 0;
    [[nodiscard]] virtual IAssetLibrary*   Assets() noexcept = 0;
    [[nodiscard]] virtual IActionContext*  Actions() noexcept = 0;
    [[nodiscard]] virtual IState*          State() noexcept = 0;

    [[nodiscard]] virtual IDebugDraw* Debug() noexcept = 0;

    // Zero FPS means uncapped. The frame and generation counters are reads,
    // not a way for a host to advance engine or device state.
    virtual void SetMaxFramesPerSecond(uint32_t fps) noexcept = 0;
    [[nodiscard]] virtual uint32_t MaxFramesPerSecond() const noexcept = 0;
    [[nodiscard]] virtual uint64_t CurrentFrame() const noexcept = 0;
    [[nodiscard]] virtual uint64_t DeviceGeneration() const noexcept = 0;

    // Registration tokens belong to this engine. Null callbacks are rejected;
    // unknown/zero tokens are inert on removal. Callbacks run on the recovery
    // caller's thread. Registrations added during notification wait until the
    // next recovery; removals take effect before any later callback runs.
    [[nodiscard]] virtual Result<uint64_t> AddDeviceLostCallback(void (*callback)(void*), void* userdata) noexcept = 0;
    virtual void RemoveDeviceLostCallback(uint64_t token) noexcept = 0;

    // --- windows -----------------------------------------------------------
    //
    // A consumer addresses a window by WindowID, never by a Window*: the engine
    // owns windows, adds and removes them while it runs, and a pointer taken
    // from one call is not valid for the next. WindowID::Primary always exists;
    // the rest are minted by AddWindow.
    [[nodiscard]] virtual WindowID PrimaryWindow() const noexcept = 0;

    // Writes up to `capacity` ids in window order (primary first) and returns
    // the total count, so a caller sizes its buffer from the first answer. Same
    // shape as IEntities::EntitiesWithRaw; nothing is allocated on either side
    // of the boundary.
    [[nodiscard]] virtual uint32_t Windows(WindowID* outWindows, uint32_t capacity) const noexcept = 0;

    // The drawable extent of a window. A zero extent means the window is
    // minimized or not presentable; drawing into it is a no-op, not an error.
    [[nodiscard]] virtual Extent2D WindowExtent(WindowID window) const noexcept = 0;

    // The window's keyboard/mouse focus, which is what a consumer needs to
    // decide whether its own input belongs to this window.
    [[nodiscard]] virtual bool IsWindowFocused(WindowID window) const noexcept = 0;

    [[nodiscard]] virtual Result<WindowID> AddWindow(const ChainInNode* windowChain) noexcept = 0;
    virtual void RemoveWindow(WindowID window) noexcept = 0;
    [[nodiscard]] virtual bool IsWindowRunning(WindowID window) const noexcept = 0;
    virtual void CloseWindow(WindowID window) noexcept = 0;
    virtual void FocusWindow(WindowID window) noexcept = 0;
    virtual void SetCursorMode(WindowID window, CursorMode mode) noexcept = 0;
    [[nodiscard]] virtual CursorMode GetCursorMode(WindowID window) const noexcept = 0;
    virtual void CaptureMouse(WindowID window, bool captured) noexcept = 0;
    [[nodiscard]] virtual bool IsMouseCaptured(WindowID window) const noexcept = 0;
    [[nodiscard]] virtual Result<void> SetClipboardText(StringView text) noexcept = 0;
    // Exact bytes, no NUL. Null/zero queries size; short buffers are untouched.
    [[nodiscard]] virtual Result<uint32_t> ClipboardText(char* buffer, uint32_t capacity) const noexcept = 0;
    // Views and their nested spans live only during callback. Null clears it.
    [[nodiscard]] virtual Result<void> SetFileDropCallback(WindowID window, FileDropCallback callback, void* userdata) noexcept = 0;
    // Physical snapshot; consume clears only this window's queued UI keys/text
    // after successful chain validation. Mouse/button state is never consumed.
    [[nodiscard]] virtual Result<void> ReadHostInput(WindowID window, ChainOutNode* output, bool consume) noexcept = 0;
    // Supported chain: UISubmitInfo. Validates and copies all geometry now.
    [[nodiscard]] virtual Result<void> SubmitUI(WindowID window, const ChainInNode* uiChain) noexcept = 0;

    [[nodiscard]] auto AddWindow(const WindowCreateInfo& info) noexcept -> Result<WindowID> { return AddWindow(InChain(info)); }
    [[nodiscard]] auto SubmitUI(WindowID window, const UISubmitInfo& info) noexcept -> Result<void> { return SubmitUI(window, InChain(info)); }

protected:
    ~IEngine() = default;
};

} // namespace ZHLN::COM
