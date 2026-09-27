// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Description.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ZHLN::Vk {


inline constexpr uint32_t kSpirvMagic             = 0x07230203;
inline constexpr size_t   kSpirvHeaderWords       = 5;
inline constexpr uint16_t kSpirvOpEntryPoint    = 15;
inline constexpr uint16_t kSpirvOpName            = 5;
inline constexpr uint16_t kSpirvOpTypeSampler     = 26;
inline constexpr uint16_t kSpirvOpTypePointer     = 32;
inline constexpr uint16_t kSpirvOpFunction        = 54;
inline constexpr uint16_t kSpirvOpVariable        = 59;
inline constexpr uint16_t kSpirvOpDecorate        = 71;
inline constexpr uint16_t kSpirvDecorationBinding = 33;
inline constexpr uint16_t kSpirvDecorationSet     = 34;


struct SpirvBinding {
    uint32_t binding    = 0;
    uint32_t nameOffset = 0;
    uint32_t nameLength = 0;
    bool sampler = false;

    [[nodiscard]] constexpr auto IsNamed(std::span<const uint8_t> module, std::string_view name) const noexcept -> bool {
        if (name.size() != nameLength || static_cast<size_t>(nameOffset) + nameLength > module.size()) {
            return false;
        }
        for (uint32_t i = 0; i < nameLength; ++i) {
            if (module[nameOffset + i] != static_cast<uint8_t>(name[i])) {
                return false;
            }
        }
        return true;
    }
};

class SpirvBindings {
  public:
    static constexpr uint32_t kCapacity = 64;

    constexpr SpirvBindings() noexcept = default;

    [[nodiscard]] static constexpr auto Parse(std::span<const uint8_t> module, uint32_t set) noexcept -> SpirvBindings;

    [[nodiscard]] constexpr auto Count() const noexcept -> uint32_t {
        return _count;
    }
    [[nodiscard]] constexpr auto operator[](uint32_t index) const noexcept -> const SpirvBinding& {
        return _bindings[index];
    }
    [[nodiscard]] constexpr auto Bytes() const noexcept -> std::span<const uint8_t> {
        return _bytes;
    }
    [[nodiscard]] constexpr auto Complete() const noexcept -> bool {
        return !_truncated;
    }

    [[nodiscard]] constexpr auto EntryPointCount() const noexcept -> uint32_t {
        return _entryCount;
    }
    [[nodiscard]] constexpr auto HighestDeclaredSet() const noexcept -> uint32_t {
        return _highestSet;
    }
    [[nodiscard]] constexpr auto ExecutionModel() const noexcept -> uint32_t {
        return _executionModel;
    }
    [[nodiscard]] constexpr auto EntryPointOffset() const noexcept -> uint32_t {
        return _entryOffset;
    }
    [[nodiscard]] constexpr auto EntryPointLength() const noexcept -> uint32_t {
        return _entryLength;
    }
    [[nodiscard]] constexpr auto IsEntryPoint(std::string_view name) const noexcept -> bool {
        if (_entryCount != 1 || name.size() != _entryLength || static_cast<size_t>(_entryOffset) + _entryLength > _bytes.size()) {
            return false;
        }
        for (uint32_t i = 0; i < _entryLength; ++i) {
            if (_bytes[_entryOffset + i] != static_cast<uint8_t>(name[i])) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr auto DeclaresResource(std::string_view name) const noexcept -> bool {
        for (uint32_t i = 0; i < _count; ++i) {
            if (!_bindings[i].sampler && _bindings[i].IsNamed(_bytes, name)) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] constexpr auto DeclaresSampler(std::string_view name) const noexcept -> bool {
        for (uint32_t i = 0; i < _count; ++i) {
            if (_bindings[i].sampler && _bindings[i].IsNamed(_bytes, name)) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<SpirvBinding, kCapacity> _bindings {};
    std::span<const uint8_t>            _bytes {};
    uint32_t                            _count     = 0;
    bool                                _truncated = false;

    uint32_t _highestSet     = 0;
    uint32_t _entryCount     = 0;
    uint32_t _executionModel = 0;
    uint32_t _entryOffset    = 0;
    uint32_t _entryLength    = 0;
};


[[nodiscard]] constexpr auto SpirvBindings::Parse(std::span<const uint8_t> module, uint32_t set) noexcept -> SpirvBindings {
    struct Candidate {
        uint32_t id         = 0;
        uint32_t set        = 0;
        uint32_t binding    = 0;
        bool     hasSet     = false;
        bool     hasBinding = false;
    };
    struct Range {
        uint32_t id     = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    struct Pair {
        uint32_t id      = 0;
        uint32_t related = 0;
    };

    constexpr uint32_t kNameCapacity     = 128;
    constexpr uint32_t kTypeCapacity     = 128;
    constexpr uint32_t kVariableCapacity = 64;
    constexpr uint32_t kSamplerCapacity  = 16;

    SpirvBindings out {};
    out._bytes = module;

    if (module.size() < kSpirvHeaderWords * 4 || (module.size() % 4) != 0) {
        out._truncated = true;
        return out;
    }
    const size_t words = module.size() / 4;

    const auto wordAt = [&](size_t index) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[index * 4]) | (static_cast<uint32_t>(module[index * 4 + 1]) << 8) |
               (static_cast<uint32_t>(module[index * 4 + 2]) << 16) | (static_cast<uint32_t>(module[index * 4 + 3]) << 24);
    };
    const auto countAt = [&](size_t word) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[word * 4 + 2]) | (static_cast<uint32_t>(module[word * 4 + 3]) << 8);
    };

    if (wordAt(0) != kSpirvMagic) {
        out._truncated = true;
        return out;
    }

    std::array<Candidate, kCapacity> candidates {};
    uint32_t                         candidateCount = 0;

    for (size_t word = kSpirvHeaderWords; word < words;) {
        const uint32_t count = countAt(word);
        if (count == 0 || word + count > words) {
            out._truncated = true;
            return out;
        }
        if (module[word * 4 + 1] == 0 && module[word * 4] == static_cast<uint8_t>(kSpirvOpFunction)) {
            break;
        }
        if (module[word * 4 + 1] == 0 && module[word * 4] == static_cast<uint8_t>(kSpirvOpEntryPoint) && count >= 4) {
            ++out._entryCount;
            if (out._entryCount == 1) {
                out._executionModel = wordAt(word + 1);
                const size_t nameStart = (word + 3) * 4;
                const size_t limit     = (word + count) * 4;
                size_t       nameEnd   = nameStart;
                while (nameEnd < limit && module[nameEnd] != 0) {
                    ++nameEnd;
                }
                if (nameEnd == limit) {
                    out._truncated = true;
                    return out;
                }
                out._entryOffset = static_cast<uint32_t>(nameStart);
                out._entryLength = static_cast<uint32_t>(nameEnd - nameStart);
            }
        }
        if (module[word * 4 + 1] == 0 && module[word * 4] == static_cast<uint8_t>(kSpirvOpDecorate) && count >= 4) {
            const uint32_t decoration = wordAt(word + 2);
            if (decoration == kSpirvDecorationBinding || decoration == kSpirvDecorationSet) {
                const uint32_t target = wordAt(word + 1);
                int64_t        known  = -1;
                for (uint32_t i = 0; i < candidateCount; ++i) {
                    if (candidates[i].id == target) {
                        known = static_cast<int64_t>(i);
                    }
                }
                if (known < 0) {
                    if (candidateCount == kCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    known                 = static_cast<int64_t>(candidateCount);
                    candidates[candidateCount].id = target;
                    ++candidateCount;
                }
                Candidate& entry = candidates[static_cast<size_t>(known)];
                if (decoration == kSpirvDecorationBinding) {
                    entry.binding    = wordAt(word + 3);
                    entry.hasBinding = true;
                } else {
                    entry.set    = wordAt(word + 3);
                    entry.hasSet = true;
                }
            }
        }
        word += count;
    }

    std::array<Range, kNameCapacity>       names {};
    std::array<Pair, kVariableCapacity>    variables {};
    std::array<Pair, kTypeCapacity>        pointers {};
    std::array<uint32_t, kSamplerCapacity> samplerTypes {};

    uint32_t nameCount     = 0;
    uint32_t variableCount = 0;
    uint32_t pointerCount  = 0;
    uint32_t samplerCount  = 0;

    const auto isCandidate = [&](uint32_t id) noexcept -> bool {
        for (uint32_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].id == id) {
                return true;
            }
        }
        return false;
    };

    for (size_t word = kSpirvHeaderWords; word < words;) {
        const uint32_t count  = countAt(word);
        const uint32_t opcode = static_cast<uint32_t>(module[word * 4]) | (static_cast<uint32_t>(module[word * 4 + 1]) << 8);
        if (count == 0 || word + count > words) {
            out._truncated = true;
            return out;
        }
        switch (opcode) {
            case kSpirvOpName:
                if (count >= 3 && isCandidate(wordAt(word + 1))) {
                    if (nameCount == kNameCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    const uint32_t offset = static_cast<uint32_t>((word + 2) * 4);
                    const uint32_t span   = (count - 2) * 4;
                    uint32_t       length = 0;
                    while (length < span && module[offset + length] != 0) {
                        ++length;
                    }
                    if (length == span) {
                        out._truncated = true;
                        return out;
                    }
                    names[nameCount++] = Range {.id = wordAt(word + 1), .offset = offset, .length = length};
                }
                break;
            case kSpirvOpVariable:
                if (count >= 4 && isCandidate(wordAt(word + 2))) {
                    if (variableCount == kVariableCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    variables[variableCount++] = Pair {.id = wordAt(word + 2), .related = wordAt(word + 1)};
                }
                break;
            case kSpirvOpTypePointer:
                if (count >= 4) {
                    if (pointerCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    pointers[pointerCount++] = Pair {.id = wordAt(word + 1), .related = wordAt(word + 3)};
                }
                break;
            case kSpirvOpFunction:
                word = words;
                break;
            case kSpirvOpTypeSampler:
                if (count >= 2) {
                    if (samplerCount == kSamplerCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    samplerTypes[samplerCount++] = wordAt(word + 1);
                }
                break;
            default:
                break;
        }
        word += count;
    }

    for (uint32_t i = 0; i < candidateCount; ++i) {
        const Candidate& candidate = candidates[i];
        const uint32_t candidateSet = candidate.hasSet ? candidate.set : 0;
        if (!candidate.hasBinding) {
            continue;
        }
        if (candidateSet > out._highestSet) {
            out._highestSet = candidateSet;
        }
        if (candidateSet != set) {
            continue;
        }

        SpirvBinding binding {.binding = candidate.binding};
        for (uint32_t n = 0; n < nameCount; ++n) {
            if (names[n].id == candidate.id) {
                binding.nameOffset = names[n].offset;
                binding.nameLength = names[n].length;
            }
        }
        if (binding.nameLength == 0) {
            out._truncated = true;
            return out;
        }

        uint32_t pointee = 0;
        for (uint32_t v = 0; v < variableCount; ++v) {
            if (variables[v].id != candidate.id) {
                continue;
            }
            for (uint32_t p = 0; p < pointerCount; ++p) {
                if (pointers[p].id == variables[v].related) {
                    pointee = pointers[p].related;
                }
            }
        }
        for (uint32_t s = 0; s < samplerCount; ++s) {
            binding.sampler = binding.sampler || samplerTypes[s] == pointee;
        }

        if (out._count == kCapacity) {
            out._truncated = true;
            return out;
        }
        out._bindings[out._count++] = binding;
    }

    for (uint32_t i = 1; i < out._count; ++i) {
        const SpirvBinding current = out._bindings[i];
        uint32_t          j         = i;
        while (j > 0 && out._bindings[j - 1].binding > current.binding) {
            out._bindings[j] = out._bindings[j - 1];
            --j;
        }
        out._bindings[j] = current;
    }

    return out;
}

}
