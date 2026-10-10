// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VTFDecoder.hpp"

#include <algorithm>
#include <cstring>

namespace ZHLN::BSP {

namespace {

#pragma pack(push, 1)
struct VTFHeader {
    char     type[4];    // "VTF\0"
    uint32_t version[2]; // [0] = major, [1] = minor
    uint32_t headerSize;
    uint16_t width;
    uint16_t height;
    uint32_t flags;
    uint16_t frames;
    uint16_t firstFrame;
    uint8_t  padding0[4];
    float    reflectivity[3];
    uint8_t  padding1[4];
    float    bumpmapScale;
    uint32_t highResImageFormat;
    uint8_t  mipmapCount;
    uint32_t lowResImageFormat;
    uint8_t  lowResImageWidth;
    uint8_t  lowResImageHeight;
};
#pragma pack(pop)

auto ReadU16LE(const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

auto ReadU32LE(const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                                 (static_cast<uint32_t>(p[3]) << 24));
}

void Unpack565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
    g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
    b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
}

void DecodeDXT1Block(const uint8_t* block, uint32_t bx, uint32_t by, uint32_t imgW, uint32_t imgH, uint8_t* dstRGBA) {
    const uint16_t c0 = ReadU16LE(block);
    const uint16_t c1 = ReadU16LE(block + 2);
    const uint32_t code = ReadU32LE(block + 4);

    uint8_t palette[4][4];
    Unpack565(c0, palette[0][0], palette[0][1], palette[0][2]);
    palette[0][3] = 255;

    Unpack565(c1, palette[1][0], palette[1][1], palette[1][2]);
    palette[1][3] = 255;

    if (c0 > c1) {
        palette[2][0] = static_cast<uint8_t>((2 * palette[0][0] + palette[1][0]) / 3);
        palette[2][1] = static_cast<uint8_t>((2 * palette[0][1] + palette[1][1]) / 3);
        palette[2][2] = static_cast<uint8_t>((2 * palette[0][2] + palette[1][2]) / 3);
        palette[2][3] = 255;

        palette[3][0] = static_cast<uint8_t>((palette[0][0] + 2 * palette[1][0]) / 3);
        palette[3][1] = static_cast<uint8_t>((palette[0][1] + 2 * palette[1][1]) / 3);
        palette[3][2] = static_cast<uint8_t>((palette[0][2] + 2 * palette[1][2]) / 3);
        palette[3][3] = 255;
    } else {
        palette[2][0] = static_cast<uint8_t>((palette[0][0] + palette[1][0]) / 2);
        palette[2][1] = static_cast<uint8_t>((palette[0][1] + palette[1][1]) / 2);
        palette[2][2] = static_cast<uint8_t>((palette[0][2] + palette[1][2]) / 2);
        palette[2][3] = 255;

        palette[3][0] = 0;
        palette[3][1] = 0;
        palette[3][2] = 0;
        palette[3][3] = 0;
    }

    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t px = bx + x;
            const uint32_t py = by + y;
            if (px < imgW && py < imgH) {
                const uint32_t idx = (code >> (2 * (y * 4 + x))) & 3;
                const size_t   dstOffset = (static_cast<size_t>(py) * imgW + px) * 4;
                dstRGBA[dstOffset + 0] = palette[idx][0];
                dstRGBA[dstOffset + 1] = palette[idx][1];
                dstRGBA[dstOffset + 2] = palette[idx][2];
                dstRGBA[dstOffset + 3] = palette[idx][3];
            }
        }
    }
}

void DecodeDXT5Block(const uint8_t* block, uint32_t bx, uint32_t by, uint32_t imgW, uint32_t imgH, uint8_t* dstRGBA) {
    // 1. Decode Alpha (first 8 bytes)
    const uint8_t a0 = block[0];
    const uint8_t a1 = block[1];

    uint8_t alphaPalette[8];
    alphaPalette[0] = a0;
    alphaPalette[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i) {
            alphaPalette[i] = static_cast<uint8_t>(((8 - i) * a0 + (i - 1) * a1) / 7);
        }
    } else {
        for (int i = 2; i < 6; ++i) {
            alphaPalette[i] = static_cast<uint8_t>(((6 - i) * a0 + (i - 1) * a1) / 5);
        }
        alphaPalette[6] = 0;
        alphaPalette[7] = 255;
    }

    uint64_t alphaBits = 0;
    for (int i = 0; i < 6; ++i) {
        alphaBits |= (static_cast<uint64_t>(block[2 + i]) << (i * 8));
    }

    // 2. Decode Color (last 8 bytes)
    const uint8_t* colorBlock = block + 8;
    const uint16_t c0 = ReadU16LE(colorBlock);
    const uint16_t c1 = ReadU16LE(colorBlock + 2);
    const uint32_t code = ReadU32LE(colorBlock + 4);

    uint8_t colorPalette[4][3];
    Unpack565(c0, colorPalette[0][0], colorPalette[0][1], colorPalette[0][2]);
    Unpack565(c1, colorPalette[1][0], colorPalette[1][1], colorPalette[1][2]);
    colorPalette[2][0] = static_cast<uint8_t>((2 * colorPalette[0][0] + colorPalette[1][0]) / 3);
    colorPalette[2][1] = static_cast<uint8_t>((2 * colorPalette[0][1] + colorPalette[1][1]) / 3);
    colorPalette[2][2] = static_cast<uint8_t>((2 * colorPalette[0][2] + colorPalette[1][2]) / 3);
    colorPalette[3][0] = static_cast<uint8_t>((colorPalette[0][0] + 2 * colorPalette[1][0]) / 3);
    colorPalette[3][1] = static_cast<uint8_t>((colorPalette[0][1] + 2 * colorPalette[1][1]) / 3);
    colorPalette[3][2] = static_cast<uint8_t>((colorPalette[0][2] + 2 * colorPalette[1][2]) / 3);

    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t px = bx + x;
            const uint32_t py = by + y;
            if (px < imgW && py < imgH) {
                const uint32_t pixelIdx = y * 4 + x;
                const uint32_t cIdx = (code >> (2 * pixelIdx)) & 3;
                const uint32_t aIdx = static_cast<uint32_t>((alphaBits >> (3 * pixelIdx)) & 7);

                const size_t dstOffset = (static_cast<size_t>(py) * imgW + px) * 4;
                dstRGBA[dstOffset + 0] = colorPalette[cIdx][0];
                dstRGBA[dstOffset + 1] = colorPalette[cIdx][1];
                dstRGBA[dstOffset + 2] = colorPalette[cIdx][2];
                dstRGBA[dstOffset + 3] = alphaPalette[aIdx];
            }
        }
    }
}

auto ComputeMipDataSize(uint32_t w, uint32_t h, VTFImageFormat format) -> size_t {
    w = std::max(1u, w);
    h = std::max(1u, h);
    switch (format) {
        case VTFImageFormat::DXT1: {
            const uint32_t bw = std::max(1u, (w + 3) / 4);
            const uint32_t bh = std::max(1u, (h + 3) / 4);
            return static_cast<size_t>(bw) * bh * 8;
        }
        case VTFImageFormat::DXT3:
        case VTFImageFormat::DXT5: {
            const uint32_t bw = std::max(1u, (w + 3) / 4);
            const uint32_t bh = std::max(1u, (h + 3) / 4);
            return static_cast<size_t>(bw) * bh * 16;
        }
        case VTFImageFormat::RGB888:
        case VTFImageFormat::BGR888:
            return static_cast<size_t>(w) * h * 3;
        case VTFImageFormat::RGBA8888:
        case VTFImageFormat::ABGR8888:
        case VTFImageFormat::BGRA8888:
        case VTFImageFormat::BGRX8888:
            return static_cast<size_t>(w) * h * 4;
        default:
            return static_cast<size_t>(w) * h * 4;
    }
}

} // namespace

auto DecodeVTF(std::span<const std::byte> vtfBytes) -> std::expected<DecodedImage, ErrorCode> {
    if (vtfBytes.size() < sizeof(VTFHeader)) {
        return std::unexpected(VTFError::InvalidMagic);
    }

    const auto* header = reinterpret_cast<const VTFHeader*>(vtfBytes.data());
    if (std::memcmp(header->type, "VTF\0", 4) != 0) {
        return std::unexpected(VTFError::InvalidMagic);
    }

    const uint32_t width  = header->width;
    const uint32_t height = header->height;
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        return std::unexpected(VTFError::InvalidDimensions);
    }

    const auto     format      = static_cast<VTFImageFormat>(header->highResImageFormat);
    const uint8_t  mipmapCount = std::max(uint8_t {1}, header->mipmapCount);
    const uint16_t frames      = std::max(uint16_t {1}, header->frames);

    // Compute offset to mipmap 0
    size_t offset = header->headerSize;

    // Skip low-res thumbnail if present
    if (header->lowResImageWidth > 0 && header->lowResImageHeight > 0 &&
        static_cast<int32_t>(header->lowResImageFormat) != -1) {
        const auto lowResFmt = static_cast<VTFImageFormat>(header->lowResImageFormat);
        offset += ComputeMipDataSize(header->lowResImageWidth, header->lowResImageHeight, lowResFmt);
    }

    // Skip smaller mipmaps (VTF stores mipmaps in ascending order from 1x1 up to WxH)
    for (int mip = mipmapCount - 1; mip > 0; --mip) {
        const uint32_t mw = std::max(1u, width >> mip);
        const uint32_t mh = std::max(1u, height >> mip);
        const size_t   mipSize = ComputeMipDataSize(mw, mh, format);
        offset += mipSize * frames;
    }

    const size_t mip0Size = ComputeMipDataSize(width, height, format);
    if (offset + mip0Size > vtfBytes.size()) {
        return std::unexpected(VTFError::OutOfBounds);
    }

    const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(vtfBytes.data()) + offset;
    DecodedImage   img;
    img.width  = width;
    img.height = height;
    img.rgba8.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = reinterpret_cast<uint8_t*>(img.rgba8.data());

    switch (format) {
        case VTFImageFormat::DXT1: {
            const uint32_t bw = (width + 3) / 4;
            const uint32_t bh = (height + 3) / 4;
            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    const uint8_t* block = srcBytes + (static_cast<size_t>(by) * bw + bx) * 8;
                    DecodeDXT1Block(block, bx * 4, by * 4, width, height, dst);
                }
            }
            break;
        }
        case VTFImageFormat::DXT5: {
            const uint32_t bw = (width + 3) / 4;
            const uint32_t bh = (height + 3) / 4;
            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    const uint8_t* block = srcBytes + (static_cast<size_t>(by) * bw + bx) * 16;
                    DecodeDXT5Block(block, bx * 4, by * 4, width, height, dst);
                }
            }
            break;
        }
        case VTFImageFormat::RGBA8888: {
            std::memcpy(dst, srcBytes, static_cast<size_t>(width) * height * 4);
            break;
        }
        case VTFImageFormat::BGRA8888:
        case VTFImageFormat::BGRX8888: {
            const size_t totalPixels = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < totalPixels; ++i) {
                dst[i * 4 + 0] = srcBytes[i * 4 + 2]; // R <- B
                dst[i * 4 + 1] = srcBytes[i * 4 + 1]; // G <- G
                dst[i * 4 + 2] = srcBytes[i * 4 + 0]; // B <- R
                dst[i * 4 + 3] = (format == VTFImageFormat::BGRX8888) ? 255 : srcBytes[i * 4 + 3];
            }
            break;
        }
        case VTFImageFormat::RGB888: {
            const size_t totalPixels = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < totalPixels; ++i) {
                dst[i * 4 + 0] = srcBytes[i * 3 + 0];
                dst[i * 4 + 1] = srcBytes[i * 3 + 1];
                dst[i * 4 + 2] = srcBytes[i * 3 + 2];
                dst[i * 4 + 3] = 255;
            }
            break;
        }
        case VTFImageFormat::BGR888: {
            const size_t totalPixels = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < totalPixels; ++i) {
                dst[i * 4 + 0] = srcBytes[i * 3 + 2]; // R <- B
                dst[i * 4 + 1] = srcBytes[i * 3 + 1]; // G
                dst[i * 4 + 2] = srcBytes[i * 3 + 0]; // B <- R
                dst[i * 4 + 3] = 255;
            }
            break;
        }
        default:
            return std::unexpected(VTFError::UnsupportedFormat);
    }

    return img;
}

} // namespace ZHLN::BSP
