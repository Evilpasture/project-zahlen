// Platform rules, filesystem helpers, SHA-256, base64 and the store lock.
//
// Everything here is dependency-free on purpose: this file is what lets zpkg be
// the one Zahlen binary that builds and runs on a machine with no Zahlen
// toolchain, no Vulkan and no particular CPU.
#include "zpkg/zpkg.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ZHLN {

namespace {

bool quietFlag = false;

} // namespace

namespace log {

void SetQuiet(bool quiet) {
    quietFlag = quiet;
}

void Say(const std::string& line) {
    if (quietFlag)
        return;
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
}

void Blank() {
    Say("");
}

void Warn(const std::string& line) {
    std::fprintf(stderr, "zpkg: warning: %s\n", line.c_str());
}

void Note(const std::string& line) {
    std::fprintf(stderr, "zpkg: %s\n", line.c_str());
}

} // namespace log

// ---------------------------------------------------------------------------
// Strings and paths
// ---------------------------------------------------------------------------
std::string ToUtf8(const fs::path& path) {
#if defined(_WIN32)
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return path.string();
#endif
}

fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// std::filesystem on Windows still refuses paths past MAX_PATH unless the
// extended-length form is used. A per-user store can easily get there: the
// toolchain id alone is ~50 characters before usr/include/Zahlen/... begins.
fs::path Native(const fs::path& path) {
#if defined(_WIN32)
    // Tar member names are always '/'-separated and a std::filesystem::path
    // built from one keeps them; Win32 accepts those, the extended-length form
    // does not, so they are normalised first. (Both of these were found by
    // running the suite under Wine.)
    std::string text = ToUtf8(path);
    for (char& c: text) {
        if (c == '/')
            c = '\\';
    }
    // The prefix is only for paths that need it. Everything shorter stays plain,
    // because a good part of the C runtime -- mkdir behind create_directories,
    // for one -- answers EINVAL to "\\\\?\C:\dir" while CreateFileW and _wfopen
    // are happy with it. 240 leaves room for a filename tail under MAX_PATH.
    constexpr size_t PrefixAbove = 240;
    if (text.size() <= PrefixAbove)
        return FromUtf8(text);
    if (text.size() >= 4 && text.compare(0, 4, R"(\\?\)") == 0)
        return FromUtf8(text);
    if (text.compare(0, 2, R"(\\)") == 0)
        return FromUtf8(R"(\\?\UNC\)" + text.substr(2));
    if (text.size() > 2 && text[1] == ':')
        return FromUtf8(R"(\\?\)" + text);
    return FromUtf8(text);
#else
    return path;
#endif
}

// A store lives under a user directory, so publishing or adding a remote from a
// path is the common case, and the URL has to be one this program can read back.
// On Windows that means file:///C:/dir with forward slashes: file://C:\dir is
// three characters of scheme followed by a host called "C".
std::string ToFileUrl(const fs::path& path) {
    std::string text = ToUtf8(fs::absolute(path).lexically_normal());
    for (char& c: text) {
        if (c == '\\')
            c = '/';
    }
    std::string url = "file://";
    if (text.empty() || text[0] != '/')
        url += '/';
    url += text;
    return url;
}

std::string AsciiLower(std::string text) {
    for (char& c: text) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Trim(std::string_view text) {
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool IsWithin(const fs::path& base, const fs::path& child) {
    fs::path normalBase  = base.lexically_normal();
    fs::path normalChild = child.lexically_normal();
    auto     b           = normalBase.begin();
    auto     c           = normalChild.begin();
    for (; b != normalBase.end(); ++b, ++c) {
        if (c == normalChild.end())
            return false;
        if (*b != *c)
            return false;
    }
    return true;
}

auto EnsureDirectories(const fs::path& path) -> std::expected<void, Fail> {
#if defined(_WIN32)
    // Component by component with CreateDirectoryW: fs::create_directories would
    // reject any prefix long enough to need one, and the store needs one for deep
    // include trees. Each component is built plain when it is short and in
    // extended form when it is not, which is what Native() already decides.
    const std::wstring full        = path.wstring();
    DWORD              first_error = 0;
    for (size_t end = 0; end <= full.size(); ++end) {
        if (end != full.size() && full[end] != L'\\')
            continue;
        const std::wstring component = full.substr(0, end);
        if (component.empty() || component.back() == L':')
            continue;
        if (!CreateDirectoryW(Native(fs::path(component)).wstring().c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && first_error == 0)
                first_error = error;
        }
    }
    std::error_code ec;
    if (fs::is_directory(full, ec))
        return {};
    return std::unexpected(Fail {"cannot create " + ToUtf8(path) + (first_error != 0 ? " (Windows error " + std::to_string(first_error) + ")" : "")});
#else
    std::error_code ec;
    fs::create_directories(Native(path), ec);
    if (ec && !fs::is_directory(Native(path), ec)) {
        return std::unexpected(Fail {"cannot create " + ToUtf8(path) + ": " + ec.message()});
    }
#endif
    return {};
}

namespace {

std::FILE* OpenNative(const fs::path& path, const char* mode) {
#if defined(_WIN32)
    const std::wstring wide_mode(mode, mode + std::strlen(mode));
    return _wfopen(Native(path).wstring().c_str(), wide_mode.c_str());
#else
    return std::fopen(Native(path).c_str(), mode);
#endif
}

} // namespace

struct InputFile::Impl {
    std::FILE* file = nullptr;
    fs::path   path;
};

auto InputFile::Open(const fs::path& path) -> std::expected<InputFile, Fail> {
    auto* impl = new Impl;
    impl->path = path;
    impl->file = OpenNative(path, "rb");
    if (impl->file == nullptr) {
        delete impl;
        return std::unexpected(Fail {"cannot read " + ToUtf8(path)});
    }
    return InputFile(impl);
}

InputFile::InputFile(Impl* impl): m_impl(impl) {
}

InputFile::~InputFile() {
    if (m_impl == nullptr)
        return;
    if (m_impl->file != nullptr)
        std::fclose(m_impl->file);
    delete m_impl;
}

InputFile::InputFile(InputFile&& other) noexcept: m_impl(other.m_impl) {
    other.m_impl = nullptr;
}

InputFile& InputFile::operator=(InputFile&& other) noexcept {
    if (this != &other) {
        this->~InputFile();
        m_impl       = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

auto InputFile::ReadSome(char* buffer, size_t count) -> std::expected<size_t, Fail> {
    if (m_impl == nullptr || m_impl->file == nullptr)
        return 0;
    const size_t got = std::fread(buffer, 1, count, m_impl->file);
    if (got == 0 && std::ferror(m_impl->file) != 0) {
        return std::unexpected(Fail {"read error on " + ToUtf8(m_impl->path)});
    }
    return got;
}

void InputFile::Rewind() {
    if (m_impl == nullptr || m_impl->file == nullptr)
        return;
    std::rewind(m_impl->file);
    std::clearerr(m_impl->file);
}

std::FILE* InputFile::Handle() const {
    return m_impl != nullptr ? m_impl->file : nullptr;
}

struct OutputFile::Impl {
    std::FILE* file = nullptr;
    fs::path   path;
};

auto OutputFile::Create(const fs::path& path) -> std::expected<OutputFile, Fail> {
    if (path.has_parent_path()) {
        auto made = EnsureDirectories(path.parent_path());
        if (!made)
            return std::unexpected(std::move(made.error()));
    }
    auto* impl = new Impl;
    impl->path = path;
    impl->file = OpenNative(path, "wb");
    if (impl->file == nullptr) {
        delete impl;
        return std::unexpected(Fail {"cannot write " + ToUtf8(path)});
    }
    return OutputFile(impl);
}

OutputFile::OutputFile(Impl* impl): m_impl(impl) {
}

OutputFile::~OutputFile() {
    if (m_impl == nullptr)
        return;
    if (m_impl->file != nullptr)
        std::fclose(m_impl->file);
    delete m_impl;
}

OutputFile::OutputFile(OutputFile&& other) noexcept: m_impl(other.m_impl) {
    other.m_impl = nullptr;
}

OutputFile& OutputFile::operator=(OutputFile&& other) noexcept {
    if (this != &other) {
        this->~OutputFile();
        m_impl       = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

auto OutputFile::Write(const char* data, size_t count) -> std::expected<void, Fail> {
    if (m_impl == nullptr || m_impl->file == nullptr)
        return std::unexpected(Fail {"file is not open for writing"});
    if (count == 0)
        return {};
    if (std::fwrite(data, 1, count, m_impl->file) != count) {
        return std::unexpected(Fail {"cannot write " + ToUtf8(m_impl->path)});
    }
    return {};
}

auto OutputFile::Flush() -> std::expected<void, Fail> {
    if (m_impl == nullptr || m_impl->file == nullptr)
        return {};
    if (std::fflush(m_impl->file) != 0)
        return std::unexpected(Fail {"cannot write " + ToUtf8(m_impl->path)});
    return {};
}

std::FILE* OutputFile::Handle() const {
    return m_impl != nullptr ? m_impl->file : nullptr;
}

auto ReadFile(const fs::path& path) -> std::expected<std::string, Fail> {
    auto file = InputFile::Open(path);
    if (!file)
        return std::unexpected(std::move(file.error()));
    std::string               bytes;
    std::array<char, 1 << 16> chunk {};
    for (;;) {
        auto got = file->ReadSome(chunk.data(), chunk.size());
        if (!got)
            return std::unexpected(std::move(got.error()));
        if (*got == 0)
            break;
        bytes.append(chunk.data(), *got);
    }
    return bytes;
}

auto WriteFile(const fs::path& path, std::string_view bytes) -> std::expected<void, Fail> {
    return OutputFile::Create(path).and_then([bytes](OutputFile file) -> std::expected<void, Fail> {
        return file.Write(bytes.data(), bytes.size()).and_then([&file]() { return file.Flush(); });
    });
}

auto RemoveTree(const fs::path& path) -> std::expected<void, Fail> {
    std::error_code ec;
    // Windows marks installed files read-only; clear that before unlinking.
    for (auto it = fs::recursive_directory_iterator(Native(path), ec); !ec && it != fs::recursive_directory_iterator(); ++it) {
        std::error_code ignored;
        fs::permissions(it->path(), fs::perms::owner_all | fs::perms::group_read, fs::perm_options::add, ignored);
    }
    fs::remove_all(Native(path), ec);
    if (ec)
        return std::unexpected(Fail {"cannot remove " + ToUtf8(path) + ": " + ec.message()});
    return {};
}

auto FileSizeOf(const fs::path& path) -> std::expected<uint64_t, Fail> {
    std::error_code ec;
    const auto      size = fs::file_size(Native(path), ec);
    if (ec)
        return std::unexpected(Fail {"cannot stat " + ToUtf8(path)});
    return size;
}

bool PathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(Native(path), ec);
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------
namespace {

std::optional<std::string> EnvVar(const char* name) {
#if defined(_WIN32)
    char        buffer[32768];
    const DWORD size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (size == 0 || size >= sizeof(buffer))
        return std::nullopt;
    return std::string(buffer, size);
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
#endif
}

bool ProcessIsElevated() {
#if defined(_WIN32)
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation {};
    DWORD           size     = sizeof(elevation);
    const bool      elevated = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
#else
    return ::geteuid() == 0;
#endif
}

// A store root that is a system prefix would silently reintroduce exactly the
// privileged model this tool exists to avoid, so it is refused even for an
// explicit ZHLN_ROOT.
bool IsSystemPath(const fs::path& root) {
    const std::string text = AsciiLower(ToUtf8(root.lexically_normal()));
#if defined(_WIN32)
    const char* names[] = {"systemroot", "windir", "programfiles", "programfiles(x86)", "programdata", "allusersprofile"};
    for (const char* name: names) {
        const auto value = EnvVar(name);
        if (!value)
            continue;
        const std::string prefix = AsciiLower(*value);
        if (text == prefix || StartsWith(text, prefix + "\\") || StartsWith(text, prefix + "/")) {
            return true;
        }
    }
    return false;
#else
    static const char* const ForbiddenSystemPaths[] = {"/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32", "/usr",
                                                       "/opt", "/etc",  "/var", "/boot",  "/root",  "/snap"};
    for (const char* prefix: ForbiddenSystemPaths) {
        const std::string p(prefix);
        if (text == p || StartsWith(text, p + "/"))
            return true;
    }
    return false;
#endif
}

} // namespace

auto ResolveEnv(bool force) -> std::expected<Env, Fail> {
    Env        env;
    const auto explicitRoot = EnvVar("ZHLN_ROOT");
    env.rootWasExplicit     = explicitRoot.has_value();
    env.elevated            = ProcessIsElevated();

    if (explicitRoot) {
        env.root = FromUtf8(*explicitRoot);
        if (ToUtf8(env.root.lexically_normal()) == "/" || ToUtf8(env.root.lexically_normal()).empty()) {
            return std::unexpected(Fail {"ZHLN_ROOT must not be the filesystem root"});
        }
    } else {
        if (env.elevated) {
            return std::unexpected(Fail {"refusing to run elevated: zpkg installs into a user's data directory.\n"
                                         "       Run it as the developer, or set ZHLN_ROOT to an explicit scratch path."});
        }
#if defined(_WIN32)
        const auto local   = EnvVar("LOCALAPPDATA");
        const auto profile = EnvVar("USERPROFILE");
        if (local) {
            env.root = FromUtf8(*local) / "zahlen";
        } else if (profile) {
            env.root = FromUtf8(*profile) / "AppData" / "Local" / "zahlen";
        } else {
            return std::unexpected(Fail {"LOCALAPPDATA is not set; pass ZHLN_ROOT"});
        }
#else
        if (const auto xdg = EnvVar("XDG_DATA_HOME")) {
            env.root = FromUtf8(*xdg) / "zahlen";
        } else if (const auto home = EnvVar("HOME")) {
            env.root = FromUtf8(*home) / ".local" / "share" / "zahlen";
        } else {
            return std::unexpected(Fail {"HOME is not set; pass ZHLN_ROOT"});
        }
#endif
    }

    env.root = env.root.lexically_normal();
    if (IsSystemPath(env.root)) {
        if (!force) {
            return std::unexpected(Fail {
                "refusing to use the system path '" + ToUtf8(env.root) +
                "' as a store root.\n       Engine packages belong in a directory you own, "
                "not in a system prefix."
            });
        }
        log::Warn("using system path '" + ToUtf8(env.root) + "' because --force was given");
    }

    if (const auto bin = EnvVar("ZHLN_BIN_DIR")) {
        env.binDir = FromUtf8(*bin);
#if defined(_WIN32)
    } else {
        // Windows has no per-user bin directory on PATH by default; the store
        // owns its own and `zpkg env` puts it on PATH for the session.
        env.binDir = env.root / "bin";
    }
#else
    } else if (const auto home = EnvVar("HOME")) {
        env.binDir = FromUtf8(*home) / ".local" / "bin";
    } else {
        env.binDir = env.root / "bin";
    }
#endif
    env.binDir = env.binDir.lexically_normal();
    return env;
}

// ---------------------------------------------------------------------------
// Store lock
// ---------------------------------------------------------------------------
struct StoreLock::Impl {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};

auto StoreLock::Acquire(const fs::path& root) -> std::expected<StoreLock, Fail> {
    auto made = EnsureDirectories(root);
    if (!made)
        return std::unexpected(std::move(made.error()));
    auto*          state    = new Impl;
    const fs::path lockPath = root / ".lock";
    // Every failure below has to release what was already acquired: with no
    // unwinding, a raw new is only safe if each exit path says what it frees.
    const auto giveUp = [&state](std::unexpected<Fail> failure) -> std::expected<StoreLock, Fail> {
        delete state;
        return failure;
    };
#if defined(_WIN32)
    // An exclusive byte-range lock rather than a share-mode=0 open. Share mode
    // alone would make *any* other handle to the file -- an indexer, a virus
    // scanner, an editor -- look like a second zpkg, and the diagnosis would be
    // wrong. LockFileEx is the flock() equivalent that stays narrow: it fails
    // only if somebody else actually holds the lock.
    state->handle = CreateFileW(
        Native(lockPath).wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (state->handle == INVALID_HANDLE_VALUE) {
        return giveUp(std::unexpected(Fail {"cannot create the store lock in " + ToUtf8(root) + " (Windows error " + std::to_string(GetLastError()) + ")"}));
    }
    OVERLAPPED overlapped {};
    if (!LockFileEx(state->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
        const DWORD error = GetLastError();
        CloseHandle(state->handle);
        if (error == ERROR_LOCK_VIOLATION) {
            return giveUp(std::unexpected(Fail {"another zpkg is already working on " + ToUtf8(root)}));
        }
        return giveUp(std::unexpected(Fail {"cannot lock " + ToUtf8(lockPath) + " (Windows error " + std::to_string(error) + ")"}));
    }
#else
    state->fd = ::open(ToUtf8(lockPath).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (state->fd < 0) {
        return giveUp(std::unexpected(Fail {"cannot create the store lock in " + ToUtf8(root)}));
    }
    if (::flock(state->fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(state->fd);
        state->fd = -1;
        return giveUp(std::unexpected(Fail {"another zpkg is already working on " + ToUtf8(root)}));
    }
#endif
    (void) giveUp;
    return StoreLock(state);
}

StoreLock::StoreLock(Impl* impl): m_impl(impl) {
}

StoreLock::StoreLock(StoreLock&& other) noexcept: m_impl(other.m_impl) {
    other.m_impl = nullptr;
}

StoreLock& StoreLock::operator=(StoreLock&& other) noexcept {
    if (this != &other) {
        this->~StoreLock();
        m_impl       = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

StoreLock::~StoreLock() {
    if (m_impl == nullptr)
        return;
#if defined(_WIN32)
    if (m_impl->handle != INVALID_HANDLE_VALUE) {
        OVERLAPPED overlapped {};
        UnlockFileEx(m_impl->handle, 0, 1, 0, &overlapped);
        CloseHandle(m_impl->handle);
    }
#else
    if (m_impl->fd >= 0) {
        ::flock(m_impl->fd, LOCK_UN);
        ::close(m_impl->fd);
    }
#endif
    delete m_impl;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t Rotr(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void Sha256Block(uint32_t state[8], const uint8_t block[64]) {
    static constexpr uint32_t RoundConstants[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                                                    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                                                    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                                                    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                                                    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                                                    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                                                    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                                                    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) | (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1  = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const uint32_t ch  = (e & f) ^ (~e & g);
        const uint32_t t1  = h + s1 + ch + RoundConstants[i] + w[i];
        const uint32_t s0  = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2  = s0 + maj;
        h                  = g;
        g                  = f;
        f                  = e;
        e                  = d + t1;
        d                  = c;
        c                  = b;
        b                  = a;
        a                  = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

struct Sha256State {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t  buffer[64] {};
    size_t   buffered = 0;
    uint64_t total    = 0;
};

void Sha256Update(Sha256State& state, const uint8_t* data, size_t size) {
    state.total += size;
    while (size > 0) {
        const size_t take = std::min(size, sizeof(state.buffer) - state.buffered);
        std::memcpy(state.buffer + state.buffered, data, take);
        state.buffered += take;
        data += take;
        size -= take;
        if (state.buffered == sizeof(state.buffer)) {
            Sha256Block(state.h, state.buffer);
            state.buffered = 0;
        }
    }
}

std::string Sha256Final(Sha256State& state) {
    const uint64_t bits = state.total * 8;
    const uint8_t  pad  = 0x80;
    Sha256Update(state, &pad, 1);
    const uint8_t zero = 0;
    while (state.buffered != 56)
        Sha256Update(state, &zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    Sha256Update(state, length, 8);

    static constexpr char HexDigits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(64);
    for (uint32_t word: state.h) {
        for (int shift = 28; shift >= 0; shift -= 4)
            out.push_back(HexDigits[(word >> shift) & 0xfu]);
    }
    return out;
}

} // namespace

std::string Sha256Hex(std::string_view bytes) {
    Sha256State state;
    Sha256Update(state, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    return Sha256Final(state);
}

auto Sha256FileHex(const fs::path& path) -> std::expected<std::string, Fail> {
    auto in = InputFile::Open(path);
    if (!in)
        return std::unexpected(std::move(in.error()));
    Sha256State               state;
    std::array<char, 1 << 16> chunk {};
    for (;;) {
        auto got = in->ReadSome(chunk.data(), chunk.size());
        if (!got)
            return std::unexpected(std::move(got.error()));
        if (*got == 0)
            break;
        Sha256Update(state, reinterpret_cast<const uint8_t*>(chunk.data()), *got);
    }
    return Sha256Final(state);
}

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------
namespace {

constexpr std::string_view Base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

} // namespace

std::string Base64Encode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < size ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                               (i + 2 < size ? static_cast<uint32_t>(data[i + 2]) : 0);
        out.push_back(Base64Alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(Base64Alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? Base64Alphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? Base64Alphabet[chunk & 0x3f] : '=');
    }
    return out;
}

auto Base64Decode(std::string_view text) -> std::expected<std::vector<uint8_t>, Fail> {
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t accumulator = 0;
    int      bits        = 0;
    for (char c: text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        const int value = Base64Value(c);
        if (value < 0)
            return std::unexpected(Fail {"invalid base64 input"});
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    return out;
}

} // namespace ZHLN
