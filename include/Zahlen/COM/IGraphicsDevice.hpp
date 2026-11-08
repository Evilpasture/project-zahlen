// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IGraphicsDevice.hpp
//
// The renderer as the public API sees it. Every method is noexcept and every type in
// every signature is fixed layout, which AuditInterfaceABI checks at compile
// time when EngineCOM declares this as one of its interfaces.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>

#include <cstddef>
#include <cstdint>

namespace ZHLN::COM {

struct IGraphicsDevice : IObject {
    virtual void SetResolution(Extent2D resolution) noexcept = 0;

    [[nodiscard]] virtual ViewportRect Viewport() const noexcept = 0;
    [[nodiscard]] virtual uint32_t     FrameIndex() const noexcept = 0;

    // BufferID::Invalid on failure: the engine's CreateBuffer has no error
    // channel of its own, so inventing a Result here would promise a
    // distinction the implementation cannot make.
    [[nodiscard]] virtual BufferID CreateBuffer(BufferUsage usage, Span<const std::byte> data) noexcept = 0;
    virtual void                   DestroyBuffer(BufferID handle) noexcept = 0;
    [[nodiscard]] virtual Result<void> UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept = 0;

    [[nodiscard]] virtual Result<TextureID> CreateTexture(StringView name, Span<const std::byte> rgba, Extent2D extent,
                                                          bool isSRGB) noexcept = 0;
    virtual void                            UnloadTexture(TextureID handle) noexcept = 0;

protected:
    ~IGraphicsDevice() = default;
};

} // namespace ZHLN::COM
