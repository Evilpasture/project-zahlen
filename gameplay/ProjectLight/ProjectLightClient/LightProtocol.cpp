// gameplay/ProjectLight/ProjectLightClient/LightProtocol.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ProjectLightClient/LightProtocol.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
#include <zlib.h>

namespace ZHLN::ProjectLight {
namespace {

constexpr std::array<std::string_view, PROPERTY_CHANNEL_COUNT> kPropertyNames = {
    "Name",
    "Parent",
    "Aspect",
    "ProjectionMode",
    "OrthographicSize",
    "NearPlane",
    "FrustumCulling",
    "LightFrustumCulling",
    "MotionBlur",
    "MotionBlurStrength",
    "MotionBlurQuality",
    "Position",
    "CurrentCamera",
    "Ambient",
    "TimeOfDay",
    "Latitude",
    "ProceduralSky",
    "ProceduralClouds",
    "MinCloudHeight",
    "MaxCloudHeight",
    "CloudDensity",
    "SkyStarCount",
    "GodRaysDensity",
    "GodRaysIntensity",
    "Part1",
    "Part2",
    "Offset1",
    "Offset2",
    "RotationOffset1",
    "RotationOffset2",
    "ServerAuthority",
    "UserId",
    "Character",
    "Rotation",
    "Size",
    "Anchored",
    "CanCollide",
    "Color",
    "Transparency",
    "Shiny",
    "Reflection",
    "Metallic",
    "Roughness",
    "Refraction",
    "RefractionIndex",
    "Emission",
    "NetworkOwner",
    "Shape",
    "Beveled",
    "BevelSize",
    "SmoothBevelNormals",
    "FrontSurface",
    "BackSurface",
    "TopSurface",
    "BottomSurface",
    "LeftSurface",
    "RightSurface",
    "MeshId",
    "MeshScale",
    "SoundId",
    "Volume",
    "Loops",
    "Playing",
    "RootPart",
    "State",
    "CurrentAngle",
    "DesiredAngle",
    "MaxVelocity",
    "Source",
    "PrimaryPart",
    "Enabled",
    "Intensity",
    "Shadows",
    "ShadowRadius",
    "SourceRadius",
    "Direction",
    "NightColor",
    "NightIntensity",
    "IsMain",
    "Range",
    "Falloff",
    "TextureId",
    "Face",
    "WrapMode",
    "Offset",
    "Scale",
    "ResampleMode",
    "Mode",
    "Adornee",
    "WorldAligned",
    "ShaftLength",
    "HeadLength",
    "Radius",
    "HeadRadius",
    "Width",
    "Order",
    "ParentShader",
    "PassTarget",
    "PassScale",
    "PassBlend",
    "PassRepeat",
    "Shader",
    "Parameters",
    "ClassName"
};

void AppendByte(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void AppendBE16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void AppendBE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void AppendBE64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

auto ReadBE16(std::span<const uint8_t> bytes, size_t& cursor) -> std::optional<uint16_t> {
    if (cursor + 2 > bytes.size()) {
        return std::nullopt;
    }
    const uint16_t value = (static_cast<uint16_t>(bytes[cursor]) << 8) | static_cast<uint16_t>(bytes[cursor + 1]);
    cursor += 2;
    return value;
}

auto ReadBE32(std::span<const uint8_t> bytes, size_t& cursor) -> std::optional<uint32_t> {
    if (cursor + 4 > bytes.size()) {
        return std::nullopt;
    }
    const uint32_t value = (static_cast<uint32_t>(bytes[cursor]) << 24) | (static_cast<uint32_t>(bytes[cursor + 1]) << 16) |
                           (static_cast<uint32_t>(bytes[cursor + 2]) << 8) | static_cast<uint32_t>(bytes[cursor + 3]);
    cursor += 4;
    return value;
}

auto ReadBE64(std::span<const uint8_t> bytes, size_t& cursor) -> std::optional<uint64_t> {
    if (cursor + 8 > bytes.size()) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[cursor + i];
    }
    cursor += 8;
    return value;
}

void EncodeLength(std::vector<uint8_t>& out, size_t count, uint8_t fixBase, size_t fixMax, uint8_t code8, uint8_t code16, uint8_t code32) {
    if (count <= fixMax) {
        out.push_back(static_cast<uint8_t>(fixBase | static_cast<uint8_t>(count)));
    } else if (code8 != 0 && count <= std::numeric_limits<uint8_t>::max()) {
        out.push_back(code8);
        out.push_back(static_cast<uint8_t>(count));
    } else if (count <= std::numeric_limits<uint16_t>::max()) {
        out.push_back(code16);
        AppendBE16(out, static_cast<uint16_t>(count));
    } else {
        out.push_back(code32);
        AppendBE32(out, static_cast<uint32_t>(count));
    }
}

void EncodeValue(const MsgPackValue& value, std::vector<uint8_t>& out, unsigned depth) {
    if (depth > 64) {
        out.push_back(0xC0);
        return;
    }

    switch (value.kind) {
        case MsgPackValue::Kind::Nil:
            out.push_back(0xC0);
            return;
        case MsgPackValue::Kind::Bool:
            out.push_back(value.boolVal ? 0xC3 : 0xC2);
            return;
        case MsgPackValue::Kind::Int: {
            const int64_t number = value.intVal;
            if (number >= 0) {
                const auto positive = static_cast<uint64_t>(number);
                if (positive <= 0x7F) {
                    out.push_back(static_cast<uint8_t>(positive));
                } else if (positive <= 0xFF) {
                    out.push_back(0xCC);
                    AppendByte(out, static_cast<uint8_t>(positive));
                } else if (positive <= 0xFFFF) {
                    out.push_back(0xCD);
                    AppendBE16(out, static_cast<uint16_t>(positive));
                } else if (positive <= 0xFFFFFFFFull) {
                    out.push_back(0xCE);
                    AppendBE32(out, static_cast<uint32_t>(positive));
                } else {
                    out.push_back(0xCF);
                    AppendBE64(out, positive);
                }
            } else if (number >= -32) {
                out.push_back(static_cast<uint8_t>(number));
            } else if (number >= std::numeric_limits<int8_t>::min()) {
                out.push_back(0xD0);
                AppendByte(out, static_cast<uint8_t>(number));
            } else if (number >= std::numeric_limits<int16_t>::min()) {
                out.push_back(0xD1);
                AppendBE16(out, static_cast<uint16_t>(number));
            } else if (number >= std::numeric_limits<int32_t>::min()) {
                out.push_back(0xD2);
                AppendBE32(out, static_cast<uint32_t>(number));
            } else {
                out.push_back(0xD3);
                AppendBE64(out, static_cast<uint64_t>(number));
            }
            return;
        }
        case MsgPackValue::Kind::UInt: {
            const uint64_t number = value.uintVal;
            if (number <= 0x7F) {
                out.push_back(static_cast<uint8_t>(number));
            } else if (number <= 0xFF) {
                out.push_back(0xCC);
                AppendByte(out, static_cast<uint8_t>(number));
            } else if (number <= 0xFFFF) {
                out.push_back(0xCD);
                AppendBE16(out, static_cast<uint16_t>(number));
            } else if (number <= 0xFFFFFFFFull) {
                out.push_back(0xCE);
                AppendBE32(out, static_cast<uint32_t>(number));
            } else {
                out.push_back(0xCF);
                AppendBE64(out, number);
            }
            return;
        }
        case MsgPackValue::Kind::Float: {
            const double number = value.floatVal;
            out.push_back(0xCB);
            AppendBE64(out, std::bit_cast<uint64_t>(number));
            return;
        }
        case MsgPackValue::Kind::String: {
            const size_t count = value.strVal.size();
            if (count <= 31) {
                out.push_back(static_cast<uint8_t>(0xA0 | count));
            } else if (count <= 0xFF) {
                out.push_back(0xD9);
                AppendByte(out, static_cast<uint8_t>(count));
            } else if (count <= 0xFFFF) {
                out.push_back(0xDA);
                AppendBE16(out, static_cast<uint16_t>(count));
            } else {
                out.push_back(0xDB);
                AppendBE32(out, static_cast<uint32_t>(count));
            }
            out.insert(out.end(), value.strVal.begin(), value.strVal.end());
            return;
        }
        case MsgPackValue::Kind::Binary: {
            const size_t count = value.binVal.size();
            if (count <= 0xFF) {
                out.push_back(0xC4);
                AppendByte(out, static_cast<uint8_t>(count));
            } else if (count <= 0xFFFF) {
                out.push_back(0xC5);
                AppendBE16(out, static_cast<uint16_t>(count));
            } else {
                out.push_back(0xC6);
                AppendBE32(out, static_cast<uint32_t>(count));
            }
            out.insert(out.end(), value.binVal.begin(), value.binVal.end());
            return;
        }
        case MsgPackValue::Kind::Array:
            EncodeLength(out, value.arrayVal.size(), 0x90, 15, 0, 0xDC, 0xDD);
            for (const MsgPackValue& entry: value.arrayVal) {
                EncodeValue(entry, out, depth + 1);
            }
            return;
        case MsgPackValue::Kind::Map:
            EncodeLength(out, value.mapVal.size(), 0x80, 15, 0, 0xDE, 0xDF);
            for (const auto& [key, entry]: value.mapVal) {
                EncodeValue(key, out, depth + 1);
                EncodeValue(entry, out, depth + 1);
            }
            return;
    }
}

class MsgPackReader {
  public:
    explicit MsgPackReader(std::span<const uint8_t> bytes): m_bytes(bytes) {
    }

    auto Read() -> std::expected<MsgPackValue, std::string> {
        auto value = ReadValue(0);
        if (!value) {
            return value;
        }
        if (m_cursor != m_bytes.size()) {
            return std::unexpected("MessagePack payload contains trailing bytes");
        }
        return value;
    }

  private:
    [[nodiscard]] auto Need(size_t count) const -> bool {
        return count <= m_bytes.size() - std::min(m_cursor, m_bytes.size());
    }

    auto ReadLength(uint8_t code, size_t fixMask, uint8_t code8, uint8_t code16, uint8_t code32) -> std::expected<size_t, std::string> {
        if (fixMask != 0 && (code & 0xF0) == fixMask) {
            return static_cast<size_t>(code & 0x0F);
        }
        if (code == code8) {
            if (!Need(1)) {
                return std::unexpected("truncated MessagePack length");
            }
            return static_cast<size_t>(m_bytes[m_cursor++]);
        }
        if (code == code16) {
            auto length = ReadBE16(m_bytes, m_cursor);
            if (!length) {
                return std::unexpected("truncated MessagePack length");
            }
            return static_cast<size_t>(*length);
        }
        if (code == code32) {
            auto length = ReadBE32(m_bytes, m_cursor);
            if (!length) {
                return std::unexpected("truncated MessagePack length");
            }
            return static_cast<size_t>(*length);
        }
        return std::unexpected("invalid MessagePack length code");
    }

    auto ReadText(size_t count) -> std::expected<MsgPackValue, std::string> {
        if (!Need(count)) {
            return std::unexpected("truncated MessagePack string");
        }
        MsgPackValue value = MsgPackValue::String(std::string_view(reinterpret_cast<const char*>(m_bytes.data() + m_cursor), count));
        m_cursor += count;
        return value;
    }

    auto ReadBinary(size_t count) -> std::expected<MsgPackValue, std::string> {
        if (!Need(count)) {
            return std::unexpected("truncated MessagePack binary blob");
        }
        MsgPackValue value = MsgPackValue::Binary(m_bytes.subspan(m_cursor, count));
        m_cursor += count;
        return value;
    }

    auto ReadValue(unsigned depth) -> std::expected<MsgPackValue, std::string> {
        if (depth > 64) {
            return std::unexpected("MessagePack nesting exceeds 64 levels");
        }
        if (!Need(1)) {
            return std::unexpected("truncated MessagePack value");
        }
        const uint8_t code = m_bytes[m_cursor++];

        if (code <= 0x7F) {
            return MsgPackValue::UInt(code);
        }
        if (code >= 0xE0) {
            return MsgPackValue::Int(static_cast<int8_t>(code));
        }
        if ((code & 0xE0) == 0xA0) {
            return ReadText(code & 0x1F);
        }
        if ((code & 0xF0) == 0x90) {
            const size_t count = code & 0x0F;
            if (count > m_bytes.size() - m_cursor) {
                return std::unexpected("MessagePack array count exceeds remaining bytes");
            }
            MsgPackValue value = MsgPackValue::Array();
            value.arrayVal.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                auto entry = ReadValue(depth + 1);
                if (!entry) {
                    return entry;
                }
                value.arrayVal.push_back(std::move(*entry));
            }
            return value;
        }
        if ((code & 0xF0) == 0x80) {
            const size_t count = code & 0x0F;
            if (count > (m_bytes.size() - m_cursor) / 2) {
                return std::unexpected("MessagePack map count exceeds remaining bytes");
            }
            MsgPackValue value = MsgPackValue::Map();
            value.mapVal.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                auto key = ReadValue(depth + 1);
                if (!key) {
                    return key;
                }
                auto entry = ReadValue(depth + 1);
                if (!entry) {
                    return entry;
                }
                value.mapVal.emplace_back(std::move(*key), std::move(*entry));
            }
            return value;
        }

        switch (code) {
            case 0xC0:
                return MsgPackValue::Nil();
            case 0xC2:
                return MsgPackValue::Bool(false);
            case 0xC3:
                return MsgPackValue::Bool(true);
            case 0xC4: {
                if (!Need(1)) {
                    return std::unexpected("truncated MessagePack binary length");
                }
                return ReadBinary(m_bytes[m_cursor++]);
            }
            case 0xC5: {
                auto length = ReadBE16(m_bytes, m_cursor);
                if (!length) {
                    return std::unexpected("truncated MessagePack binary length");
                }
                return ReadBinary(*length);
            }
            case 0xC6: {
                auto length = ReadBE32(m_bytes, m_cursor);
                if (!length) {
                    return std::unexpected("truncated MessagePack binary length");
                }
                return ReadBinary(*length);
            }
            case 0xCA: {
                auto bits = ReadBE32(m_bytes, m_cursor);
                if (!bits) {
                    return std::unexpected("truncated MessagePack float32");
                }
                return MsgPackValue::Float(std::bit_cast<float>(*bits));
            }
            case 0xCB: {
                auto bits = ReadBE64(m_bytes, m_cursor);
                if (!bits) {
                    return std::unexpected("truncated MessagePack float64");
                }
                return MsgPackValue::Float(std::bit_cast<double>(*bits));
            }
            case 0xCC:
                if (!Need(1)) {
                    return std::unexpected("truncated MessagePack uint8");
                }
                return MsgPackValue::UInt(m_bytes[m_cursor++]);
            case 0xCD: {
                auto value = ReadBE16(m_bytes, m_cursor);
                if (!value) {
                    return std::unexpected("truncated MessagePack uint16");
                }
                return MsgPackValue::UInt(*value);
            }
            case 0xCE: {
                auto value = ReadBE32(m_bytes, m_cursor);
                if (!value) {
                    return std::unexpected("truncated MessagePack uint32");
                }
                return MsgPackValue::UInt(*value);
            }
            case 0xCF: {
                auto value = ReadBE64(m_bytes, m_cursor);
                if (!value) {
                    return std::unexpected("truncated MessagePack uint64");
                }
                return MsgPackValue::UInt(*value);
            }
            case 0xD0: {
                if (!Need(1)) {
                    return std::unexpected("truncated MessagePack int8");
                }
                return MsgPackValue::Int(static_cast<int8_t>(m_bytes[m_cursor++]));
            }
            case 0xD1: {
                auto value = ReadBE16(m_bytes, m_cursor);
                if (!value) {
                    return std::unexpected("truncated MessagePack int16");
                }
                return MsgPackValue::Int(static_cast<int16_t>(*value));
            }
            case 0xD2: {
                auto value = ReadBE32(m_bytes, m_cursor);
                if (!value) {
                    return std::unexpected("truncated MessagePack int32");
                }
                return MsgPackValue::Int(static_cast<int32_t>(*value));
            }
            case 0xD3: {
                auto value = ReadBE64(m_bytes, m_cursor);
                if (!value) {
                    return std::unexpected("truncated MessagePack int64");
                }
                return MsgPackValue::Int(static_cast<int64_t>(*value));
            }
            case 0xD9: {
                if (!Need(1)) {
                    return std::unexpected("truncated MessagePack string length");
                }
                return ReadText(m_bytes[m_cursor++]);
            }
            case 0xDA: {
                auto length = ReadBE16(m_bytes, m_cursor);
                if (!length) {
                    return std::unexpected("truncated MessagePack string length");
                }
                return ReadText(*length);
            }
            case 0xDB: {
                auto length = ReadBE32(m_bytes, m_cursor);
                if (!length) {
                    return std::unexpected("truncated MessagePack string length");
                }
                return ReadText(*length);
            }
            case 0xDC:
            case 0xDD: {
                std::optional<uint32_t> length;
                if (code == 0xDC) {
                    auto shortLength = ReadBE16(m_bytes, m_cursor);
                    if (shortLength) {
                        length = *shortLength;
                    }
                } else {
                    length = ReadBE32(m_bytes, m_cursor);
                }
                if (!length) {
                    return std::unexpected("truncated MessagePack array length");
                }
                const auto count = static_cast<size_t>(*length);
                if (count > m_bytes.size() - m_cursor) {
                    return std::unexpected("MessagePack array count exceeds remaining bytes");
                }
                MsgPackValue value = MsgPackValue::Array();
                value.arrayVal.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    auto entry = ReadValue(depth + 1);
                    if (!entry) {
                        return entry;
                    }
                    value.arrayVal.push_back(std::move(*entry));
                }
                return value;
            }
            case 0xDE:
            case 0xDF: {
                std::optional<uint32_t> length;
                if (code == 0xDE) {
                    auto shortLength = ReadBE16(m_bytes, m_cursor);
                    if (shortLength) {
                        length = *shortLength;
                    }
                } else {
                    length = ReadBE32(m_bytes, m_cursor);
                }
                if (!length) {
                    return std::unexpected("truncated MessagePack map length");
                }
                const auto count = static_cast<size_t>(*length);
                if (count > (m_bytes.size() - m_cursor) / 2) {
                    return std::unexpected("MessagePack map count exceeds remaining bytes");
                }
                MsgPackValue value = MsgPackValue::Map();
                value.mapVal.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    auto key = ReadValue(depth + 1);
                    if (!key) {
                        return key;
                    }
                    auto entry = ReadValue(depth + 1);
                    if (!entry) {
                        return entry;
                    }
                    value.mapVal.emplace_back(std::move(*key), std::move(*entry));
                }
                return value;
            }
            default:
                return std::unexpected("unsupported or reserved MessagePack type code");
        }
    }

    std::span<const uint8_t> m_bytes;
    size_t                   m_cursor = 0;
};

auto HasUriScheme(std::string_view url) noexcept -> bool {
    const size_t delimiter = url.find("://");
    if (delimiter == std::string_view::npos || delimiter == 0) {
        return false;
    }
    const auto isAlpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (!isAlpha(url.front())) {
        return false;
    }
    for (size_t i = 1; i < delimiter; ++i) {
        const char c = url[i];
        if (!isAlpha(c) && (c < '0' || c > '9') && c != '+' && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

auto HasLaunchScheme(std::string_view url, std::string_view scheme) noexcept -> bool {
    if (scheme.ends_with("://")) {
        scheme.remove_suffix(3);
    }
    if (scheme.empty() || url.size() < scheme.size() + 3) {
        return false;
    }
    for (size_t i = 0; i < scheme.size(); ++i) {
        const auto c     = static_cast<unsigned char>(url[i]);
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
        if (lower != scheme[i]) {
            return false;
        }
    }
    return url.substr(scheme.size(), 3) == "://";
}

auto PercentDecode(std::string_view text) -> std::string {
    std::string output;
    output.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            output.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f') {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F') {
                    return c - 'A' + 10;
                }
                return -1;
            };
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                output.push_back(text[i]);
            }
        } else {
            output.push_back(text[i]);
        }
    }
    return output;
}

auto ParseUnsigned(std::string_view text, uint64_t& output) -> bool {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
    return error == std::errc {} && end == text.data() + text.size();
}

auto ParsePort(std::string_view text, uint16_t& output) -> bool {
    uint64_t value = 0;
    if (!ParseUnsigned(text, value) || value == 0 || value > 65535) {
        return false;
    }
    output = static_cast<uint16_t>(value);
    return true;
}

} // namespace

// ============================================================================
// Property channel lookup
// ============================================================================

auto PropertyChannelName(uint16_t channel) noexcept -> std::string_view {
    return channel < kPropertyNames.size() ? kPropertyNames[channel] : std::string_view {};
}

auto PropertyNameChannel(std::string_view name) noexcept -> std::optional<uint16_t> {
    for (size_t i = 0; i < kPropertyNames.size(); ++i) {
        if (kPropertyNames[i] == name) {
            return static_cast<uint16_t>(i);
        }
    }
    return std::nullopt;
}

// ============================================================================
// zlib
// ============================================================================

auto InflateZlib(std::span<const uint8_t> compressed, size_t maxOutputBytes) -> std::expected<std::vector<uint8_t>, std::string> {
    if (compressed.empty()) {
        return std::unexpected("empty zlib stream");
    }
    if (compressed.size() > std::numeric_limits<uInt>::max()) {
        return std::unexpected("compressed stream exceeds zlib input limit");
    }

    z_stream stream {};
    stream.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit(&stream) != Z_OK) {
        return std::unexpected("zlib inflateInit failed");
    }

    std::vector<uint8_t> output;
    output.reserve(std::min(maxOutputBytes, std::max<size_t>(4096, compressed.size() * 2)));
    std::array<uint8_t, static_cast<size_t>(64 * 1024)> chunk {};
    int                                                 status = Z_OK;
    while (status == Z_OK) {
        stream.next_out       = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out      = static_cast<uInt>(chunk.size());
        status                = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = chunk.size() - stream.avail_out;
        if (produced > maxOutputBytes - std::min(maxOutputBytes, output.size())) {
            inflateEnd(&stream);
            return std::unexpected("decompressed stream exceeds configured limit");
        }
        output.insert(output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        if (status == Z_BUF_ERROR && stream.avail_in == 0) {
            break;
        }
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::unexpected("invalid zlib stream");
        }
    }

    const bool complete = status == Z_STREAM_END;
    const bool trailing = stream.avail_in != 0;
    inflateEnd(&stream);
    if (!complete) {
        return std::unexpected("truncated zlib stream");
    }
    if (trailing) {
        return std::unexpected("zlib stream has trailing bytes");
    }
    return output;
}

auto DeflateZlib(std::span<const uint8_t> raw) -> std::expected<std::vector<uint8_t>, std::string> {
    if (raw.size() > std::numeric_limits<uLong>::max()) {
        return std::unexpected("uncompressed stream exceeds zlib input limit");
    }
    const auto           sourceLength = static_cast<uLong>(raw.size());
    const uLongf         bound        = compressBound(sourceLength);
    std::vector<uint8_t> output(static_cast<size_t>(bound));
    uLongf               outputLength = bound;
    const int            status       = compress2(output.data(), &outputLength, raw.data(), sourceLength, 3);
    if (status != Z_OK) {
        return std::unexpected("zlib compress2 failed");
    }
    output.resize(static_cast<size_t>(outputLength));
    return output;
}

// ============================================================================
// MessagePack value constructors / accessors
// ============================================================================

auto MsgPackValue::Nil() -> MsgPackValue {
    return {};
}
auto MsgPackValue::Bool(bool value) -> MsgPackValue {
    MsgPackValue result;
    result.kind    = Kind::Bool;
    result.boolVal = value;
    return result;
}
auto MsgPackValue::Int(int64_t value) -> MsgPackValue {
    MsgPackValue result;
    result.kind   = Kind::Int;
    result.intVal = value;
    return result;
}
auto MsgPackValue::UInt(uint64_t value) -> MsgPackValue {
    MsgPackValue result;
    result.kind    = Kind::UInt;
    result.uintVal = value;
    return result;
}
auto MsgPackValue::Float(double value) -> MsgPackValue {
    MsgPackValue result;
    result.kind     = Kind::Float;
    result.floatVal = value;
    return result;
}
auto MsgPackValue::String(std::string_view value) -> MsgPackValue {
    MsgPackValue result;
    result.kind = Kind::String;
    result.strVal.assign(value);
    return result;
}
auto MsgPackValue::Binary(std::span<const uint8_t> value) -> MsgPackValue {
    MsgPackValue result;
    result.kind = Kind::Binary;
    result.binVal.assign(value.begin(), value.end());
    return result;
}
auto MsgPackValue::Array(std::vector<MsgPackValue> value) -> MsgPackValue {
    MsgPackValue result;
    result.kind     = Kind::Array;
    result.arrayVal = std::move(value);
    return result;
}
auto MsgPackValue::Map(std::vector<std::pair<MsgPackValue, MsgPackValue>> value) -> MsgPackValue {
    MsgPackValue result;
    result.kind   = Kind::Map;
    result.mapVal = std::move(value);
    return result;
}

auto MsgPackValue::AsBool(bool fallback) const noexcept -> bool {
    if (kind == Kind::Bool) {
        return boolVal;
    }
    if (kind == Kind::Int) {
        return intVal != 0;
    }
    if (kind == Kind::UInt) {
        return uintVal != 0;
    }
    return fallback;
}

auto MsgPackValue::AsInt(int64_t fallback) const noexcept -> int64_t {
    if (kind == Kind::Int) {
        return intVal;
    }
    if (kind == Kind::UInt && uintVal <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(uintVal);
    }
    if (kind == Kind::Float && std::isfinite(floatVal) && floatVal >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
        floatVal <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(floatVal);
    }
    return fallback;
}

auto MsgPackValue::AsUInt(uint64_t fallback) const noexcept -> uint64_t {
    if (kind == Kind::UInt) {
        return uintVal;
    }
    if (kind == Kind::Int && intVal >= 0) {
        return static_cast<uint64_t>(intVal);
    }
    if (kind == Kind::Float && std::isfinite(floatVal) && floatVal >= 0.0 && floatVal <= static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        return static_cast<uint64_t>(floatVal);
    }
    return fallback;
}

auto MsgPackValue::AsDouble(double fallback) const noexcept -> double {
    if (kind == Kind::Float) {
        return floatVal;
    }
    if (kind == Kind::Int) {
        return static_cast<double>(intVal);
    }
    if (kind == Kind::UInt) {
        return static_cast<double>(uintVal);
    }
    return fallback;
}

auto MsgPackValue::AsFloat(float fallback) const noexcept -> float {
    const double value = AsDouble(std::numeric_limits<double>::quiet_NaN());
    return std::isfinite(value) ? static_cast<float>(value) : fallback;
}

auto MsgPackValue::AsString(std::string_view fallback) const -> std::string {
    return kind == Kind::String ? strVal : std::string(fallback);
}

auto MsgPackValue::AsUid() const noexcept -> uint64_t {
    if (kind == Kind::UInt) {
        return uintVal;
    }
    if (kind == Kind::Int && intVal >= 0) {
        return static_cast<uint64_t>(intVal);
    }
    if (kind == Kind::String) {
        uint64_t value = 0;
        if (ParseUnsigned(strVal, value)) {
            return value;
        }
    }
    return 0;
}

auto MsgPackValue::AsVec3(float integerScale, JPH::Vec3 fallback) const noexcept -> JPH::Vec3 {
    if (kind != Kind::Array || arrayVal.size() < 3) {
        return fallback;
    }
    const bool  scaled  = arrayVal[0].kind == Kind::Int || arrayVal[0].kind == Kind::UInt;
    const float divisor = (scaled && integerScale != 0.0f) ? integerScale : 1.0f;
    const float x       = arrayVal[0].AsFloat(fallback.GetX()) / divisor;
    const float y       = arrayVal[1].AsFloat(fallback.GetY()) / divisor;
    const float z       = arrayVal[2].AsFloat(fallback.GetZ()) / divisor;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return fallback;
    }
    return {x, y, z};
}

auto MsgPackValue::Find(std::string_view key) const noexcept -> const MsgPackValue* {
    if (kind != Kind::Map) {
        return nullptr;
    }
    for (const auto& [entryKey, value]: mapVal) {
        if (entryKey.kind == Kind::String && entryKey.strVal == key) {
            return &value;
        }
    }
    return nullptr;
}

auto MsgPackValue::FindIntKey(int64_t key) const noexcept -> const MsgPackValue* {
    if (kind != Kind::Map) {
        return nullptr;
    }
    for (const auto& [entryKey, value]: mapVal) {
        if (entryKey.kind == Kind::Int && entryKey.intVal == key) {
            return &value;
        }
        if (entryKey.kind == Kind::UInt && key >= 0 && entryKey.uintVal == static_cast<uint64_t>(key)) {
            return &value;
        }
    }
    return nullptr;
}

auto MsgPackValue::FindProperty(uint16_t channel, std::string_view propertyName) const noexcept -> const MsgPackValue* {
    if (const MsgPackValue* numeric = FindIntKey(channel)) {
        return numeric;
    }
    return propertyName.empty() ? nullptr : Find(propertyName);
}

void MsgPackValue::Set(std::string_view key, MsgPackValue value) {
    if (kind != Kind::Map) {
        kind = Kind::Map;
        mapVal.clear();
    }
    for (auto& [entryKey, entryValue]: mapVal) {
        if (entryKey.kind == Kind::String && entryKey.strVal == key) {
            entryValue = std::move(value);
            return;
        }
    }
    mapVal.emplace_back(String(key), std::move(value));
}

void MsgPackValue::SetIntKey(int64_t key, MsgPackValue value) {
    if (kind != Kind::Map) {
        kind = Kind::Map;
        mapVal.clear();
    }
    for (auto& [entryKey, entryValue]: mapVal) {
        if ((entryKey.kind == Kind::Int && entryKey.intVal == key) ||
            (key >= 0 && entryKey.kind == Kind::UInt && entryKey.uintVal == static_cast<uint64_t>(key))) {
            entryValue = std::move(value);
            return;
        }
    }
    mapVal.emplace_back(Int(key), std::move(value));
}

// ============================================================================
// MessagePack encoding / decoding
// ============================================================================

auto EncodeMsgPack(const MsgPackValue& value) -> std::vector<uint8_t> {
    std::vector<uint8_t> output;
    EncodeValue(value, output, 0);
    return output;
}

auto DecodeMsgPack(std::span<const uint8_t> bytes) -> std::expected<MsgPackValue, std::string> {
    return MsgPackReader(bytes).Read();
}

// ============================================================================
// Stream framing
// ============================================================================

auto EncodePayloadBody(const MsgPackValue& payload, bool compress) -> std::expected<std::vector<uint8_t>, std::string> {
    std::vector<uint8_t> raw = EncodeMsgPack(payload);
    if (raw.empty() || raw.size() > MAX_STREAM_FRAME_BYTES) {
        return std::unexpected("MessagePack payload exceeds stream-frame limit");
    }
    std::vector<uint8_t> body;
    body.reserve(raw.size() + 1);
    body.push_back(0);
    if (compress && raw.size() >= COMPRESSION_MIN_BYTES) {
        auto compressed = DeflateZlib(raw);
        if (!compressed) {
            return std::unexpected(compressed.error());
        }
        if (compressed->size() + 32 < raw.size()) {
            body[0] = FRAME_FLAG_COMPRESSED;
            body.insert(body.end(), compressed->begin(), compressed->end());
            return body;
        }
    }
    body.insert(body.end(), raw.begin(), raw.end());
    return body;
}

auto DecodePayloadBody(std::span<const uint8_t> body, size_t maxOutputBytes) -> std::expected<MsgPackValue, std::string> {
    if (body.empty()) {
        return std::unexpected("empty stream frame body");
    }
    if ((body[0] & ~FRAME_FLAG_COMPRESSED) != 0) {
        return std::unexpected("stream frame contains unsupported flags");
    }
    std::vector<uint8_t>     inflated;
    std::span<const uint8_t> packed = body.subspan(1);
    if ((body[0] & FRAME_FLAG_COMPRESSED) != 0) {
        auto result = InflateZlib(packed, maxOutputBytes);
        if (!result) {
            return std::unexpected(result.error());
        }
        inflated = std::move(*result);
        packed   = inflated;
    }
    auto value = DecodeMsgPack(packed);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!value->IsMap()) {
        return std::unexpected("stream payload must be a MessagePack map");
    }
    return value;
}

auto EncodeStreamFrame(const MsgPackValue& payload, bool compress) -> std::expected<std::vector<uint8_t>, std::string> {
    auto body = EncodePayloadBody(payload, compress);
    if (!body) {
        return std::unexpected(body.error());
    }
    if (body->empty() || body->size() > MAX_STREAM_FRAME_BYTES || body->size() > std::numeric_limits<uint32_t>::max()) {
        return std::unexpected("stream frame length exceeds configured limit");
    }
    std::vector<uint8_t> frame;
    frame.reserve(body->size() + 4);
    AppendBE32(frame, static_cast<uint32_t>(body->size()));
    frame.insert(frame.end(), body->begin(), body->end());
    return frame;
}

auto PeekStreamBodyLength(std::span<const uint8_t> streamBytes) noexcept -> std::optional<size_t> {
    if (streamBytes.size() < 4) {
        return std::nullopt;
    }
    const size_t length = (static_cast<size_t>(streamBytes[0]) << 24) | (static_cast<size_t>(streamBytes[1]) << 16) |
                          (static_cast<size_t>(streamBytes[2]) << 8) | static_cast<size_t>(streamBytes[3]);
    if (length == 0 || length > MAX_STREAM_FRAME_BYTES) {
        return std::optional<size_t> {0};
    }
    return length;
}

auto DecodeStreamFrame(std::span<const uint8_t> frameBytes, size_t maxOutputBytes) -> std::expected<MsgPackValue, std::string> {
    auto length = PeekStreamBodyLength(frameBytes);
    if (!length) {
        return std::unexpected("stream frame is missing its four-byte header");
    }
    if (*length == 0) {
        return std::unexpected("invalid or oversized stream-frame length");
    }
    if (frameBytes.size() != *length + 4) {
        return std::unexpected("stream frame length does not match buffer size");
    }
    return DecodePayloadBody(frameBytes.subspan(4), maxOutputBytes);
}

// ============================================================================
// Launch URL parsing
// ============================================================================

auto ParseHostPort(std::string_view endpoint, std::string& host, uint16_t& port) -> bool {
    endpoint = std::string_view(endpoint.data(), endpoint.size());
    if (endpoint.empty()) {
        return false;
    }

    std::string_view hostPart = endpoint;
    std::string_view portPart;
    bool             hasPort = false;

    if (endpoint.front() == '[') {
        const size_t close = endpoint.find(']');
        if (close == std::string_view::npos) {
            return false;
        }

        hostPart = endpoint.substr(1, close - 1);
        if (close + 1 < endpoint.size()) {
            if (endpoint[close + 1] != ':') {
                return false;
            }
            portPart = endpoint.substr(close + 2);
            hasPort  = true;
        }
    } else {
        const size_t colon = endpoint.rfind(':');
        if (colon != std::string_view::npos && endpoint.find(':') == colon) {
            hostPart = endpoint.substr(0, colon);
            portPart = endpoint.substr(colon + 1);
            hasPort  = true;
        }
    }

    if (hostPart.empty()) {
        return false;
    }

    host.assign(hostPart);

    // If we have a port, it must parse successfully; otherwise, we succeed without a port
    if (hasPort) {
        return ParsePort(portPart, port);
    }

    return true;
}

auto ParseLaunchUrl(std::string_view url, std::string_view scheme, LaunchConfig base) -> std::expected<LaunchConfig, std::string> {
    if (scheme.empty()) {
        return std::unexpected("PROJECT_LIGHT_SCHEME is required to parse a launch URL");
    }
    if (!HasLaunchScheme(url, scheme)) {
        return std::unexpected("launch URL does not use the configured PROJECT_LIGHT_SCHEME");
    }

    LaunchConfig config = std::move(base);
    config.enabled      = true;
    const size_t query  = url.find('?');
    if (query == std::string_view::npos) {
        return config;
    }
    std::string_view params = url.substr(query + 1);
    while (!params.empty()) {
        const size_t           ampersand = params.find('&');
        const std::string_view pair      = params.substr(0, ampersand);
        const size_t           equals    = pair.find('=');
        const std::string      key       = PercentDecode(pair.substr(0, equals));
        const std::string      value     = equals == std::string_view::npos ? std::string {} : PercentDecode(pair.substr(equals + 1));
        if (key == "token") {
            config.token = value;
        } else if (key == "userId") {
            uint64_t id = 0;
            if (!ParseUnsigned(value, id)) {
                return std::unexpected("launch URL contains an invalid userId");
            }
            config.userId = id;
        } else if (key == "server") {
            config.host = value;
            if (config.host == "0.0.0.0") {
                config.host = "127.0.0.1";
            }
        } else if (key == "port") {
            if (!ParsePort(value, config.port)) {
                return std::unexpected("launch URL contains an invalid port");
            }
        } else if (key == "place") {
            config.place = value;
        } else if (key == "username") {
            config.username = value;
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        params.remove_prefix(ampersand + 1);
    }
    return config;
}

auto ExtractLaunchUrlArgs(std::span<const std::string_view> rawArgs, LaunchConfig& config, std::string_view scheme)
    -> std::expected<std::vector<std::string>, std::string> {
    std::vector<std::string> filtered;
    filtered.reserve(rawArgs.size());
    for (size_t i = 0; i < rawArgs.size(); ++i) {
        const std::string_view arg = rawArgs[i];
        std::string_view       launchUrl;
        bool                   hasLaunchUrl = false;
        if (arg.starts_with("--launch-url=")) {
            launchUrl    = arg.substr(std::string_view("--launch-url=").size());
            hasLaunchUrl = true;
        } else if (arg == "--launch-url") {
            if (i + 1 >= rawArgs.size()) {
                return std::unexpected("--launch-url requires a URL");
            }
            launchUrl    = rawArgs[++i];
            hasLaunchUrl = true;
        } else if (HasUriScheme(arg)) {
            launchUrl    = arg;
            hasLaunchUrl = true;
        }
        if (hasLaunchUrl) {
            auto parsed = ParseLaunchUrl(launchUrl, scheme, config);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            config = std::move(*parsed);
            continue;
        }
        filtered.emplace_back(arg);
    }
    return filtered;
}

} // namespace ZHLN::ProjectLight
