// zpkg -- the Zahlen toolchain manager. Public surface.
//
// The whole program is these declarations plus seven translation units. There is
// no daemon, no database and no dependency solver: an archive states what it is,
// where it goes and what it needs, and zpkg puts it there.
//
// Nothing here throws. zpkg is built with -fno-exceptions, the same policy
// zahlen_engine passes down to zcook, so every operation that can fail returns
// std::expected and every caller decides: propagate, or report and stop. The
// compiler enforces the policy; the alternative (promise it in a comment) is how
// this file ended up with 128 throw statements that nobody's build could catch.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN {

namespace fs = std::filesystem;

// Manifests and toolchain.json are format 1 and always have been: they carry
// requires[] from the first release. index.json is the document that did not,
// and format 2 is the one that does -- see IndexPackage below.
inline constexpr int              PackageFormat = 1;
inline constexpr int              IndexFormat   = 2;
inline constexpr std::string_view ToolVersion   = "2.0.0";
inline constexpr std::string_view SignatureFile = "SHA256SUMS.ed25519";
inline constexpr std::string_view SumsFile      = "SHA256SUMS";

// ---------------------------------------------------------------------------
// Failures
// ---------------------------------------------------------------------------
// A failure is a value carrying the text a developer can act on, plus the exit
// status main() should use if the operation reached that far. There is exactly
// one error type on purpose: an installer has nothing useful to tell apart by
// category, and a caller that wants to react differently tests the operation it
// called, not the kind of error that came back.
struct Fail {
    std::string message;
    int         code = 1;
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
namespace log {
void SetQuiet(bool quiet);
void Say(const std::string& line); // stdout, suppressed by --quiet
void Blank();
void Warn(const std::string& line); // stderr
void Note(const std::string& line); // stderr, not an error
} // namespace log

// ---------------------------------------------------------------------------
// Platform and filesystem
// ---------------------------------------------------------------------------
struct Env {
    fs::path root;   // store root
    fs::path binDir; // shim directory
    bool     rootWasExplicit = false;
    bool     elevated        = false;
};

// Resolves $ZHLN_ROOT/$ZHLN_BIN_DIR or the platform default, and enforces the
// rules that make "unprivileged only" true rather than aspirational: no default
// root for an elevated process, and never a system path as a root.
auto ResolveEnv(bool force) -> std::expected<Env, Fail>;

fs::path    Native(const fs::path& path); // long-path form on Windows
std::string ToUtf8(const fs::path& path);
fs::path    FromUtf8(std::string_view text);
std::string ToFileUrl(const fs::path& path); // file:///C:/dir, forward slashes
std::string AsciiLower(std::string text);
bool        EndsWith(std::string_view text, std::string_view suffix);
bool        StartsWith(std::string_view text, std::string_view prefix);
std::string Trim(std::string_view text);

// Lexical containment: both paths are normalised in place and 'child' must have
// 'base' as a component prefix. No filesystem access, so it also works for paths
// that do not exist yet -- which is the case for everything inside an archive.
bool IsWithin(const fs::path& base, const fs::path& child);

// Directory creation and file handles for a Windows shipping target.
//
// fs::create_directories ends up in the C runtime's mkdir, which rejects the
// extended-length path form the store needs once a path passes MAX_PATH
// (measured: EINVAL from create_directories("\\\\?\\C:\\...") while
// CreateDirectoryW accepts the same string). EnsureDirectories therefore builds
// the tree one component at a time on Windows.
auto EnsureDirectories(const fs::path& path) -> std::expected<void, Fail>;

// std::ofstream/std::ifstream built from a std::filesystem::path are not usable
// here either: libstdc++ hands the path to the ANSI code page, so a user whose
// profile directory is not pure ASCII cannot install into it, and the narrow CRT
// has no notion of the extended-length form at all. A FILE* opened with _wfopen
// has neither problem. On POSIX these are fopen with the same bytes.
//
// Both handles open through a factory rather than a constructor: a constructor
// has nowhere to put a failure now that nothing throws.
class InputFile {
  public:
    [[nodiscard]] static auto Open(const fs::path& path) -> std::expected<InputFile, Fail>;
    ~InputFile();
    InputFile(InputFile&& other) noexcept;
    InputFile& operator=(InputFile&& other) noexcept;
    InputFile(const InputFile&)            = delete;
    InputFile& operator=(const InputFile&) = delete;

    // Returns the number of bytes read; zero only at end of file.
    auto       ReadSome(char* buffer, size_t count) -> std::expected<size_t, Fail>;
    void       Rewind();
    std::FILE* Handle() const;

  private:
    struct Impl;
    explicit InputFile(Impl* impl);
    Impl* m_impl = nullptr;
};

class OutputFile {
  public:
    [[nodiscard]] static auto Create(const fs::path& path) -> std::expected<OutputFile, Fail>;
    ~OutputFile();
    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&& other) noexcept;
    OutputFile(const OutputFile&)            = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    auto       Write(const char* data, size_t count) -> std::expected<void, Fail>;
    auto       Flush() -> std::expected<void, Fail>;
    std::FILE* Handle() const;

  private:
    struct Impl;
    explicit OutputFile(Impl* impl);
    Impl* m_impl = nullptr;
};

auto ReadFile(const fs::path& path) -> std::expected<std::string, Fail>;
auto WriteFile(const fs::path& path, std::string_view bytes) -> std::expected<void, Fail>;
auto RemoveTree(const fs::path& path) -> std::expected<void, Fail>;
auto FileSizeOf(const fs::path& path) -> std::expected<uint64_t, Fail>;
bool PathExists(const fs::path& path);

// An exclusive lock on the store, so two zpkg processes cannot interleave an
// install. Held for the lifetime of the object.
class StoreLock {
  public:
    [[nodiscard]] static auto Acquire(const fs::path& root) -> std::expected<StoreLock, Fail>;
    ~StoreLock();
    StoreLock(StoreLock&& other) noexcept;
    StoreLock& operator=(StoreLock&& other) noexcept;
    StoreLock(const StoreLock&)            = delete;
    StoreLock& operator=(const StoreLock&) = delete;

  private:
    struct Impl;
    explicit StoreLock(Impl* impl);
    Impl* m_impl;
};

// ---------------------------------------------------------------------------
// SHA-256 and base64 (no dependency: the core of the tool must build anywhere)
// ---------------------------------------------------------------------------
std::string Sha256Hex(std::string_view bytes);
auto        Sha256FileHex(const fs::path& path) -> std::expected<std::string, Fail>;
auto        Base64Decode(std::string_view text) -> std::expected<std::vector<uint8_t>, Fail>;
std::string Base64Encode(const uint8_t* data, size_t size);

// ---------------------------------------------------------------------------
// JSON (simdjson; parsing is strict on purpose)
// ---------------------------------------------------------------------------
struct ToolchainInfo {
    int         format = 0;
    std::string id;
    std::string version;
    std::string commit;
    std::string compiler;
    std::string compilerVersion;
    std::string processor;
    std::string profile;
    std::string abi;
};

struct PackageInfo { // share/zahlen/manifests/<package>.json
    int                      format = 0;
    std::string              package;
    std::string              component;
    std::string              toolchain;
    std::string              profile;
    std::vector<std::string> dependencies;
    std::vector<std::string> provides;
};

auto ParseToolchainJson(std::string_view json, std::string_view origin) -> std::expected<ToolchainInfo, Fail>;
auto ParseManifestJson(std::string_view json, std::string_view origin) -> std::expected<PackageInfo, Fail>;

// The remote index is the only document zpkg both reads and writes.
struct IndexPackage {
    std::string package;
    std::string component;
    std::string file;
    std::string sha256;
    uint64_t    size = 0;
    // Index format 2. The same graph the archive's manifest states, hoisted into
    // the index so a closure can be resolved before a single byte of a 93 MB
    // archive is downloaded. A format-1 index leaves both empty and zpkg falls
    // back to reading each manifest after fetching it. (Named as the manifest
    // names them: "requires" is a C++20 keyword and cannot be an identifier.)
    std::vector<std::string> dependencies;
    std::vector<std::string> provides;
};
struct IndexToolchain {
    std::string               id;
    std::string               version;
    std::string               profile;
    std::vector<IndexPackage> packages;
};
struct Index {
    int                         format = 0;
    std::vector<IndexToolchain> toolchains;
};
auto        ParseIndexJson(std::string_view json, std::string_view origin) -> std::expected<Index, Fail>;
std::string BuildIndexJson(const std::vector<IndexToolchain>& toolchains);

// ---------------------------------------------------------------------------
// Checksum files and signatures
// ---------------------------------------------------------------------------
struct SumEntry {
    std::string sha256;
    std::string name; // as written in the file; matching is by basename
};
auto                       ParseSums(std::string_view text) -> std::expected<std::vector<SumEntry>, Fail>;
std::optional<std::string> LookupSum(const std::vector<SumEntry>& entries, std::string_view basename);

struct Signature {
    std::string          fingerprint; // hex, first 16 bytes of sha256(public key)
    std::vector<uint8_t> signature;
};
auto        ParseSignatureFile(std::string_view text) -> std::expected<std::vector<Signature>, Fail>;
auto        Ed25519Verify(const std::vector<uint8_t>& publicKey, std::string_view message, const std::vector<uint8_t>& signature) -> std::expected<bool, Fail>;
std::string KeyFingerprint(const std::vector<uint8_t>& publicKey);
auto        ParsePublicKey(std::string_view text) -> std::expected<std::vector<uint8_t>, Fail>;

// ---------------------------------------------------------------------------
// Archives
// ---------------------------------------------------------------------------
// A member of a tar archive, after validation. Only regular files, directories,
// symbolic links and hard links survive validation; the paths are guaranteed to
// be relative and to start with "usr".
struct Member {
    std::string path;
    char        type = '0'; // '0' file, '5' directory, '2' symlink, '1' hard link
    std::string link;       // target for '2'/'1'
    uint64_t    size = 0;
};

class Archive {
  public:
    [[nodiscard]] static auto Open(fs::path path) -> std::expected<Archive, Fail>;
    Archive(Archive&& other) noexcept            = default;
    Archive& operator=(Archive&& other) noexcept = default;
    Archive(const Archive&)                      = delete;
    Archive& operator=(const Archive&)           = delete;

    const fs::path& Path() const {
        return m_path;
    }
    // Scan() reads the headers, validates every member against the layout
    // contract and captures the metadata documents. It writes nothing.
    auto                       Scan() -> std::expected<void, Fail>;
    const std::vector<Member>& Members() const {
        return m_members;
    }
    const ToolchainInfo& Toolchain() const {
        return m_toolchain;
    }
    const PackageInfo& Manifest() const {
        return m_manifest;
    }
    // ExtractInto() re-reads the archive and writes the payload under dest,
    // which must be the toolchain directory (it receives usr/).
    auto ExtractInto(const fs::path& dest) -> std::expected<void, Fail>;

  private:
    explicit Archive(fs::path path);
    auto                Walk(bool extract, const fs::path& dest) -> std::expected<void, Fail>;
    fs::path            m_path;
    std::vector<Member> m_members;
    ToolchainInfo       m_toolchain;
    PackageInfo         m_manifest;
    bool                m_scanned = false;
};

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
struct InstallOptions {
    std::vector<std::string> only; // package or component names
    bool                     requireChecksums  = false;
    bool                     requireSignatures = false;
    bool                     force             = false;
    // The caller already verified the content hash against a signed index, so
    // the per-archive SHA256SUMS is neither required nor meaningful.
    bool        prehashed = false;
    std::string toolchain; // remote selection
    // A package that needs one the toolchain does not ship is a refusal, not a
    // warning: the prefix would install and then fail at find_package(). This
    // escape hatch exists for the one legitimate case -- installing a package
    // into a toolchain that another command is about to complete.
    bool ignoreMissing = false;
};

struct InstalledPackage {
    PackageInfo              info;
    std::string              archive;
    std::string              sha256;
    std::vector<std::string> files;
};

struct InstallReport {
    std::string              toolchain;
    std::vector<std::string> installed; // package names, in install order
    std::vector<std::string> skipped;   // filtered out by --only
    std::vector<std::string> missing;   // required packages not present
    size_t                   files = 0;
};

class Store {
  public:
    explicit Store(Env env, bool force = false);

    const Env& Environment() const {
        return m_env;
    }
    fs::path StoreDir() const;
    fs::path Root() const {
        return m_env.root;
    }

    // Toolchains
    auto     Toolchains() const -> std::expected<std::vector<std::string>, Fail>;
    auto     Active() const -> std::expected<std::optional<std::string>, Fail>;
    auto     SetActive(const std::string& id) -> std::expected<void, Fail>;
    fs::path ToolchainDir(const std::string& id) const;
    auto     Prefix(const std::string& id = {}) const -> std::expected<fs::path, Fail>; // empty id -> active
    auto     ReadToolchain(const std::string& id) const -> std::expected<ToolchainInfo, Fail>;
    auto     Packages(const std::string& id) const -> std::expected<std::vector<InstalledPackage>, Fail>;

    // Installation
    auto InstallArchive(const fs::path& archive, const InstallOptions& options, InstallReport report = {}) -> std::expected<InstallReport, Fail>;
    auto InstallDirectory(const fs::path& dir, const InstallOptions& options, InstallReport report = {}) -> std::expected<InstallReport, Fail>;
    // Reports what is still missing and activates the toolchain if none is
    // active. Returns the report with "missing" filled in, or refuses when a
    // requirement is unsatisfied and the caller did not ask for a broken prefix.
    auto Finish(InstallReport report, bool ignoreMissing = false) -> std::expected<InstallReport, Fail>;

    // Reads an archive fully -- layout, metadata, checksums, signatures -- and
    // writes nothing. `zpkg verify` runs this instead of extracting.
    auto VerifyArchive(const fs::path& archive, const InstallOptions& options) -> std::expected<void, Fail>;

    // Removal
    auto RemoveToolchain(const std::string& id, bool force) -> std::expected<void, Fail>;
    auto RemovePackage(const std::string& package, const std::string& id) -> std::expected<void, Fail>;

    // Trust store and remotes
    auto KeyAdd(const fs::path& keyFile, std::string_view name) -> std::expected<void, Fail>;
    auto Keys() const -> std::expected<std::vector<std::pair<std::string, std::string>>, Fail>; // fingerprint, name
    auto KeyRemove(const std::string& fingerprint) -> std::expected<void, Fail>;
    auto TrustedKey(const std::string& fingerprint) const -> std::expected<std::optional<std::vector<uint8_t>>, Fail>;

    auto RemoteAdd(std::string_view name, std::string_view url) -> std::expected<void, Fail>;
    auto Remotes() const -> std::expected<std::map<std::string, std::string>, Fail>;
    auto RemoteRemove(std::string_view name) -> std::expected<void, Fail>;

    fs::path CacheDir() const;

    // Shims
    auto ShimsEnable() -> std::expected<void, Fail>;
    auto ShimsDisable() -> std::expected<void, Fail>;

  private:
    auto ApplyRecord(const std::string& id, const PackageInfo& info, const fs::path& archive, const std::string& sha, const std::vector<std::string>& files)
        -> std::expected<void, Fail>;
    auto                               VerifySource(const fs::path& archive, const InstallOptions& options) -> std::expected<void, Fail>;
    Env                                m_env;
    bool                               m_force;
    mutable std::optional<std::string> m_activeCache;
};

// ---------------------------------------------------------------------------
// Remotes
// ---------------------------------------------------------------------------
struct RemoteFile {
    std::string name;
    std::string sha256;
    uint64_t    size = 0;
};

std::string UrlJoin(std::string_view base, std::string_view leaf);
auto        FetchToFile(std::string_view url, const fs::path& dest) -> std::expected<void, Fail>;
auto        FetchToString(std::string_view url) -> std::expected<std::optional<std::string>, Fail>;

// Installs every selected package of a remote's toolchain, verifying content
// against the (optionally signed) index before anything is extracted.
auto InstallFromRemote(Store& store, const std::string& name, const InstallOptions& options) -> std::expected<InstallReport, Fail>;
// Verifies a remote without installing; returns how many packages were checked.
auto VerifyRemote(Store& store, const std::string& name, const InstallOptions& options) -> std::expected<size_t, Fail>;

// Publishes <dir>/index.json from the archives in <dir>.
auto BuildIndex(const fs::path& dir) -> std::expected<std::vector<IndexToolchain>, Fail>;

// ---------------------------------------------------------------------------
// Selection and resolution
// ---------------------------------------------------------------------------
// The tokens a user types -- `zpkg install up:zahlen-gltf`, `--only Engine`,
// `--only gltf` -- all have to reach the same package. Matching is by package
// name, component name, "zahlen-" prefix or provided CMake target.
bool PackageMatches(const IndexPackage& package, std::string_view want);

// True when any package in the index states a requirement, i.e. when the
// closure can be resolved without fetching. False for a format-1 index.
bool ToolchainHasDependencies(const IndexToolchain& toolchain);

struct Resolution {
    std::vector<IndexPackage> packages; // dependencies before dependents
    std::vector<std::string>  missing;  // required by a selected package, provided by none
};
// Expands `roots` over the requires[] graph in the index. A toolchain with
// cycles resolves (nothing is lost); one with an unprovidable requirement
// reports it in `missing` rather than inventing an order.
auto ResolveClosure(const IndexToolchain& toolchain, const std::vector<IndexPackage>& roots) -> Resolution;

} // namespace ZHLN
