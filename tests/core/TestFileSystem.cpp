// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/FileSystem/FileSystem.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::Test {

static_assert(!std::is_copy_constructible_v<FS::MappedFile>);
static_assert(!std::is_copy_assignable_v<FS::MappedFile>);
static_assert(std::is_nothrow_move_constructible_v<FS::MappedFile>);
static_assert(std::is_nothrow_move_assignable_v<FS::MappedFile>);

namespace {

auto ReadDiskText(const std::filesystem::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

auto WriteMinimalUncompressedPak(
    const std::filesystem::path& pakPath,
    std::string_view             virtualPath,
    std::string_view             payload
) -> bool {
    constexpr uint64_t headerSize  = sizeof(FS::PakHeader);
    const auto         payloadSize = static_cast<uint64_t>(payload.size());
    const uint64_t     dirOffset   = headerSize + payloadSize;

    FS::PakHeader header {};
    std::memcpy(header.magic, "ZPAK", 4);
    header.version    = FS::kPakFormatVersion;
    header.entryCount = 1;
    header.tocOffset  = dirOffset;

    FS::PakEntry entry {};
    entry.pathHash         = FS::HashPath(virtualPath);
    entry.offset           = headerSize;
    entry.compressedSize   = payloadSize;
    entry.uncompressedSize = payloadSize;
    entry.compression      = 0;
    entry.flags            = 0;

    std::ofstream out(pakPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    return static_cast<bool>(out);
}

} // namespace

struct ZHLN_ANNOTATION(SuiteFixture {}) VfsTaskPoolFixture {
    TaskSystem::Scope scope {2, 32, kMinimumFiberStackSize};
};

void test_mapped_file_raii_modes_and_move_semantics(TempDir& tmp) {
    // Missing and zero-length files return an invalid handle cleanly.
    const auto missing = FS::MappedFile::Open((tmp.Path() / "missing.bin").string());
    Expect(!missing.IsValid());
    Expect(!static_cast<bool>(missing));
    Expect(missing.Bytes().empty());

    const auto emptyPath = tmp.WriteText("empty.bin", "");
    const auto emptyMap  = FS::MappedFile::Open(emptyPath.string());
    Expect(!emptyMap.IsValid());

    const auto payloadPath = tmp.WriteText("greeting.txt", "Hello Zahlen VFS");

    // ReadOnly mapping: exposes const spans, withholds MutableData/MutableBytes.
    {
        auto ro = FS::MappedFile::Open(payloadPath.string(), FS::MapAccess::ReadOnly);
        Expect(ro.IsValid());
        Expect(ro.Size()) == 16u;
        Expect(ro.Access() == FS::MapAccess::ReadOnly);
        Expect(ro.MutableData() == nullptr);
        Expect(ro.MutableBytes().empty());
        Expect(std::string_view(ro.Chars().data(), ro.Chars().size())) == "Hello Zahlen VFS";

        // Move construction transfers ownership and leaves the source empty.
        FS::MappedFile moved(std::move(ro));
        Expect(!ro.IsValid());
        Expect(ro.Size()) == 0u;
        Expect(moved.IsValid());
        Expect(std::string_view(moved.Chars().data(), moved.Chars().size())) == "Hello Zahlen VFS";

        moved.Reset();
        Expect(!moved.IsValid());
    }

    // CopyOnWrite mapping: local page edits never mutate the file on disk.
    {
        auto cow = FS::MappedFile::Open(payloadPath.string(), FS::MapAccess::CopyOnWrite);
        Expect(cow.IsValid());
        auto writable = cow.MutableBytes();
        Expect(writable.size()) == 16u;
        writable[0] = std::byte {'X'};
        Expect(std::string_view(cow.Chars().data(), cow.Chars().size())) == "Xello Zahlen VFS";
    }
    Expect(ReadDiskText(payloadPath)) == "Hello Zahlen VFS";

    // ReadWrite mapping: edits persist to the underlying file on disk.
    {
        auto rw = FS::MappedFile::Open(payloadPath.string(), FS::MapAccess::ReadWrite);
        Expect(rw.IsValid());
        auto writable = rw.MutableBytes();
        Expect(writable.size()) == 16u;
        writable[0] = std::byte {'Z'};
        rw.Reset();
    }
    Expect(ReadDiskText(payloadPath)) == "Zello Zahlen VFS";
}

void test_vfs_directory_mount_overlay_async_and_traversal_rejection(
    VfsTaskPoolFixture&,
    TempDir& tmp
) {
    const auto dirA    = tmp.Path() / "mount_a";
    const auto dirB    = tmp.Path() / "mount_b";
    const auto pakPath = tmp.SubPath("archives/base.pak");
    tmp.WriteText("secret.txt", "TOP_SECRET");

    std::error_code ec;
    std::filesystem::create_directories(dirA / "configs", ec);
    std::filesystem::create_directories(dirB / "configs", ec);

    {
        std::ofstream f(dirA / "configs/settings.cfg", std::ios::binary);
        f << "from_dir_a";
    }
    {
        std::ofstream f(dirA / "only_a.txt", std::ios::binary);
        f << "exclusive_a";
    }
    {
        std::ofstream f(dirB / "configs/settings.cfg", std::ios::binary);
        f << "from_dir_b_override";
    }
    Expect(WriteMinimalUncompressedPak(pakPath, "configs/settings.cfg", "from_pak_base"));

    FS::VirtualFileSystem vfs;
    Expect(!vfs.MountDirectory(""));
    Expect(vfs.MountDirectory("").error()) == ErrorCode(FS::VFSError::InvalidPath);
    Expect(!vfs.MountDirectory((tmp.Path() / "does_not_exist").string()));
    Expect(!vfs.MountDirectory(pakPath.string())); // regular file, not a directory
    Expect(!vfs.MountDirectory(dirA.string(), FS::MountOptions {.virtualPrefix = "../escape"}));

    Expect(vfs.MountPak(pakPath.string()).has_value());
    Expect(vfs.Exists("configs/settings.cfg"));

    // Mount dirA, then dirB: reverse mount order means dirB > dirA > pak.
    Expect(vfs.MountDirectory(dirA.string()).has_value());
    Expect(vfs.MountDirectory(dirB.string()).has_value());
    Expect(vfs.MountedDirectoryCount()) == 2u;

    {
        auto loaded = vfs.LoadSync(FS::HashPath("configs/settings.cfg"));
        Expect(loaded.has_value());
        Expect(!loaded->IsZeroCopy());
        Expect(loaded->Text()) == "from_dir_b_override";
    }

    // Unmount dirB -> falls back to dirA; unmount dirA -> falls back to pak!
    Expect(vfs.UnmountDirectory(dirB.string()).has_value());
    Expect(vfs.MountedDirectoryCount()) == 1u;
    {
        auto text = vfs.ReadText("configs/settings.cfg");
        Expect(text.has_value());
        Expect(*text) == "from_dir_a";

        auto sz = vfs.FileSize("configs/settings.cfg");
        Expect(sz.has_value());
        Expect(*sz) == std::string_view("from_dir_a").size();
    }

    // Dynamic file written into dirA AFTER MountDirectory() is discovered by Hash64 LoadSync & LoadAsync!
    {
        std::ofstream lateFile(dirA / "configs/late_spawned.txt", std::ios::binary);
        lateFile << "created_after_mount";
    }
    Expect(vfs.Exists(FS::HashPath("configs/late_spawned.txt")));

    std::array<FS::AsyncLoad, 2> asyncReqs {
        FS::AsyncLoad {.assetID = FS::HashPath("only_a.txt")},
        FS::AsyncLoad {.assetID = FS::HashPath("configs/late_spawned.txt")},
    };
    TaskSystem::Counter counter {0};
    vfs.LoadAsync(RestrictSpan<FS::AsyncLoad>(asyncReqs.data(), asyncReqs.size()), &counter);
    TaskSystem::Wait(&counter);

    Expect(asyncReqs[0].result.has_value());
    Expect(asyncReqs[1].result.has_value());
    Expect(asyncReqs[0].result->Text()) == "exclusive_a";
    Expect(asyncReqs[1].result->Text()) == "created_after_mount";

    // Path traversal & absolute path attempts must be rejected unconditionally.
    Expect(!vfs.Exists("../secret.txt"));
    Expect(!vfs.Exists("configs/../../secret.txt"));
    Expect(!vfs.Exists("/etc/passwd"));
    Expect(vfs.Open("../secret.txt").error()) == ErrorCode(FS::VFSError::InvalidPath);
    Expect(vfs.FileSize("configs/../../secret.txt").error()) == ErrorCode(FS::VFSError::InvalidPath);

    // Virtual prefix + case-insensitive mount options (e.g. Source engine assets on POSIX).
    {
        const auto mixedDir = tmp.Path() / "mixed_mod";
        std::filesystem::create_directories(mixedDir / "Materials" / "Brick", ec);
        {
            std::ofstream f(mixedDir / "Materials" / "Brick" / "Wall.VMT", std::ios::binary);
            f << "LightmappedGeneric";
        }
        Expect(vfs.MountDirectory(
            mixedDir.string(),
            FS::MountOptions {.virtualPrefix = "csgo", .caseInsensitive = true}
        ).has_value());
        Expect(vfs.Exists("csgo/materials/brick/wall.vmt"));
        Expect(!vfs.Exists("materials/brick/wall.vmt"));
        auto text = vfs.ReadText("CSGO/materials/brick/wall.vmt");
        Expect(text.has_value());
        Expect(*text) == "LightmappedGeneric";
        Expect(vfs.UnmountDirectory(mixedDir.string()).has_value());
    }

    // Unmounting dirA restores the underlying `.pak` entry with zero-copy pinning.
    Expect(vfs.UnmountDirectory(dirA.string()).has_value());
    Expect(vfs.MountedDirectoryCount()) == 0u;
    {
        auto loaded = vfs.LoadSync(FS::HashPath("configs/settings.cfg"));
        Expect(loaded.has_value());
        Expect(loaded->IsZeroCopy());
        Expect(loaded->Text()) == "from_pak_base";
    }
}

} // namespace ZHLN::Test
