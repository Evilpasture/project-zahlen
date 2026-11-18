// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Image.hpp"
#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 4505)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
namespace ZHLN::Image {
namespace {
auto Check(std::span<const uint8_t> bytes) -> std::expected<void, Error> {
    if (bytes.empty())
        return std::unexpected(Error::InvalidArgument);
    if (bytes.size() > INT_MAX)
        return std::unexpected(Error::TooLarge);
    return {};
}
bool ValidPath(std::string_view path) {
    return !path.empty() && path.find('\0') == std::string_view::npos;
}
} // namespace
auto Inspect(std::span<const uint8_t> bytes) -> std::expected<Info, Error> {
    if (auto ok = Check(bytes); !ok)
        return std::unexpected(ok.error());
    int w = 0, h = 0, c = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c) || w <= 0 || h <= 0)
        return std::unexpected(Error::DecodeFailed);
    return Info {static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(c)};
}
auto IsHDR(std::span<const uint8_t> bytes) -> std::expected<bool, Error> {
    if (auto info = Inspect(bytes); !info)
        return std::unexpected(info.error());
    return stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size())) != 0;
}
auto DecodeRGBA8(std::span<const uint8_t> bytes) -> std::expected<RGBA8, Error> {
    if (auto ok = Check(bytes); !ok)
        return std::unexpected(ok.error());
    int                                                  w = 0, h = 0, c = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c, 4), &stbi_image_free
    );
    if (!data)
        return std::unexpected(Error::DecodeFailed);
    const uint64_t count = uint64_t(w) * uint64_t(h) * 4;
    if (count > std::vector<uint8_t>().max_size())
        return std::unexpected(Error::TooLarge);
    return RGBA8 {static_cast<uint32_t>(w), static_cast<uint32_t>(h), {data.get(), data.get() + static_cast<size_t>(count)}};
}
auto DecodeFloatRGBA(std::span<const uint8_t> bytes) -> std::expected<FloatRGBA, Error> {
    if (auto ok = Check(bytes); !ok)
        return std::unexpected(ok.error());
    int                                                w = 0, h = 0, c = 0;
    std::unique_ptr<float, decltype(&stbi_image_free)> data(
        stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c, 4), &stbi_image_free
    );
    if (!data)
        return std::unexpected(Error::DecodeFailed);
    const uint64_t count = uint64_t(w) * uint64_t(h) * 4;
    if (count > std::vector<float>().max_size())
        return std::unexpected(Error::TooLarge);
    return FloatRGBA {static_cast<uint32_t>(w), static_cast<uint32_t>(h), {data.get(), data.get() + static_cast<size_t>(count)}};
}
auto ReadRGBA8(std::string_view path) -> std::expected<RGBA8, Error> {
    if (!ValidPath(path))
        return std::unexpected(Error::InvalidArgument);
    std::ifstream in(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!in)
        return std::unexpected(Error::FileIO);
    const auto size = in.tellg();
    if (size <= 0 || size > INT_MAX)
        return std::unexpected(size > INT_MAX ? Error::TooLarge : Error::DecodeFailed);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::unexpected(Error::FileIO);
    return DecodeRGBA8(bytes);
}
auto EncodePNGToMemory(std::span<const uint8_t> pixels, uint32_t w, uint32_t h, uint32_t channels, uint32_t stride)
    -> std::expected<std::vector<uint8_t>, Error> {
    if (!w || !h || channels < 1 || channels > 4)
        return std::unexpected(Error::InvalidArgument);
    const uint64_t row   = uint64_t(w) * channels;
    const uint64_t pitch = stride ? stride : row;
    if (w > INT_MAX || h > INT_MAX || pitch > INT_MAX || uint64_t(h) * (row + 1) > INT_MAX)
        return std::unexpected(Error::TooLarge);
    if (pitch < row || uint64_t(h - 1) * pitch + row > pixels.size())
        return std::unexpected(Error::InvalidArgument);
    std::vector<uint8_t> output;
    const auto           append = [](void* context, void* data, int size) {
        auto&       out   = *static_cast<std::vector<uint8_t>*>(context);
        const auto* first = static_cast<const uint8_t*>(data);
        if (size > 0)
            out.insert(out.end(), first, first + size);
    };
    if (!stbi_write_png_to_func(append, &output, static_cast<int>(w), static_cast<int>(h), static_cast<int>(channels), pixels.data(), static_cast<int>(pitch)))
        return std::unexpected(Error::EncodeFailed);
    return output;
}
auto WritePNG(std::string_view path, std::span<const uint8_t> pixels, uint32_t w, uint32_t h, uint32_t c, uint32_t stride) -> std::expected<void, Error> {
    if (!ValidPath(path))
        return std::unexpected(Error::InvalidArgument);
    auto encoded = EncodePNGToMemory(pixels, w, h, c, stride);
    if (!encoded)
        return std::unexpected(encoded.error());
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected(Error::FileIO);
    out.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
    out.close();
    if (!out)
        return std::unexpected(Error::FileIO);
    return {};
}
} // namespace ZHLN::Image
