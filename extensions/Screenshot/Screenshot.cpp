// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Screenshot.hpp"
#include <Image/Image.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

auto ZHLN::Screenshot::CaptureBuffer::Read(COM::IGraphicsDevice& graphics, Extent2D extent)
    -> std::expected<const CapturedFrame*, ErrorCode> {
    _frame.extent = {};
    _frame.metrics = {};
    const uint64_t texels = uint64_t(extent.width) * extent.height;
    if (!texels || texels > _frame.pixels.max_size() / 8) return std::unexpected(COM::Error::InvalidArgument);
    const size_t initial = static_cast<size_t>(texels * 8);
    // Keep high-water storage after a resize; never do a null size query.
    _frame.pixels.resize(std::max(initial, _frame.pixels.size()));
    COM::CaptureInfo info;
    auto copied = graphics.CaptureFrame(COM::OutChain(info), _frame.pixels);
    if (!copied.HasValue()) return std::unexpected(copied.error);
    if (copied.value > _frame.pixels.size()) {
        if (copied.value > _frame.pixels.max_size()) return std::unexpected(COM::Error::InvalidArgument);
        _frame.pixels.resize(static_cast<size_t>(copied.value));
        copied = graphics.CaptureFrame(COM::OutChain(info), _frame.pixels);
        if (!copied.HasValue()) return std::unexpected(copied.error);
    }
    if (copied.value > _frame.pixels.size() || copied.value != info.byteCount)
        return std::unexpected(COM::Error::InvalidArgument);
    _frame.pixels.resize(static_cast<size_t>(copied.value));
    _frame.extent = info.extent;
    _frame.format = info.format;
    _frame.metrics = info.metrics;
    if (!_frame.Valid()) return std::unexpected(COM::Error::InvalidArgument);
    return &_frame;
}

auto ZHLN::Screenshot::WritePNG(std::string_view path, const ZHLN::CapturedFrame& frame) -> bool {
    if (path.empty() || !frame.Valid() ||
        frame.extent.width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 3) ||
        frame.extent.height > static_cast<uint32_t>(std::numeric_limits<int>::max())) return false;

    // PNG's RGB bytes are display referred; apply the same HDR conversion as
    // tests/helpers/HeadlessEngineFixture.hpp when a wide capture is returned.
    std::vector<uint8_t> rgb(frame.PixelCount() * 3);
    for (size_t i = 0; i < frame.PixelCount(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            if (frame.format == ZHLN::PixelFormat::Rgba16F) {
                uint16_t half;
                std::memcpy(&half, frame.pixels.data() + i * 8 + c * 2, sizeof(half));
                const float linear = ZHLN::Math::HalfToFloat(half);
                const float toned = ZHLN::Math::AcesFilm(linear * ZHLN::kCaptureExposure);
                rgb[i * 3 + c] = static_cast<uint8_t>((std::isfinite(toned) ? std::clamp(toned, 0.0f, 1.0f) : 0.0f) * 255.0f);
            } else {
                rgb[i * 3 + c] = frame.pixels[i * frame.BytesPerPixel() + c];
            }
        }
    }

    return ZHLN::Image::WritePNG(path, rgb, frame.extent.width, frame.extent.height, 3).has_value();
}

void ZHLN::Screenshot::CaptureLayer::Start(COM::IEngine&) noexcept {
    if (_outcome != nullptr) *_outcome = {};
}

void ZHLN::Screenshot::CaptureLayer::AfterTick(COM::IEngine& engine) {
    if (_outcome == nullptr || _outcome->Attempted()) return;
    const std::string_view path = _boundPath != nullptr ? std::string_view(*_boundPath) : std::string_view(_path);
    if (path.empty()) return;
    const uint32_t configured = _boundAfterFrames != nullptr ? *_boundAfterFrames : _afterFrames;
    const uint32_t target     = configured ? configured : 1u;
    if (engine.CurrentFrame() < target) return;

    auto* graphics = engine.Graphics();
    if (graphics == nullptr) {
        *_outcome = CaptureOutcome {.status = CaptureStatus::ReadFailed, .error = COM::Error::NullArgument};
        engine.RequestClose();
        return;
    }

    const auto frame = _buffer.Read(*graphics, engine.WindowExtent(engine.PrimaryWindow()));
    if (!frame) {
        *_outcome = CaptureOutcome {.status = CaptureStatus::ReadFailed, .error = frame.error()};
    } else {
        *_outcome = CaptureOutcome {
            .status  = WritePNG(path, **frame) ? CaptureStatus::Succeeded : CaptureStatus::WriteFailed,
            .extent  = (*frame)->extent,
            .metrics = (*frame)->metrics,
        };
    }
    engine.RequestClose();
}
