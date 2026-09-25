// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RemoteAsset/DiskCache.cpp

#include <RemoteAsset/DiskCache.hpp>

#include <HTTP/HTTP.hpp>

#include <atomic>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <thread>

namespace ZHLN::Remote {

namespace
{

// One temp file per (writer, invocation): two concurrent WriteAtomic calls to
// the same name stage in different files, so neither truncates the other's
// half-written bytes, and the rename makes whichever finished last the file.
[[nodiscard]] auto TempNameFor(const std::filesystem::path& file) -> std::filesystem::path {
    static std::atomic<uint64_t> nextSuffix {0};

    const uint64_t threadId = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string name   = ".tmp-" + std::to_string(threadId) + "-" + std::to_string(nextSuffix++);
    return std::filesystem::path(file.string() + name);
}

[[nodiscard]] auto ReadWholeFile(const std::filesystem::path& file) -> std::vector<uint8_t> {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = in.tellg();
    if (size <= 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
        return {}; // A short read is no file at all: the caller re-fetches.
    }
    return bytes;
}

// This directory was first populated with .glb names by RemoteGLBSample,
// before the cache moved into the extra; a miss on the current spelling
// migrates the old file into place instead of re-downloading it. The rewrite
// is the whole migration: nothing else ever used a different spelling.
void MigrateLegacySpelling(const std::filesystem::path& file) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        return;
    }
    if (file.extension() != ".bin") {
        return;
    }
    std::filesystem::path legacy = file;
    legacy.replace_extension(".glb");
    if (std::filesystem::exists(legacy, ec)) {
        std::filesystem::rename(legacy, file, ec); // A failed rename just costs a re-download.
    }
}

} // namespace

namespace Validators
{

[[nodiscard]] auto IsGLB(std::span<const uint8_t> bytes) noexcept -> bool {
    if ((bytes.size() < 12) || (bytes.size() > ZHLN::HTTP::kMaxBodyBytes)) {
        return false;
    }
    if (std::memcmp(bytes.data(), "glTF", 4) != 0) {
        return false;
    }
    const auto littleEndian32 = [](const uint8_t* p) noexcept -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    };
    const uint32_t version = littleEndian32(bytes.data() + 4);
    const uint32_t length  = littleEndian32(bytes.data() + 8);
    return (version == 2) && (static_cast<size_t>(length) == bytes.size());
}

[[nodiscard]] auto AnyNonEmpty(std::span<const uint8_t> bytes) noexcept -> bool {
    return !bytes.empty();
}

} // namespace Validators

DiskCache::DiskCache(std::filesystem::path rootDir): m_root(std::move(rootDir)) {}

[[nodiscard]] auto DiskCache::Read(std::string_view cacheFileName, ValidatorFn validator) const -> std::optional<std::vector<uint8_t>> {
    const std::filesystem::path file = m_root / std::string(cacheFileName);

    MigrateLegacySpelling(file);

    std::vector<uint8_t> bytes = ReadWholeFile(file);
    if (bytes.empty()) {
        return std::nullopt;
    }
    if ((validator == nullptr) || !validator(bytes)) {
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] auto DiskCache::WriteAtomic(std::string_view cacheFileName, std::span<const uint8_t> bytes) -> bool {
    const std::filesystem::path file = m_root / std::string(cacheFileName);

    std::error_code ec;
    if (const auto parent = file.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    const std::filesystem::path temp = TempNameFor(file);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (out.fail()) {
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

void DiskCache::Invalidate(std::string_view cacheFileName) {
    std::error_code ec;
    std::filesystem::remove(m_root / std::string(cacheFileName), ec);
}

[[nodiscard]] auto DiskCache::Exists(std::string_view cacheFileName) const -> bool {
    std::error_code ec;
    return std::filesystem::exists(m_root / std::string(cacheFileName), ec);
}

} // namespace ZHLN::Remote
