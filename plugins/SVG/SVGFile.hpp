// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Opt-in host-side filesystem helpers for ZHLN::SVG.
//
// Core SVG rasterization (<SVG/SVG.hpp> and ISvgRasterizer) is strictly
// in-memory / VFS-only and never touches the OS filesystem. Include this header
// only from host tools, CLI utilities, or tests that explicitly read SVG or
// font files from OS paths.

#include "SVG.hpp"
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace ZHLN::SVG {

[[nodiscard]] inline auto ReadHostBytes(std::string_view path) noexcept -> std::expected<std::vector<uint8_t>, ErrorCode> {
    if (path.empty()) {
        return std::unexpected(Error::FileOpenFailed);
    }
    const std::filesystem::path fsPath(path);
    std::error_code             ec;
    if (!std::filesystem::is_regular_file(fsPath, ec) || ec) {
        return std::unexpected(Error::FileOpenFailed);
    }
    std::ifstream in(fsPath, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::unexpected(Error::FileOpenFailed);
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return std::unexpected(Error::FileOpenFailed);
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(Error::FileOpenFailed);
    }
    return bytes;
}

[[nodiscard]] inline auto LoadFontBytes(std::string_view path) noexcept -> std::expected<std::vector<uint8_t>, ErrorCode> {
    auto bytes = ReadHostBytes(path);
    if (!bytes || bytes->empty()) {
        return std::unexpected(Error::FontLoadFailed);
    }
    return bytes;
}

[[nodiscard]] inline auto LoadFile(const Rasterizer& rasterizer, std::string_view path) noexcept -> std::expected<Document, ErrorCode> {
    auto bytes = ReadHostBytes(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return rasterizer.LoadData(*bytes);
}

[[nodiscard]] inline auto LoadFile(std::string_view path, const Options& options = {}) noexcept -> std::expected<Document, ErrorCode> {
    const Rasterizer rasterizer(options);
    return LoadFile(rasterizer, path);
}

[[nodiscard]] inline auto RasterizeFile(
    const Rasterizer& rasterizer,
    std::string_view  path,
    uint32_t          width,
    uint32_t          height,
    FitMode           fit   = FitMode::Contain,
    AlphaMode         alpha = AlphaMode::Straight
) noexcept -> std::expected<Raster, ErrorCode> {
    auto document = LoadFile(rasterizer, path);
    if (!document) {
        return std::unexpected(document.error());
    }
    return document->RenderFitted(width, height, fit, alpha);
}

[[nodiscard]] inline auto RasterizeFile(
    std::string_view path,
    uint32_t         width,
    uint32_t         height,
    FitMode          fit     = FitMode::Contain,
    AlphaMode        alpha   = AlphaMode::Straight,
    const Options&   options = {}
) noexcept -> std::expected<Raster, ErrorCode> {
    const Rasterizer rasterizer(options);
    return RasterizeFile(rasterizer, path, width, height, fit, alpha);
}

} // namespace ZHLN::SVG
