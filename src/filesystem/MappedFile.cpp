// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/FileSystem/MappedFile.hpp>
#include <filesystem>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ZHLN::FS {

MappedFile::~MappedFile() noexcept {
    Reset();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : _data(std::exchange(other._data, nullptr)),
      _size(std::exchange(other._size, 0)),
      _access(std::exchange(other._access, MapAccess::ReadOnly))
#if defined(_WIN32)
      ,
      _fileHandle(std::exchange(other._fileHandle, nullptr)),
      _mappingHandle(std::exchange(other._mappingHandle, nullptr))
#else
      ,
      _fd(std::exchange(other._fd, -1))
#endif
{
}

auto MappedFile::operator=(MappedFile&& other) noexcept -> MappedFile& {
    if (this != &other) {
        Reset();
        _data   = std::exchange(other._data, nullptr);
        _size   = std::exchange(other._size, 0);
        _access = std::exchange(other._access, MapAccess::ReadOnly);
#if defined(_WIN32)
        _fileHandle    = std::exchange(other._fileHandle, nullptr);
        _mappingHandle = std::exchange(other._mappingHandle, nullptr);
#else
        _fd = std::exchange(other._fd, -1);
#endif
    }
    return *this;
}

auto MappedFile::Open(std::string_view filepath, MapAccess access) noexcept -> MappedFile {
    MappedFile out;
    if (filepath.empty()) {
        return out;
    }

#if defined(_WIN32)
    std::error_code       ec;
    std::filesystem::path fsPath(filepath);
    if (ec) {
        return out;
    }

    DWORD desiredAccess = GENERIC_READ;
    DWORD shareMode     = FILE_SHARE_READ;
    DWORD protect       = PAGE_READONLY;
    DWORD mapDesired    = FILE_MAP_READ;

    if (access == MapAccess::ReadWrite) {
        desiredAccess = GENERIC_READ | GENERIC_WRITE;
        protect       = PAGE_READWRITE;
        mapDesired    = FILE_MAP_WRITE;
    } else if (access == MapAccess::CopyOnWrite) {
        desiredAccess = GENERIC_READ;
        protect       = PAGE_WRITECOPY;
        mapDesired    = FILE_MAP_COPY;
    }

    HANDLE hFile = CreateFileW(fsPath.c_str(), desiredAccess, shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return out;
    }

    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
        CloseHandle(hFile);
        return out;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, protect, 0, 0, nullptr);
    if (hMap == nullptr) {
        CloseHandle(hFile);
        return out;
    }

    void* view = MapViewOfFile(hMap, mapDesired, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return out;
    }

    out._data          = view;
    out._size          = static_cast<size_t>(fileSize.QuadPart);
    out._access        = access;
    out._fileHandle    = hFile;
    out._mappingHandle = hMap;
    return out;
#else
    const std::string pathStr(filepath);

    int openFlags = O_RDONLY;
    int prot      = PROT_READ;
    int mapFlags  = MAP_PRIVATE;

    if (access == MapAccess::ReadWrite) {
        openFlags = O_RDWR;
        prot      = PROT_READ | PROT_WRITE;
        mapFlags  = MAP_SHARED;
    } else if (access == MapAccess::CopyOnWrite) {
        openFlags = O_RDONLY;
        prot      = PROT_READ | PROT_WRITE;
        mapFlags  = MAP_PRIVATE;
    }

#if defined(O_CLOEXEC)
    openFlags |= O_CLOEXEC;
#endif

    const int fd = ::open(pathStr.c_str(), openFlags);
    if (fd < 0) {
        return out;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ::close(fd);
        return out;
    }

    const auto byteSize = static_cast<size_t>(st.st_size);
    void*      view     = ::mmap(nullptr, byteSize, prot, mapFlags, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        return out;
    }

    out._data   = view;
    out._size   = byteSize;
    out._access = access;
    out._fd     = fd;
    return out;
#endif
}

void MappedFile::Reset() noexcept {
#if defined(_WIN32)
    if (_data != nullptr) {
        UnmapViewOfFile(_data);
        _data = nullptr;
    }
    if (_mappingHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(_mappingHandle));
        _mappingHandle = nullptr;
    }
    if (_fileHandle != nullptr && _fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(_fileHandle));
        _fileHandle = nullptr;
    }
#else
    if (_data != nullptr) {
        ::munmap(_data, _size);
        _data = nullptr;
    }
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
#endif
    _size   = 0;
    _access = MapAccess::ReadOnly;
}

} // namespace ZHLN::FS
