// Archive reading and extraction.
//
// tar is parsed here rather than shelled out to `tar -x`, for two reasons that
// both matter on Windows: System32's bsdtar is not something a package manager
// should depend on, and `tar -x` performs the extraction itself -- so the
// validation afterwards would be a reconstruction of what already happened
// instead of a decision made before a single byte is written.
//
// The order is therefore: Scan() validates the whole archive and reads its
// metadata; ExtractInto() re-reads it and writes. Nothing touches the store
// until the archive has been proven to be a Zahlen package that stays inside
// usr/.
#include "zpkg/zpkg.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>
#if defined(ZPKG_HAVE_ZSTD)
#include <zstd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

namespace ZHLN {

namespace {

constexpr size_t BlockSize = 512;

// ---------------------------------------------------------------------------
// Decompressing byte stream
// ---------------------------------------------------------------------------
class ByteStream {
  public:
    // A factory rather than a constructor: opening an archive reads its magic
    // bytes, and with no exceptions that is an operation that can fail.
    [[nodiscard]] static auto Open(const fs::path& path) -> std::expected<std::unique_ptr<ByteStream>, Fail> {
        auto file = InputFile::Open(path);
        if (!file)
            return std::unexpected(std::move(file.error()));
        auto stream = std::unique_ptr<ByteStream>(new ByteStream(std::move(*file)));
        auto ready  = stream->DetectCompression();
        if (!ready)
            return std::unexpected(std::move(ready.error()));
        return stream;
    }

    ~ByteStream() {
        if (m_mode == Mode::Gzip)
            inflateEnd(&m_gz);
#if defined(ZPKG_HAVE_ZSTD)
        if (m_mode == Mode::Zstd && m_zds != nullptr)
            ZSTD_freeDStream(m_zds);
#endif
    }

    // Returns the number of bytes read; fewer than requested only at EOF.
    auto Read(char* destination, size_t wanted) -> std::expected<size_t, Fail> {
        switch (m_mode) {
            case Mode::Plain:
                return ReadPlain(destination, wanted);
            case Mode::Gzip:
                return ReadGzip(destination, wanted);
            case Mode::Zstd:
#if defined(ZPKG_HAVE_ZSTD)
                return ReadZstd(destination, wanted);
#else
                return 0;
#endif
        }
        return 0;
    }

    auto ReadExactly(char* destination, size_t wanted, const char* what) -> std::expected<void, Fail> {
        auto got = Read(destination, wanted);
        if (!got)
            return std::unexpected(std::move(got.error()));
        if (*got != wanted) {
            return std::unexpected(Fail {std::string("truncated archive while reading ") + what});
        }
        return {};
    }

    auto Skip(uint64_t count) -> std::expected<void, Fail> {
        std::array<char, 1 << 16> scratch {};
        while (count > 0) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(count, scratch.size()));
            auto         got  = Read(scratch.data(), take);
            if (!got)
                return std::unexpected(std::move(got.error()));
            if (*got == 0)
                return std::unexpected(Fail {"truncated archive while skipping file data"});
            count -= *got;
        }
        return {};
    }

  private:
    explicit ByteStream(InputFile file): m_file(std::move(file)) {
    }

    auto DetectCompression() -> std::expected<void, Fail> {
        std::array<unsigned char, 6> magic {};
        auto                         probe = m_file.ReadSome(reinterpret_cast<char*>(magic.data()), magic.size());
        if (!probe)
            return std::unexpected(std::move(probe.error()));
        const size_t got = *probe;
        m_file.Rewind();

        if (got >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
            m_mode = Mode::Gzip;
            std::memset(&m_gz, 0, sizeof(m_gz));
            if (inflateInit2(&m_gz, 15 + 32) != Z_OK) {
                return std::unexpected(Fail {"cannot initialise zlib"});
            }
        } else if (got >= 4 && magic[0] == 0x28 && magic[1] == 0xb5 && magic[2] == 0x2f && magic[3] == 0xfd) {
#if defined(ZPKG_HAVE_ZSTD)
            m_mode = Mode::Zstd;
            m_zds  = ZSTD_createDStream();
            if (m_zds == nullptr)
                return std::unexpected(Fail {"cannot initialise zstd"});
            ZSTD_initDStream(m_zds);
            m_zstdIn = {m_inBuffer.data(), 0, 0};
#else
            return std::unexpected(Fail {"this build has no zstd support; use the .tar.gz artifacts"});
#endif
        } else if (got >= 6 && magic[0] == 0xfd && magic[1] == '7' && magic[2] == 'z' && magic[3] == 'X' && magic[4] == 'Z' && magic[5] == 0x00) {
            return std::unexpected(Fail {"xz-compressed archives are not supported; use .tar.gz or .tar.zst"});
        } else {
            m_mode = Mode::Plain;
        }
        return {};
    }

    ByteStream(const ByteStream&)            = delete;
    ByteStream& operator=(const ByteStream&) = delete;

    enum class Mode { Plain, Gzip, Zstd };

    auto FillInput() -> std::expected<size_t, Fail> {
        if (m_fileEof)
            return 0;
        auto got = m_file.ReadSome(m_inBuffer.data(), m_inBuffer.size());
        if (!got)
            return std::unexpected(std::move(got.error()));
        if (*got < m_inBuffer.size())
            m_fileEof = true;
        return *got;
    }

    auto ReadPlain(char* destination, size_t wanted) -> std::expected<size_t, Fail> {
        return m_file.ReadSome(destination, wanted);
    }

    auto ReadGzip(char* destination, size_t wanted) -> std::expected<size_t, Fail> {
        m_gz.next_out  = reinterpret_cast<Bytef*>(destination);
        m_gz.avail_out = static_cast<uInt>(wanted);
        while (m_gz.avail_out > 0) {
            if (m_gz.avail_in == 0) {
                auto filled = FillInput();
                if (!filled)
                    return std::unexpected(std::move(filled.error()));
                if (*filled == 0)
                    break;
                m_gz.next_in  = reinterpret_cast<Bytef*>(m_inBuffer.data());
                m_gz.avail_in = static_cast<uInt>(*filled);
            }
            const int status = inflate(&m_gz, Z_NO_FLUSH);
            if (status == Z_STREAM_END) {
                m_inEnded = true;
                break;
            }
            if (status != Z_OK && status != Z_BUF_ERROR) {
                return std::unexpected(Fail {std::string("corrupt gzip stream: ") + (m_gz.msg ? m_gz.msg : "inflate")});
            }
            if (status == Z_BUF_ERROR && m_gz.avail_in == 0 && m_gz.avail_out == wanted)
                break;
        }
        return wanted - m_gz.avail_out;
    }

#if defined(ZPKG_HAVE_ZSTD)
    auto ReadZstd(char* destination, size_t wanted) -> std::expected<size_t, Fail> {
        ZSTD_outBuffer output {destination, wanted, 0};
        while (output.pos < output.size) {
            if (m_zstdIn.pos == m_zstdIn.size) {
                auto filled = FillInput();
                if (!filled)
                    return std::unexpected(std::move(filled.error()));
                if (*filled == 0)
                    break;
                m_zstdIn = {m_inBuffer.data(), *filled, 0};
            }
            const size_t status = ZSTD_decompressStream(m_zds, &output, &m_zstdIn);
            if (ZSTD_isError(status)) {
                return std::unexpected(Fail {std::string("corrupt zstd stream: ") + ZSTD_getErrorName(status)});
            }
            if (status == 0)
                break; // frame complete
        }
        return output.pos;
    }
#endif

    InputFile                 m_file;
    Mode                      m_mode    = Mode::Plain;
    bool                      m_fileEof = false;
    bool                      m_inEnded = false;
    std::array<char, 1 << 16> m_inBuffer {};
    z_stream                  m_gz {};
#if defined(ZPKG_HAVE_ZSTD)
    ZSTD_DStream* m_zds = nullptr;
    ZSTD_inBuffer m_zstdIn {nullptr, 0, 0};
#endif
};

// ---------------------------------------------------------------------------
// tar header parsing
// ---------------------------------------------------------------------------
auto ParseOctal(const char* field, size_t size) -> std::expected<uint64_t, Fail> {
    uint64_t value = 0;
    size_t   index = 0;
    while (index < size && (field[index] == ' ' || field[index] == '\0')) {
        if (field[index] == '\0')
            return value;
        ++index;
    }
    for (; index < size; ++index) {
        const char c = field[index];
        if (c == '\0' || c == ' ')
            break;
        if (c < '0' || c > '7')
            return std::unexpected(Fail {"invalid octal field in tar header"});
        value = value * 8 + static_cast<uint64_t>(c - '0');
    }
    return value;
}

// GNU/PAX tar uses base-256 for values that do not fit in the octal field.
auto ParseNumeric(const char* field, size_t size) -> std::expected<uint64_t, Fail> {
    if (static_cast<unsigned char>(field[0]) & 0x80u) {
        uint64_t value = static_cast<uint64_t>(field[0] & 0x7f);
        for (size_t i = 1; i < size; ++i) {
            value = (value << 8) | static_cast<uint64_t>(static_cast<unsigned char>(field[i]));
        }
        return value;
    }
    return ParseOctal(field, size);
}

std::string FieldString(const char* field, size_t size) {
    size_t length = 0;
    while (length < size && field[length] != '\0')
        ++length;
    return std::string(field, length);
}

// A name that does not fit the 100-byte header field travels in a preceding GNU
// 'L' (long name) or 'K' (long link) record. That record's size counts a
// trailing NUL, so the string we read carries one; a NUL is not part of a name,
// and keeping it would make ValidateMemberPath reject the writer's own archive
// for "carrying a control character".
void TrimAtNul(std::string& value) {
    if (const size_t nul = value.find('\0'); nul != std::string::npos)
        value.erase(nul);
}

// Paths quoted back to the user come straight out of the archive, so escape the
// bytes that would otherwise break the message apart: a refused control
// character has to be visible, not swallow the rest of the line.
std::string QuoteForMessage(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size());
    for (const char c: value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            constexpr std::string_view digits = "0123456789abcdef";
            quoted += "\\x";
            quoted += digits[byte >> 4];
            quoted += digits[byte & 0x0F];
            continue;
        }
        quoted += c;
    }
    return quoted;
}

bool BlockIsZero(const char* block) {
    for (size_t i = 0; i < BlockSize; ++i) {
        if (block[i] != '\0')
            return false;
    }
    return true;
}

// The checksum is the sum of all header bytes with the checksum field read as
// spaces. Both the signed and unsigned historical variants are accepted.
auto VerifyHeaderChecksum(const char* header) -> std::expected<void, Fail> {
    auto parsed = ParseOctal(header + 148, 8);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    const uint64_t expected    = *parsed;
    uint64_t       unsignedSum = 0;
    int64_t        signedSum   = 0;
    for (size_t i = 0; i < BlockSize; ++i) {
        const unsigned char byte = (i >= 148 && i < 156) ? static_cast<unsigned char>(' ') : static_cast<unsigned char>(header[i]);
        unsignedSum += byte;
        signedSum += static_cast<int8_t>(header[i]);
    }
    if (expected != unsignedSum && static_cast<uint64_t>(signedSum) != expected) {
        return std::unexpected(Fail {"corrupt tar header: checksum mismatch"});
    }
    return {};
}

// PAX extended headers carry records of the form "<len> <key>=<value>\n".
std::string PaxLookup(const std::string& records, const std::string& key) {
    size_t position = 0;
    while (position < records.size()) {
        const size_t space = records.find(' ', position);
        if (space == std::string::npos)
            break;
        size_t            length = 0;
        const std::string digits = records.substr(position, space - position);
        const auto        parsed = std::from_chars(digits.data(), digits.data() + digits.size(), length);
        if (parsed.ec != std::errc {} || parsed.ptr != digits.data() + digits.size())
            break;
        if (length == 0 || position + length > records.size())
            break;
        const std::string record = records.substr(space + 1, position + length - space - 2);
        const size_t      equals = record.find('=');
        if (equals != std::string::npos && record.compare(0, equals, key) == 0) {
            return record.substr(equals + 1);
        }
        position += length;
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Member path validation
// ---------------------------------------------------------------------------
namespace {

std::vector<std::string> SplitComponents(std::string_view path) {
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        parts.emplace_back(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return parts;
}

// A member path is accepted only if it is a plain relative path whose first
// component is "usr". Anything else -- absolute, drive-qualified, containing
// "..", containing a backslash (which is a separator on Windows and therefore
// an escape there), or carrying control characters -- is refused.
auto ValidateMemberPath(std::string_view raw, std::string_view archiveName) -> std::expected<std::string, Fail> {
    std::string path(raw);
    while (StartsWith(path, "./"))
        path.erase(0, 2);
    if (path.size() > 1 && path.back() == '/')
        path.pop_back();

    const auto refuse = [&](std::string_view why) -> std::unexpected<Fail> {
        return std::unexpected(Fail {std::string(archiveName) + " contains '" + QuoteForMessage(raw) + "', " + std::string(why)});
    };

    if (path.empty())
        return refuse("which is an empty path");
    if (path[0] == '/')
        return refuse("which is an absolute path");
    if (path.find('\\') != std::string::npos)
        return refuse("which uses a backslash separator");
    if (path.find(':') != std::string::npos)
        return refuse("which contains a colon");
    for (char c: path) {
        if (static_cast<unsigned char>(c) < 0x20) {
            return refuse("which contains a control character");
        }
    }

    const auto parts = SplitComponents(path);
    for (const auto& part: parts) {
        if (part.empty())
            return refuse("which has an empty path component");
        if (part == "." || part == "..")
            return refuse("which escapes the extraction root");
    }
    if (parts.empty() || parts[0] != "usr")
        return refuse("which is outside usr/");
    return path;
}

// A link target must stay inside the extraction root no matter where the archive
// puts the link. Checking it lexically is deliberate: the target may not exist
// yet, and "it exists now" is not a property an attacker cannot change later.
auto ValidateLinkTarget(const std::string& memberPath, const std::string& target, const std::string& archiveName) -> std::expected<void, Fail> {
    const auto refuse = [&](std::string_view why) -> std::unexpected<Fail> {
        return std::unexpected(Fail {archiveName + ": link '" + memberPath + "' -> '" + QuoteForMessage(target) + "' " + std::string(why)});
    };
    if (target.empty())
        return refuse("has an empty target");
    if (target[0] == '/')
        return refuse("has an absolute target");
    if (target.find('\\') != std::string::npos)
        return refuse("has a backslash in its target");
    if (target.find(':') != std::string::npos)
        return refuse("has a colon in its target");

    // Resolve the target relative to the link's directory without touching disk.
    fs::path resolved              = fs::path(memberPath).parent_path() / FromUtf8(target);
    resolved                       = resolved.lexically_normal();
    const std::string resolvedText = ToUtf8(resolved);
    if (StartsWith(resolvedText, "..") || !IsWithin(fs::path("usr"), resolved)) {
        return refuse("escapes usr/");
    }
    return {};
}

// Creates every parent directory of 'relative' below 'root', refusing to walk
// through a symbolic link that the archive itself may have created.
auto CreateParents(const fs::path& root, const std::string& relative, const std::string& archiveName) -> std::expected<void, Fail> {
    fs::path   current = root;
    const auto parts   = SplitComponents(relative);
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        current /= FromUtf8(parts[i]);
        std::error_code ec;
        const auto      status = fs::symlink_status(Native(current), ec);
        if (!ec && fs::is_symlink(status)) {
            return std::unexpected(Fail {archiveName + ": refusing to write through the symbolic link '" + ToUtf8(current) + "'"});
        }
        if (!fs::exists(status)) {
            auto made = EnsureDirectories(current);
            if (!made)
                return std::unexpected(std::move(made.error()));
        }
    }
    return {};
}

std::string MemberManifestName(const std::string& path) {
    constexpr std::string_view Prefix = "usr/share/zahlen/manifests/";
    if (!StartsWith(path, Prefix))
        return {};
    const std::string leaf = path.substr(Prefix.size());
    if (leaf.empty() || leaf.find('/') != std::string::npos)
        return {};
    if (!EndsWith(leaf, ".json"))
        return {};
    return leaf;
}

} // namespace

// ---------------------------------------------------------------------------
// Archive
// ---------------------------------------------------------------------------
Archive::Archive(fs::path path): m_path(std::move(path)) {
}

auto Archive::Open(fs::path path) -> std::expected<Archive, Fail> {
    Archive archive(std::move(path));
    // Opening here is what reports "no such archive" and "not a tar". The handle
    // is closed again immediately: an archive is not a thing that holds a file
    // open, which on Windows is also what lets the caller delete it afterwards.
    auto stream = ByteStream::Open(archive.m_path);
    if (!stream)
        return std::unexpected(std::move(stream.error()));
    return archive;
}

auto Archive::Scan() -> std::expected<void, Fail> {
    if (m_scanned)
        return {};
    auto walked = Walk(false, {});
    if (!walked)
        return std::unexpected(std::move(walked.error()));
    m_scanned = true;

    // A symbolic link that is a prefix of another member would let that member
    // be written through the link, outside the store. Collecting first makes
    // this a property of the archive instead of a race with extraction order.
    for (const auto& member: m_members) {
        if (member.type != '2' && member.type != '1')
            continue;
        const std::string prefix = member.path + "/";
        for (const auto& other: m_members) {
            if (&other == &member)
                continue;
            if (StartsWith(other.path, prefix)) {
                return std::unexpected(
                    Fail {ToUtf8(m_path) + ": link member '" + member.path + "' is a parent of '" + other.path + "', which would write through the link"}
                );
            }
        }
    }
    return {};
}

auto Archive::Walk(bool extract, const fs::path& destination) -> std::expected<void, Fail> {
    // An archive is read twice -- once to validate, once to extract -- and each
    // pass starts at the first block, so each pass opens its own stream and
    // closes it on the way out.
    auto stream = ByteStream::Open(m_path);
    if (!stream)
        return std::unexpected(std::move(stream.error()));
    ByteStream&       reader      = **stream;
    const std::string archiveName = ToUtf8(m_path);
    std::string       longName;
    std::string       longLink;
    std::string       paxPath;
    std::string       paxLink;
    size_t            expectedIndex = 0;
    bool              sawManifest   = false;
    bool              sawToolchain  = false;

    std::vector<char> fileBuffer(1 << 16);

    for (;;) {
        char header[BlockSize];
        auto read = reader.Read(header, BlockSize);
        if (!read)
            return std::unexpected(std::move(read.error()));
        const size_t got = *read;
        if (got == 0)
            break; // clean EOF without a zero block; tolerated
        if (got != BlockSize)
            return std::unexpected(Fail {archiveName + ": truncated tar header"});
        if (BlockIsZero(header))
            break;
        auto checked = VerifyHeaderChecksum(header);
        if (!checked)
            return std::unexpected(std::move(checked.error()));

        const char type        = header[156];
        auto       numericSize = ParseNumeric(header + 124, 12);
        if (!numericSize)
            return std::unexpected(std::move(numericSize.error()));
        const size_t size   = static_cast<size_t>(*numericSize);
        const size_t padded = (size + BlockSize - 1) / BlockSize * BlockSize;

        auto readData = [&](std::string& out) -> std::expected<void, Fail> {
            out.resize(size);
            if (size > 0) {
                auto exact = reader.ReadExactly(out.data(), size, "extended header");
                if (!exact)
                    return std::unexpected(std::move(exact.error()));
            }
            return reader.Skip(padded - size);
        };

        if (type == 'L') { // GNU long name
            auto stuffed = readData(longName);
            if (!stuffed)
                return std::unexpected(std::move(stuffed.error()));
            TrimAtNul(longName);
            continue;
        }
        if (type == 'K') { // GNU long link target
            auto stuffed = readData(longLink);
            if (!stuffed)
                return std::unexpected(std::move(stuffed.error()));
            TrimAtNul(longLink);
            continue;
        }
        if (type == 'x' || type == 'X') { // PAX per-file header
            std::string records;
            auto        stuffed = readData(records);
            if (!stuffed)
                return std::unexpected(std::move(stuffed.error()));
            if (auto value = PaxLookup(records, "path"); !value.empty()) {
                TrimAtNul(value);
                paxPath = value;
            }
            if (auto value = PaxLookup(records, "linkpath"); !value.empty()) {
                TrimAtNul(value);
                paxLink = value;
            }
            continue;
        }
        if (type == 'g') { // PAX global header: nothing we honour
            auto skipped = reader.Skip(padded);
            if (!skipped)
                return std::unexpected(std::move(skipped.error()));
            continue;
        }
        if (type == 'V' || type == 'v') { // volume label
            auto skipped = reader.Skip(padded);
            if (!skipped)
                return std::unexpected(std::move(skipped.error()));
            continue;
        }

        std::string rawPath;
        if (!paxPath.empty()) {
            rawPath = paxPath;
        } else {
            const std::string name   = longName.empty() ? FieldString(header, 100) : longName;
            const std::string prefix = FieldString(header + 345, 155);
            rawPath                  = prefix.empty() ? name : prefix + "/" + name;
        }
        std::string rawLink = !paxLink.empty() ? paxLink : !longLink.empty() ? longLink : FieldString(header + 157, 100);
        longName.clear();
        longLink.clear();
        paxPath.clear();
        paxLink.clear();

        char kind = '0';
        switch (type) {
            case '0':
            case '\0':
            case '7':
                kind = '0';
                break;
            case '5':
                kind = '5';
                break;
            case '2':
                kind = '2';
                break;
            case '1':
                kind = '1';
                break;
            case '3':
            case '4':
            case '6': {
                return std::unexpected(
                    Fail {archiveName + " contains '" + QuoteForMessage(rawPath) + "', which is a device or fifo and cannot be installed"}
                );
            }
            default:
                return std::unexpected(
                    Fail {archiveName + " contains '" + QuoteForMessage(rawPath) + "', which has an unsupported tar entry type"}
                );
        }

        auto validated = ValidateMemberPath(rawPath, archiveName);
        if (!validated)
            return std::unexpected(std::move(validated.error()));
        const std::string path = *validated;
        if (kind == '2' || kind == '1') {
            auto linked = ValidateLinkTarget(path, rawLink, archiveName);
            if (!linked)
                return std::unexpected(std::move(linked.error()));
        }

        if (!extract) {
            for (const auto& member: m_members) {
                if (member.path == path && (member.type != '5' || kind != '5')) {
                    return std::unexpected(Fail {archiveName + " lists '" + path + "' more than once"});
                }
            }
            m_members.push_back(Member {path, kind, rawLink, static_cast<uint64_t>(size)});

            const std::string manifestLeaf = MemberManifestName(path);
            if (!manifestLeaf.empty()) {
                if (sawManifest) {
                    return std::unexpected(Fail {
                        archiveName + " carries more than one package manifest; it is not a "
                                      "Zahlen package archive"
                    });
                }
                sawManifest = true;
                std::string json;
                json.resize(size);
                if (size > 0) {
                    auto exact = reader.ReadExactly(json.data(), size, "package manifest");
                    if (!exact)
                        return std::unexpected(std::move(exact.error()));
                }
                auto skipped = reader.Skip(padded - size);
                if (!skipped)
                    return std::unexpected(std::move(skipped.error()));
                auto manifest = ParseManifestJson(json, archiveName + " (" + manifestLeaf + ")");
                if (!manifest)
                    return std::unexpected(std::move(manifest.error()));
                m_manifest = std::move(*manifest);
                continue;
            }
            if (path == "usr/share/zahlen/toolchain.json") {
                sawToolchain = true;
                std::string json;
                json.resize(size);
                if (size > 0) {
                    auto exact = reader.ReadExactly(json.data(), size, "toolchain descriptor");
                    if (!exact)
                        return std::unexpected(std::move(exact.error()));
                }
                auto skipped = reader.Skip(padded - size);
                if (!skipped)
                    return std::unexpected(std::move(skipped.error()));
                auto toolchain = ParseToolchainJson(json, archiveName + " (toolchain.json)");
                if (!toolchain)
                    return std::unexpected(std::move(toolchain.error()));
                m_toolchain = std::move(*toolchain);
                continue;
            }
            auto skipped = reader.Skip(padded);
            if (!skipped)
                return std::unexpected(std::move(skipped.error()));
            continue;
        }

        // --- extraction pass -------------------------------------------------
        if (expectedIndex >= m_members.size() || m_members[expectedIndex].path != path) {
            return std::unexpected(Fail {archiveName + " changed while it was being installed"});
        }
        const fs::path target = destination / FromUtf8(path);
        if (!IsWithin(destination, target)) {
            return std::unexpected(Fail {archiveName + ": refusing to write outside the store: " + path});
        }

        if (kind == '5') {
            auto made = EnsureDirectories(target);
            if (!made)
                return std::unexpected(std::move(made.error()));
            ++expectedIndex;
            auto skipped = reader.Skip(padded);
            if (!skipped)
                return std::unexpected(std::move(skipped.error()));
            continue;
        }
        auto parents = CreateParents(destination, path, archiveName);
        if (!parents)
            return std::unexpected(std::move(parents.error()));

        if (kind == '0') {
            auto opened = OutputFile::Create(target);
            if (!opened)
                return std::unexpected(std::move(opened.error()));
            OutputFile& out       = *opened;
            uint64_t    remaining = size;
            while (remaining > 0) {
                const size_t take  = static_cast<size_t>(std::min<uint64_t>(remaining, fileBuffer.size()));
                auto         exact = reader.ReadExactly(fileBuffer.data(), take, path.c_str());
                if (!exact)
                    return std::unexpected(std::move(exact.error()));
                auto written = out.Write(fileBuffer.data(), take);
                if (!written)
                    return std::unexpected(std::move(written.error()));
                remaining -= take;
            }
            auto flushed = out.Flush();
            if (!flushed)
                return std::unexpected(std::move(flushed.error()));
            auto skipped = reader.Skip(padded - size);
            if (!skipped)
                return std::unexpected(std::move(skipped.error()));
            // The executable bit is the only permission bit carried over; setuid,
            // setgid and sticky are dropped because a package may not smuggle
            // privilege into the store.
            //
            // None of that exists on Windows, where std::filesystem reports the
            // same rw-rw-rw- for every file and can only express one bit -- the
            // read-only attribute. Setting it from an archive's mode would make a
            // store file that the next install could not replace, and would gain
            // nothing: executability there comes from the extension. Verified by
            // running the suite under Wine, which reports 0x1b6 after chmod 644
            // and after chmod 755 alike.
            auto mode = ParseOctal(header + 100, 8);
            if (!mode)
                return std::unexpected(std::move(mode.error()));
            [[maybe_unused]] const bool executable = (*mode & 0100u) != 0;
#if !defined(_WIN32)
            std::error_code ec;
            fs::permissions(
                Native(target),
                executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec :
                             fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                fs::perm_options::replace, ec
            );
#endif
        } else if (kind == '2') {
            std::error_code ec;
            fs::create_symlink(FromUtf8(rawLink), Native(target), ec);
            if (ec) {
                return std::unexpected(Fail {
                    "cannot create the symbolic link '" + path + "' in the store: " + ec.message() +
                    "\n       On Windows, symbolic links require developer mode or an "
                    "elevated shell; ask the publisher for a package without links."
                });
            }
            reader.Skip(padded);
        } else if (kind == '1') {
            const fs::path source = destination / FromUtf8(rawLink);
            if (!IsWithin(destination, source)) {
                return std::unexpected(Fail {archiveName + ": hard link target escapes the store: " + rawLink});
            }
            std::error_code ec;
            fs::create_hard_link(Native(source), Native(target), ec);
            if (ec) {
                // Different filesystems or Windows: fall back to a copy, which
                // is what a hard link means for an immutable payload.
                fs::copy_file(Native(source), Native(target), fs::copy_options::overwrite_existing, ec);
                if (ec)
                    return std::unexpected(Fail {"cannot link '" + path + "': " + ec.message()});
            }
            reader.Skip(padded);
        }
        ++expectedIndex;
    }

    if (!extract) {
        if (!sawToolchain) {
            return std::unexpected(Fail {
                archiveName + " carries no usr/share/zahlen/toolchain.json, so zpkg cannot name the "
                              "toolchain it belongs to.\n       Build packages with "
                              "-DZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON."
            });
        }
        if (!sawManifest) {
            return std::unexpected(Fail {
                archiveName + " carries no usr/share/zahlen/manifests/*.json, so zpkg cannot tell what "
                              "package it is."
            });
        }
        if (m_members.empty())
            return std::unexpected(Fail {archiveName + " contains no files"});
    } else if (expectedIndex != m_members.size()) {
        return std::unexpected(Fail {archiveName + " changed while it was being installed"});
    }
    return {};
}

auto Archive::ExtractInto(const fs::path& destination) -> std::expected<void, Fail> {
    if (!m_scanned)
        return std::unexpected(Fail {"ExtractInto() called before Scan()"});
    auto made = EnsureDirectories(destination);
    if (!made)
        return std::unexpected(std::move(made.error()));
    return Walk(true, destination);
}

} // namespace ZHLN
