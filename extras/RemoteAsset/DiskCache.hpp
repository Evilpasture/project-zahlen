// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RemoteAsset/DiskCache.hpp
//
// A directory of complete downloads, shared between runs. A cache file is
// either the exact bytes a fetcher already verified, or it is not there at
// all: every write goes through a sibling temp file and an atomic rename, the
// way the engine's pipeline cache writes, so an interrupted download leaves no
// half-written file behind for the next run to mistake for a cache hit.
//
// Thread safety needs no mutex: the rename is the synchronization. Two writers
// to one name stage in different temp files (a per-thread unique suffix) and
// the last rename wins; a reader sees either the old file or the new one,
// never a torn one. The validators below are the price a reader pays to trust
// a file: a hit that fails its validator is a truncated download or an HTML
// error page that outlived its fetch, and it is re-fetched rather than handed
// to a parser that would only answer "not what you asked for".

#pragma once

#include <Zahlen/FileSystem/Paths.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Remote {

// A byte predicate that decides whether cached bytes are what they claim to
// be. A plain function pointer: a validator must be stateless and must not
// throw, because it runs on the frame thread against a file somebody else
// wrote.
using ValidatorFn = bool (*)(std::span<const uint8_t> bytes);

// Built-in container checkers.
namespace Validators
{
// The 12-byte GLB container header: the magic, the version, and the total
// byte length of the container, read little-endian because the spec says
// little-endian and the host is not the spec. The length is the bytes the
// parser will actually read, so a container that disagrees with its own size
// is a bad one.
[[nodiscard]] auto IsGLB(std::span<const uint8_t> bytes) noexcept -> bool;

// Anything non-empty. For callers whose parser is the validator.
[[nodiscard]] auto AnyNonEmpty(std::span<const uint8_t> bytes) noexcept -> bool;
} // namespace Validators

class DiskCache
{
  public:
    // @p rootDir defaults to the engine's cache directory under "http" --
    // build/cache in a dev tree, the per-user cache directory otherwise,
    // ZHLN_CACHE_DIR over both -- because a downloaded asset is the same kind
    // of thing as a pipeline cache: regenerable, machine-local, never
    // committed. Tests pass a directory of their own.
    explicit DiskCache(std::filesystem::path rootDir = ZHLN::FS::Paths::CacheDir() / "http");

    // The cached bytes under @p cacheFileName, validated -- or nullopt when
    // the file is absent, unreadable, short, or fails the validator. A miss
    // tells the caller nothing about why; Exists plus Invalidate is the
    // deliberate pair for a miss that should clean up a corrupt file.
    [[nodiscard]] auto Read(std::string_view cacheFileName, ValidatorFn validator) const -> std::optional<std::vector<uint8_t>>;

    // Stages @p bytes in a sibling temp file and renames it into place. The
    // directory is created when missing. False when the staging write or the
    // rename refused; the temp file is then removed, so a failure leaves the
    // previous state exactly as it was.
    [[nodiscard]] auto WriteAtomic(std::string_view cacheFileName, std::span<const uint8_t> bytes) -> bool;

    // Removes the file under @p cacheFileName. Missing is success: the point
    // of the call is the state after it.
    void Invalidate(std::string_view cacheFileName);

    [[nodiscard]] auto Exists(std::string_view cacheFileName) const -> bool;

    [[nodiscard]] auto Root() const -> const std::filesystem::path& {
        return m_root;
    }

  private:
    std::filesystem::path m_root;
};

} // namespace ZHLN::Remote
