// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
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
    Expect(!vfs.MountDirectory((tmp.Path() / "does_not_exist").string()));
    Expect(!vfs.MountDirectory(pakPath.string())); // regular file, not a directory

    Expect(vfs.MountPak(pakPath.string()));
    Expect(vfs.Exists("configs/settings.cfg"));

    // Mount dirA, then dirB: reverse mount order means dirB > dirA > pak.
    Expect(vfs.MountDirectory(dirA.string()));
    Expect(vfs.MountDirectory(dirB.string()));
    Expect(vfs.MountedDirectoryCount()) == 2u;

    {
        FS::LoadRequest req {.assetID = FS::HashPath("configs/settings.cfg")};
        Expect(vfs.LoadSync(req));
        Expect(!req.isZeroCopy);
        Expect(std::string_view(static_cast<const char*>(req.outData), req.outSize)) == "from_dir_b_override";
        FS::VirtualFileSystem::FreeMemory(req);
    }

    // Unmount dirB -> falls back to dirA; unmount dirA -> falls back to pak!
    Expect(vfs.UnmountDirectory(dirB.string()));
    Expect(vfs.MountedDirectoryCount()) == 1u;
    {
        std::array<char, 64> buf {};
        const size_t         n = vfs.ReadFile("configs/settings.cfg", buf.data(), buf.size());
        Expect(std::string_view(buf.data(), n)) == "from_dir_a";
    }

    // Dynamic file written into dirA AFTER MountDirectory() is discovered by Hash64 LoadSync & LoadAsync!
    {
        std::ofstream lateFile(dirA / "configs/late_spawned.txt", std::ios::binary);
        lateFile << "created_after_mount";
    }
    Expect(vfs.Exists(FS::HashPath("configs/late_spawned.txt")));

    std::array<FS::LoadRequest, 2> asyncReqs {
        FS::LoadRequest {.assetID = FS::HashPath("only_a.txt")},
        FS::LoadRequest {.assetID = FS::HashPath("configs/late_spawned.txt")},
    };
    TaskSystem::Counter counter {0};
    vfs.LoadAsync(RestrictSpan<FS::LoadRequest>(asyncReqs.data(), asyncReqs.size()), &counter);
    TaskSystem::Wait(&counter);

    Expect(asyncReqs[0].success);
    Expect(asyncReqs[1].success);
    Expect(std::string_view(static_cast<const char*>(asyncReqs[0].outData), asyncReqs[0].outSize)) == "exclusive_a";
    Expect(std::string_view(static_cast<const char*>(asyncReqs[1].outData), asyncReqs[1].outSize)) == "created_after_mount";
    FS::VirtualFileSystem::FreeMemory(asyncReqs[0]);
    FS::VirtualFileSystem::FreeMemory(asyncReqs[1]);

    // Path traversal & absolute path attempts must be rejected unconditionally.
    std::array<char, 64> leakBuf {};
    Expect(!vfs.Exists("../secret.txt"));
    Expect(!vfs.Exists("configs/../../secret.txt"));
    Expect(!vfs.Exists("/etc/passwd"));
    Expect(vfs.ReadFile("../secret.txt", leakBuf.data(), leakBuf.size())) == 0u;
    Expect(vfs.ReadFile("configs/../../secret.txt", nullptr, 0)) == 0u;

    // Unmounting dirA restores the underlying `.pak` entry.
    Expect(vfs.UnmountDirectory(dirA.string()));
    Expect(vfs.MountedDirectoryCount()) == 0u;
    {
        FS::LoadRequest req {.assetID = FS::HashPath("configs/settings.cfg")};
        Expect(vfs.LoadSync(req));
        Expect(req.isZeroCopy);
        Expect(std::string_view(static_cast<const char*>(req.outData), req.outSize)) == "from_pak_base";
        FS::VirtualFileSystem::FreeMemory(req);
    }
}

} // namespace ZHLN::Test
