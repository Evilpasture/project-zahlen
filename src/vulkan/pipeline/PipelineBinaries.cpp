// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "PipelineBinaries.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace ZHLN::Vk {

namespace {

// Serialized store layout (all integers little-endian, host-native):
//   char     magic[4]   "ZPB1"
//   uint32   version    1
//   uint32   globalKeySize, then the device global pipeline key bytes
//   uint32   entryCount
//   per entry: uint32 keySize, uint32 dataSize, key bytes, data bytes
constexpr std::array<uint8_t, 4> kMagic {'Z', 'P', 'B', '1'};
constexpr uint32_t               kFormatVersion = 1;

void AppendU32(std::vector<uint8_t>& out, const uint32_t value) {
    const auto bytes = std::bit_cast<std::array<uint8_t, 4>>(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

struct Reader {
    std::span<const uint8_t> bytes;
    size_t                   offset = 0;

    [[nodiscard]] auto U32() noexcept -> std::optional<uint32_t> {
        if (offset + sizeof(uint32_t) > bytes.size()) {
            return std::nullopt;
        }
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        offset += sizeof(value);
        return value;
    }

    [[nodiscard]] auto Take(const size_t count) noexcept -> std::optional<std::span<const uint8_t>> {
        if (offset + count > bytes.size()) {
            return std::nullopt;
        }
        const auto out = bytes.subspan(offset, count);
        offset += count;
        return out;
    }
};

} // namespace

void PipelineBinaryStore::Init(const VkDevice device, const bool enabled, const std::span<const uint8_t> initialData) noexcept {
    _enabled = enabled && device != VK_NULL_HANDLE;
    if (!_enabled) {
        return;
    }

    VkPipelineBinaryKeyKHR global {};
    global.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
    if (vkGetPipelineKeyKHR(device, nullptr, &global) != VK_SUCCESS || global.keySize == 0 || global.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
        ZHLN::Log("[PipelineBinary] vkGetPipelineKeyKHR failed; pipeline binaries disabled.");
        _enabled = false;
        return;
    }
    _globalKey.assign(global.key, global.key + global.keySize);

    if (initialData.empty()) {
        return;
    }

    Reader     reader {initialData};
    const auto magic         = reader.Take(kMagic.size());
    const auto version       = reader.U32();
    const auto globalKeySize = reader.U32();
    const auto globalKey     = globalKeySize ? reader.Take(*globalKeySize) : std::nullopt;
    const auto entryCount    = reader.U32();
    if (!magic || !version || *version != kFormatVersion || !entryCount || !globalKey || !std::ranges::equal(*magic, std::span<const uint8_t> {kMagic}) ||
        !std::ranges::equal(*globalKey, std::span<const uint8_t> {_globalKey})) {
        ZHLN::Log("[PipelineBinary] Provided binary store was written by a different driver or GPU, or is corrupt; discarding.");
        return;
    }

    std::vector<Entry> entries;
    entries.reserve(*entryCount);
    for (uint32_t i = 0; i < *entryCount; ++i) {
        const auto keySize  = reader.U32();
        const auto dataSize = reader.U32();
        if (!keySize || *keySize == 0 || *keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR || !dataSize) {
            entries.clear();
            break;
        }
        const auto key  = reader.Take(*keySize);
        const auto data = reader.Take(*dataSize);
        if (!key || !data) {
            entries.clear();
            break;
        }
        entries.push_back(Entry {.key = {key->begin(), key->end()}, .data = {data->begin(), data->end()}});
    }
    if (entries.size() != *entryCount) {
        ZHLN::Log("[PipelineBinary] Provided binary store is truncated; discarding.");
        return;
    }

    _entries = std::move(entries);
    ZHLN::Log("[PipelineBinary] Reused {} pipeline binaries from provided data.", _entries.size());
}

auto PipelineBinaryStore::Find(const std::span<const uint8_t> key) const noexcept -> std::optional<std::vector<uint8_t>> {
    const std::lock_guard lock(_mutex);
    for (const Entry& entry: _entries) {
        if (std::ranges::equal(entry.key, key)) {
            return entry.data;
        }
    }
    return std::nullopt;
}

void PipelineBinaryStore::Insert(const std::span<const uint8_t> key, const std::span<const uint8_t> data) noexcept {
    if (key.empty() || data.empty()) {
        return;
    }
    const std::lock_guard lock(_mutex);
    for (Entry& entry: _entries) {
        if (std::ranges::equal(entry.key, key)) {
            entry.data.assign(data.begin(), data.end());
            return;
        }
    }
    _entries.push_back(Entry {.key = {key.begin(), key.end()}, .data = {data.begin(), data.end()}});
}

void PipelineBinaryStore::Erase(const std::span<const uint8_t> key) noexcept {
    const std::lock_guard lock(_mutex);
    std::erase_if(_entries, [&](const Entry& entry) { return std::ranges::equal(entry.key, key); });
}

auto PipelineBinaryStore::Serialize() const noexcept -> std::vector<uint8_t> {
    if (!_enabled) {
        return {};
    }
    const std::lock_guard lock(_mutex);
    std::vector<uint8_t>  out;
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    AppendU32(out, kFormatVersion);
    AppendU32(out, static_cast<uint32_t>(_globalKey.size()));
    out.insert(out.end(), _globalKey.begin(), _globalKey.end());
    AppendU32(out, static_cast<uint32_t>(_entries.size()));
    for (const Entry& entry: _entries) {
        AppendU32(out, static_cast<uint32_t>(entry.key.size()));
        AppendU32(out, static_cast<uint32_t>(entry.data.size()));
        out.insert(out.end(), entry.key.begin(), entry.key.end());
        out.insert(out.end(), entry.data.begin(), entry.data.end());
    }
    return out;
}

auto PipelineBinarySession::Begin(const VkDevice device, PipelineBinaryStore* store, const VkPipelineCreateInfoKHR& createInfo) noexcept
    -> PipelineBinarySession {
    PipelineBinarySession session;
    if (store == nullptr || !store->Enabled() || device == VK_NULL_HANDLE) {
        return session;
    }

    VkPipelineBinaryKeyKHR key {};
    key.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
    if (vkGetPipelineKeyKHR(device, &createInfo, &key) != VK_SUCCESS || key.keySize == 0 || key.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
        return session;
    }

    session._device = device;
    session._store  = store;
    session._key.assign(key.key, key.key + key.keySize);

    const auto stored = store->Find(session._key);

    // On a hit the stored bytes populate the binaries; on a miss the driver
    // compiles them. A rejected stored blob falls back to compiling, and
    // Commit() then overwrites the entry with the fresh binary.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool replay = stored.has_value() && attempt == 0;

        VkPipelineBinaryDataKHR        binaryData {};
        VkPipelineBinaryKeysAndDataKHR keysAndData {};
        if (replay) {
            binaryData  = {.dataSize = stored->size(), .pData = const_cast<void*>(static_cast<const void*>(stored->data()))};
            keysAndData = {.binaryCount = 1, .pPipelineBinaryKeys = &key, .pPipelineBinaryData = &binaryData};
        }

        const VkPipelineBinaryCreateInfoKHR binaryCreateInfo {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
            .pKeysAndDataInfo    = replay ? &keysAndData : nullptr,
            .pipeline            = VK_NULL_HANDLE,
            .pPipelineCreateInfo = &createInfo,
        };
        VkPipelineBinaryHandlesInfoKHR handles {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
        if (vkCreatePipelineBinariesKHR(device, &binaryCreateInfo, nullptr, &handles) != VK_SUCCESS || handles.pipelineBinaryCount == 0) {
            continue;
        }
        session._raw.assign(handles.pipelineBinaryCount, VK_NULL_HANDLE);
        handles.pPipelineBinaries = session._raw.data();
        if (vkCreatePipelineBinariesKHR(device, &binaryCreateInfo, nullptr, &handles) != VK_SUCCESS) {
            session._raw.clear();
            continue;
        }
        session._capture = !replay;
        break;
    }

    if (session._raw.empty()) {
        // No binaries either way: creation proceeds without them, and a
        // stored entry that led here has proven itself useless.
        if (stored.has_value()) {
            store->Erase(session._key);
        }
        session._device = VK_NULL_HANDLE;
        session._store  = nullptr;
        session._key.clear();
        return session;
    }

    session._binaries.reserve(session._raw.size());
    for (const VkPipelineBinaryKHR raw: session._raw) {
        session._binaries.emplace_back(device, raw);
    }
    return session;
}

void PipelineBinarySession::Commit() noexcept {
    if (!_capture || _store == nullptr) {
        return;
    }
    _capture = false;
    for (const PipelineBinary& binary: _binaries) {
        const VkPipelineBinaryDataInfoKHR info {
            .sType          = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,
            .pipelineBinary = binary.Get(),
        };
        VkPipelineBinaryKeyKHR key {};
        key.sType       = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
        size_t dataSize = 0;
        if (vkGetPipelineBinaryDataKHR(_device, &info, &key, &dataSize, nullptr) != VK_SUCCESS || dataSize == 0) {
            continue;
        }
        std::vector<uint8_t> data(dataSize);
        if (vkGetPipelineBinaryDataKHR(_device, &info, &key, &dataSize, data.data()) != VK_SUCCESS || key.keySize == 0) {
            continue;
        }
        _store->Insert(std::span<const uint8_t> {key.key, key.keySize}, std::span<const uint8_t> {data.data(), dataSize});
    }
}

void PipelineBinarySession::Invalidate() noexcept {
    if (_store != nullptr && !_key.empty()) {
        _store->Erase(_key);
    }
}

} // namespace ZHLN::Vk
