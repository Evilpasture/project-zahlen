// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace ZHLN::COM {
struct IEngine;
struct IGraphicsDevice;
} // namespace ZHLN::COM

// The renderer owns readback. This extension owns encoding, file I/O, and the
// optional single-shot host capture layer.
namespace ZHLN::Screenshot {

// Reusable caller-owned storage for COM readback. Read once with a conservative
// extent * 8 buffer, rather than querying through a second GPU readback. A
// larger capture retries once with the reported byte count. No GPU cache.
// Calls must be serialized; a successful pointer lives until the next Read or
// destruction. Neither this object nor its returned frame owns the device.
class CaptureBuffer {
public:
    [[nodiscard]] auto Read(COM::IGraphicsDevice& graphics, Extent2D extent)
        -> std::expected<const CapturedFrame*, ErrorCode>;
private:
    CapturedFrame _frame;
};

// Write display-referred RGB as PNG, dropping alpha; returns false on an
// invalid frame, unsupported dimensions, encoding failure, or I/O failure.
[[nodiscard]] auto WritePNG(std::string_view path, const CapturedFrame& frame) -> bool;

enum class CaptureStatus : uint8_t {
    Pending,
    Succeeded,
    ReadFailed,
    WriteFailed,
};

struct CaptureOutcome {
    CaptureStatus status  = CaptureStatus::Pending;
    Extent2D      extent  = {};
    FrameCaptureMetrics metrics = {};
    ErrorCode     error   = {};

    [[nodiscard]] constexpr auto Succeeded() const noexcept -> bool {
        return status == CaptureStatus::Succeeded;
    }
    [[nodiscard]] constexpr auto Attempted() const noexcept -> bool {
        return status != CaptureStatus::Pending;
    }
};

// Host layer compatible with both COM::App and ZHLN::Application. Once
// `engine.CurrentFrame() >= afterFrames`, AfterTick reads back the primary
// window, writes a PNG to `path`, stores the outcome in caller-owned storage,
// and calls `engine.RequestClose()`. An empty path disables capture; an
// `afterFrames` value of zero clamps to one completed frame. If the engine
// closes before the target frame, `outcome.status` remains `Pending`.
class CaptureLayer {
public:
    CaptureLayer(std::string path, uint32_t afterFrames, CaptureOutcome& outcome) noexcept
        : _path(std::move(path))
        , _afterFrames(afterFrames ? afterFrames : 1u)
        , _outcome(&outcome) {}

    // Deferred binding for hosts whose CLI populates `path` and `afterFrames`
    // after AddLayer() and before Start(). Both objects must outlive Run().
    [[nodiscard]] static auto Bind(
        const std::string& path, const uint32_t& afterFrames, CaptureOutcome& outcome
    ) noexcept -> CaptureLayer {
        return CaptureLayer(&path, &afterFrames, &outcome);
    }
    static auto Bind(std::string&&, const uint32_t&, CaptureOutcome&) = delete;
    static auto Bind(const std::string&, uint32_t&&, CaptureOutcome&) = delete;
    static auto Bind(std::string&&, uint32_t&&, CaptureOutcome&)      = delete;

    void Start(COM::IEngine& engine) noexcept;
    void AfterTick(COM::IEngine& engine);

private:
    CaptureLayer(
        const std::string* path, const uint32_t* afterFrames, CaptureOutcome* outcome
    ) noexcept
        : _boundPath(path)
        , _boundAfterFrames(afterFrames)
        , _outcome(outcome) {}

    CaptureBuffer      _buffer;
    std::string        _path;
    uint32_t           _afterFrames      = 1;
    const std::string* _boundPath        = nullptr;
    const uint32_t*    _boundAfterFrames = nullptr;
    CaptureOutcome*    _outcome          = nullptr;
};

} // namespace ZHLN::Screenshot
