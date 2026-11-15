// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IEngine.hpp
//
// The root of the public API. One object implements this and every capability
// interface, so Graphics() and Query<IGraphicsDevice>() reach the same
// subobject of the same allocation and a capability costs nothing to obtain.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>

namespace ZHLN::COM {

struct IGraphicsDevice;
struct IEntities;
struct ISimulation;
struct IAudioDevice;
struct IAssetLibrary;
struct IActionContext;
struct IState;

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

protected:
    ~IEngine() = default;
};

} // namespace ZHLN::COM
