// extras/Network/Wire.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ZHLN.Wire module implementation unit.
//
// The interface (Wire.cppm) exports the declarations; this unit defines the
// non-template runtime code: CRC32 + block compression. Everything here is
// compiled once, in this module's own TU, and never enters an importer's
// instantiation graph — which is what GCC's modules merger trips over when a
// header included in the interface's global module fragment is also included
// textually by an importer (Network.cppm).
//
// Exported templates (Codec<T>, EncodeValue/DecodeValue, EncodeAggregate/
// DecodeAggregate, SchemaVersionOf, Writer::Put/Reader::Get, Encode/Decode)
// must stay defined in the interface: importers instantiate them with their
// own types, and a template declared in the .cppm but defined here is "used
// but never defined" to the importer.

module;

#include <Zahlen/Core/Reflection.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <vector>

module ZHLN.Wire;

// ============================================================================
// CRC32 (IEEE 802.3, reflected) — used by the network frame codec
// ============================================================================

namespace ZHLN::Wire::Checksum {
consteval auto MakeCrcTable() {
    std::array<uint32_t, 256> table {};
    for (uint32_t index = 0; index < 256; ++index) {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }
        table[index] = value;
    }
    return table;
}
} // namespace ZHLN::Wire::Checksum (module-private)

namespace ZHLN::Wire::Checksum {

constexpr auto kCrcTable = MakeCrcTable();

[[nodiscard]] auto Crc32(std::span<const uint8_t> data) noexcept -> uint32_t {
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte: data) {
        crc = kCrcTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace ZHLN::Wire::Checksum

// ============================================================================
// Block compression — compact LZ77 ("mini-LZ4"), dependency-free
//
// Token stream (mirrors the LZ4 block layout):
//   token byte: high nibble = literal length (15 = extended), low nibble =
//   match length - 4 (15 = extended). Then the literal bytes, then — unless
//   this is the final sequence — a 2-byte little-endian back-reference offset
//   in [1, 65535] and the extended match length bytes. The last sequence of
//   a block contains literals only. Length extensions add 255 per byte while
//   the byte equals 255.
// ============================================================================

namespace ZHLN::Wire::Compression {
inline constexpr size_t HASH_LOG  = 12;
inline constexpr size_t HASH_SIZE = 1u << HASH_LOG;

inline auto Hash4(std::span<const uint8_t> data, size_t index) noexcept -> uint32_t {
    const uint32_t value = static_cast<uint32_t>(data[index]) | (static_cast<uint32_t>(data[index + 1]) << 8) | (static_cast<uint32_t>(data[index + 2]) << 16) |
                           (static_cast<uint32_t>(data[index + 3]) << 24);
    return (value * 2654435761u) >> (32 - HASH_LOG);
}
} // namespace ZHLN::Wire::Compression (module-private)

namespace ZHLN::Wire::Compression {

inline constexpr size_t WINDOW_SIZE = 64 * 1024;
inline constexpr size_t MIN_MATCH   = 4;
inline constexpr size_t MAX_MATCH   = 65535;

/// Worst-case encoded size (everything literal, every length extended).
[[nodiscard]] inline auto CompressBound(size_t rawSize) noexcept -> size_t {
    return rawSize + (rawSize / 255) + 16;
}

[[nodiscard]] auto Compress(std::span<const uint8_t> raw, size_t maxOutput) -> Result<std::vector<uint8_t>> {
    if (raw.empty()) {
        return std::vector<uint8_t> {0x00};
    }
    if (CompressBound(raw.size()) > maxOutput) {
        return std::unexpected(
            Failure {
                .code    = ZHLN::Error(WireError::CompressionFailed),
                .details = ZHLN::Reflect::FormatEnumMessage(
                    WireError::CompressionFailed, std::format("input of {} byte(s) exceeds the {} byte output limit", raw.size(), maxOutput)
                )
            }
        );
    }

    Buffer output(CompressBound(raw.size()));
    // 0xFFFFFFFF marks an empty slot; position 0 is a legitimate candidate.
    std::vector<uint32_t> table(HASH_SIZE, 0xFFFFFFFFu);

    const auto emitLength = [&output](size_t length) -> Result<void> {
        while (length >= 255) {
            const Result<void> res = output.AppendByte(255);
            if (!res) {
                return res;
            }
            length -= 255;
        }
        return output.AppendByte(static_cast<uint8_t>(length));
    };

    const auto emitLiterals = [&output, &raw](size_t begin, size_t end) -> Result<void> { return output.Append(raw.subspan(begin, end - begin)); };

    size_t anchor   = 0; // start of the pending literal run
    size_t position = 0;

    while (position + MIN_MATCH <= raw.size()) {
        const uint32_t hash      = Hash4(raw, position);
        const uint32_t candidate = table[hash];
        table[hash]              = static_cast<uint32_t>(position);

        // Offsets are stored as 16-bit values in [1, 65535]; WINDOW_SIZE itself is not representable.
        const bool usable = candidate != 0xFFFFFFFFu && candidate < position && (position - candidate) < WINDOW_SIZE &&
                            std::memcmp(raw.data() + candidate, raw.data() + position, MIN_MATCH) == 0;
        if (!usable) {
            ++position;
            continue;
        }

        // Extend the match as far as it actually goes.
        size_t matchLength = MIN_MATCH;
        while (matchLength < MAX_MATCH && position + matchLength < raw.size() && raw[candidate + matchLength] == raw[position + matchLength]) {
            ++matchLength;
        }

        const size_t  literalLength = position - anchor;
        const size_t  matchToken    = matchLength - MIN_MATCH;
        const uint8_t token         = static_cast<uint8_t>(((literalLength < 15 ? literalLength : 15) << 4) | (matchToken < 15 ? matchToken : 15));

        Result<void> res = output.AppendByte(token);
        if (res && literalLength >= 15) {
            res = emitLength(literalLength - 15);
        }
        if (res) {
            res = emitLiterals(anchor, position);
        }
        if (res) {
            const size_t offset = position - candidate;
            res                 = output.AppendByte(static_cast<uint8_t>(offset & 0xFFu));
            if (res) {
                res = output.AppendByte(static_cast<uint8_t>((offset >> 8) & 0xFFu));
            }
        }
        if (res && matchToken >= 15) {
            res = emitLength(matchToken - 15);
        }
        if (!res) {
            return std::unexpected(res.error());
        }

        anchor   = position + matchLength;
        position = anchor;
    }

    // Final literal-only sequence.
    const size_t literalLength = raw.size() - anchor;
    Result<void> res           = output.AppendByte(static_cast<uint8_t>((literalLength < 15 ? literalLength : 15) << 4));
    if (res && literalLength >= 15) {
        res = emitLength(literalLength - 15);
    }
    if (res) {
        res = emitLiterals(anchor, raw.size());
    }
    if (!res) {
        return std::unexpected(res.error());
    }

    const auto bytes = output.Data();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

[[nodiscard]] auto Decompress(std::span<const uint8_t> compressed, size_t maxDecompressed) -> Result<std::vector<uint8_t>> {
    // maxDecompressed == 0 means "no explicit cap".
    std::vector<uint8_t> out;
    if (maxDecompressed != 0) {
        out.reserve(maxDecompressed < (1u << 20) ? maxDecompressed : (1u << 20));
    }

    const auto fits = [&](size_t additional) -> bool { return maxDecompressed == 0 || out.size() + additional <= maxDecompressed; };

    const auto readLength = [&](size_t& position, size_t base) -> Result<size_t> {
        size_t length = base;
        while (true) {
            if (position >= compressed.size()) {
                return std::unexpected(
                    Failure {
                        .code    = ZHLN::Error(WireError::DecompressionFailed),
                        .details = ZHLN::Reflect::FormatEnumMessage(WireError::DecompressionFailed, "truncated length extension")
                    }
                );
            }
            const uint8_t byte = compressed[position];
            ++position;
            length += byte;
            if (byte != 255) {
                return length;
            }
        }
    };

    size_t position = 0;
    while (position < compressed.size()) {
        const uint8_t token = compressed[position];
        ++position;

        size_t literalLength = token >> 4;
        if (literalLength == 15) {
            const auto extended = readLength(position, 15);
            if (!extended) {
                return std::unexpected(extended.error());
            }
            literalLength = *extended;
        }
        if (literalLength > compressed.size() - position) {
            return std::unexpected(
                Failure {
                    .code    = ZHLN::Error(WireError::DecompressionFailed),
                    .details = ZHLN::Reflect::FormatEnumMessage(
                        WireError::DecompressionFailed, std::format("literal run of {} byte(s) at input offset {} overruns the block", literalLength, position)
                    )
                }
            );
        }
        if (!fits(literalLength)) {
            return std::unexpected(
                Failure {
                    .code    = ZHLN::Error(WireError::DecompressionFailed),
                    .details = ZHLN::Reflect::FormatEnumMessage(
                        WireError::DecompressionFailed, std::format("decompressed size would exceed the {} byte limit", maxDecompressed)
                    )
                }
            );
        }
        out.insert(
            out.end(), compressed.begin() + static_cast<std::ptrdiff_t>(position), compressed.begin() + static_cast<std::ptrdiff_t>(position + literalLength)
        );
        position += literalLength;

        if (position == compressed.size()) {
            break; // final sequence: literals only
        }
        if (compressed.size() - position < 2) {
            return std::unexpected(
                Failure {
                    .code    = ZHLN::Error(WireError::DecompressionFailed),
                    .details = ZHLN::Reflect::FormatEnumMessage(WireError::DecompressionFailed, "truncated match offset")
                }
            );
        }
        const size_t offset = static_cast<size_t>(compressed[position]) | (static_cast<size_t>(compressed[position + 1]) << 8);
        position += 2;
        if (offset == 0 || offset > out.size()) {
            return std::unexpected(
                Failure {
                    .code    = ZHLN::Error(WireError::DecompressionFailed),
                    .details = ZHLN::Reflect::FormatEnumMessage(
                        WireError::DecompressionFailed, std::format("back-reference offset {} is out of range ({} byte(s) produced so far)", offset, out.size())
                    )
                }
            );
        }

        size_t matchLength = static_cast<size_t>(token & 0x0Fu) + MIN_MATCH;
        if ((token & 0x0Fu) == 15) {
            const auto extended = readLength(position, 15);
            if (!extended) {
                return std::unexpected(extended.error());
            }
            matchLength = *extended + MIN_MATCH;
        }
        if (!fits(matchLength)) {
            return std::unexpected(
                Failure {
                    .code    = ZHLN::Error(WireError::DecompressionFailed),
                    .details = ZHLN::Reflect::FormatEnumMessage(
                        WireError::DecompressionFailed, std::format("decompressed size would exceed the {} byte limit", maxDecompressed)
                    )
                }
            );
        }

        size_t source = out.size() - offset;
        for (size_t index = 0; index < matchLength; ++index) {
            out.push_back(out[source]); // overlapping copies replicate runs, exactly like LZ4
            ++source;
        }
    }
    return out;
}

} // namespace ZHLN::Wire::Compression
