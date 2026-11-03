// The store: layout, installation, removal, trust and remotes.
//
// Layout under the root (never a system directory):
//
//   store/<toolchain-id>/usr/{bin,lib,include,share}   the payload, immutable
//   store/<toolchain-id>/zpkg/toolchain.json           who this toolchain is
//   store/<toolchain-id>/zpkg/installed/<pkg>.json      the manifest, verbatim
//   store/<toolchain-id>/zpkg/installed/<pkg>.meta      archive, sha256, file count
//   store/<toolchain-id>/zpkg/installed/<pkg>.files     every path it owns
//   store/ACTIVE                                        the active toolchain id
//   active -> store/<toolchain-id>                      POSIX mirror of ACTIVE
//   keys/<fingerprint>.key                              trusted publisher keys
//   remotes/<name>                                      where a name resolves to
//   cache/<sha256>                                      downloaded archives
#include "zpkg/zpkg.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

namespace {
std::string Join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0)
            out += separator;
        out += items[i];
    }
    return out;
}
} // namespace

namespace {

constexpr std::string_view ActiveFile = "ACTIVE";

bool ValidName(std::string_view text) {
    if (text.empty())
        return false;
    const auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(text[0]))
        return false;
    for (char c: text) {
        if (!alnum(c) && c != '.' && c != '_' && c != '+' && c != '-')
            return false;
    }
    return true;
}

auto CheckToolchainId(const std::string& id) -> std::expected<void, Fail> {
    if (!ValidName(id)) {
        return std::unexpected(Fail {"invalid toolchain id '" + id + "' (allowed: letters, digits, . _ + -)"});
    }
    return {};
}

auto CheckPackageName(const std::string& name) -> std::expected<void, Fail> {
    if (!ValidName(name)) {
        return std::unexpected(Fail {"invalid package name '" + name + "' (allowed: letters, digits, . _ + -)"});
    }
    return {};
}

std::string LeafOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Only the payload prefix is ever deleted by a removal. Anything else in a file
// list is a corrupted record and must not turn into an rm -rf of the home dir.
auto CheckOwnedPath(const std::string& path) -> std::expected<void, Fail> {
    if (!StartsWith(path, "usr/") || path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
        return std::unexpected(Fail {"refusing to touch '" + path + "': it is not inside the package payload"});
    }
    return {};
}

bool MatchesOnly(const std::vector<std::string>& only, const PackageInfo& info) {
    if (only.empty())
        return true;
    for (const auto& want: only) {
        if (want == info.package || want == info.component || "zahlen-" + AsciiLower(want) == info.package) {
            return true;
        }
    }
    return false;
}

auto WriteAtomic(const fs::path& path, std::string_view bytes) -> std::expected<void, Fail> {
    const fs::path temporary = path.string() + ".tmp";
    if (auto written = WriteFile(temporary, bytes); !written)
        return std::unexpected(std::move(written.error()));
    std::error_code ec;
    fs::rename(Native(temporary), Native(path), ec);
    if (ec) {
        fs::remove(Native(temporary), ec);
        return std::unexpected(Fail {"cannot replace " + ToUtf8(path)});
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and layout
// ---------------------------------------------------------------------------
Store::Store(Env env, bool force): m_env(std::move(env)), m_force(force) {
}

fs::path Store::StoreDir() const {
    return m_env.root / "store";
}

fs::path Store::CacheDir() const {
    return m_env.root / "cache";
}

fs::path Store::ToolchainDir(const std::string& id) const {
    return m_env.root / "store" / id;
}

auto Store::Toolchains() const -> std::expected<std::vector<std::string>, Fail> {
    std::vector<std::string> ids;
    std::error_code          ec;
    if (!fs::is_directory(Native(StoreDir()), ec))
        return ids;
    for (const auto& entry: fs::directory_iterator(Native(StoreDir()), ec)) {
        if (!entry.is_directory())
            continue;
        const std::string id = ToUtf8(entry.path().filename());
        if (PathExists(entry.path() / "zpkg" / "toolchain.json"))
            ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

auto Store::Active() const -> std::expected<std::optional<std::string>, Fail> {
    if (m_activeCache)
        return *m_activeCache;
    const fs::path marker = StoreDir() / std::string(ActiveFile);
    if (PathExists(marker)) {
        auto bytes = ReadFile(marker);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        const std::string id = Trim(*bytes);
        if (!id.empty()) {
            m_activeCache = id;
            return *m_activeCache;
        }
    }
    // The POSIX symlink is a convenience mirror; on Windows it may not exist at
    // all, which is why ACTIVE is the authority.
    const fs::path  link = m_env.root / "active";
    std::error_code ec;
    if (fs::is_symlink(Native(link), ec)) {
        const fs::path target = fs::read_symlink(Native(link), ec);
        if (!ec) {
            const std::string id = ToUtf8(target.filename());
            m_activeCache        = id;
            return m_activeCache;
        }
    }
    return std::nullopt;
}

auto Store::SetActive(const std::string& id) -> std::expected<void, Fail> {
    auto named = CheckToolchainId(id);
    if (!named)
        return std::unexpected(std::move(named.error()));
    if (!PathExists(ToolchainDir(id) / "usr")) {
        return std::unexpected(Fail {"no toolchain '" + id + "' in " + ToUtf8(m_env.root)});
    }
    auto written = WriteAtomic(StoreDir() / std::string(ActiveFile), id + "\n");
    if (!written)
        return std::unexpected(std::move(written.error()));
    m_activeCache = id;

#if !defined(_WIN32)
    // Relative on purpose: the store stays relocatable, and a moved store keeps
    // pointing at itself.
    std::error_code ec;
    const fs::path  link = m_env.root / "active";
    fs::remove(Native(link), ec);
    fs::create_directory_symlink(fs::path("store") / id, Native(link), ec);
#endif
    return {};
}

auto Store::Prefix(const std::string& id) const -> std::expected<fs::path, Fail> {
    // "Nothing is active" and "there is no store here" are different problems,
    // and after somebody moves the root the second one is the answer; the first
    // message sends them looking for a marker that was never the issue.
    if (!PathExists(m_env.root)) {
        return std::unexpected(Fail {"no store at " + ToUtf8(m_env.root) + "; set ZHLN_ROOT if it lives somewhere else"});
    }
    std::string use = id;
    if (use.empty()) {
        auto current = Active();
        if (!current)
            return std::unexpected(std::move(current.error()));
        if (!current->has_value()) {
            return std::unexpected(Fail {"no active toolchain; 'zpkg list' shows what is installed, 'zpkg use <id>' "
                                         "makes one active"});
        }
        use = **current;
    }
    auto named = CheckToolchainId(use);
    if (!named)
        return std::unexpected(std::move(named.error()));
    const fs::path directory = ToolchainDir(use) / "usr";
    if (!PathExists(directory))
        return std::unexpected(Fail {"no toolchain '" + use + "' in " + ToUtf8(m_env.root)});
    return directory;
}

auto Store::ReadToolchain(const std::string& id) const -> std::expected<ToolchainInfo, Fail> {
    const fs::path file = ToolchainDir(id) / "zpkg" / "toolchain.json";
    return CheckToolchainId(id)
        .and_then([&]() -> std::expected<std::string, Fail> {
            if (!PathExists(file))
                return std::unexpected(Fail {"no toolchain '" + id + "' in " + ToUtf8(m_env.root)});
            return ReadFile(file);
        })
        .and_then([&](std::string text) { return ParseToolchainJson(text, ToUtf8(file)); });
}

auto Store::Packages(const std::string& id) const -> std::expected<std::vector<InstalledPackage>, Fail> {
    std::vector<InstalledPackage> installed;
    const fs::path                directory = ToolchainDir(id) / "zpkg" / "installed";
    std::error_code               ec;
    if (!fs::is_directory(Native(directory), ec))
        return installed;
    std::vector<fs::path> records;
    for (const auto& entry: fs::directory_iterator(Native(directory), ec)) {
        if (entry.path().extension() == ".json")
            records.push_back(entry.path());
    }
    std::sort(records.begin(), records.end());
    for (const auto& record: records) {
        auto manifest = ReadFile(record).and_then([&record](std::string text) { return ParseManifestJson(text, ToUtf8(record)); });
        if (!manifest)
            return std::unexpected(std::move(manifest.error()));
        InstalledPackage package;
        package.info        = std::move(*manifest);
        const fs::path meta = record.parent_path() / (record.stem().string() + ".meta");
        if (PathExists(meta)) {
            auto metaText = ReadFile(meta);
            if (!metaText)
                return std::unexpected(std::move(metaText.error()));
            std::istringstream lines(*metaText);
            std::string        line;
            while (std::getline(lines, line)) {
                const size_t equals = line.find('=');
                if (equals == std::string::npos)
                    continue;
                const std::string key   = line.substr(0, equals);
                const std::string value = line.substr(equals + 1);
                if (key == "archive")
                    package.archive = value;
                if (key == "sha256")
                    package.sha256 = value;
            }
        }
        const fs::path files = record.parent_path() / (record.stem().string() + ".files");
        if (PathExists(files)) {
            auto listingText = ReadFile(files);
            if (!listingText)
                return std::unexpected(std::move(listingText.error()));
            std::istringstream listing(*listingText);
            std::string        line;
            while (std::getline(listing, line)) {
                if (!line.empty())
                    package.files.push_back(line);
            }
        }
        installed.push_back(std::move(package));
    }
    return installed;
}

// ---------------------------------------------------------------------------
// Trust
// ---------------------------------------------------------------------------
auto Store::VerifySource(const fs::path& archive, const InstallOptions& options) -> std::expected<void, Fail> {
    if (options.prehashed)
        return {};
    const std::string base      = LeafOf(ToUtf8(archive));
    const fs::path    directory = archive.has_parent_path() ? archive.parent_path() : fs::path(".");

    fs::path sums = directory / std::string(SumsFile);
    if (!PathExists(sums)) {
        const fs::path sidecar = archive.string() + ".sha256";
        if (PathExists(sidecar))
            sums = sidecar;
    }

    std::optional<std::string> expected;
    std::string                sumsBytes;
    if (PathExists(sums)) {
        auto bytes = ReadFile(sums);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        sumsBytes    = std::move(*bytes);
        auto entries = ParseSums(sumsBytes);
        if (!entries)
            return std::unexpected(std::move(entries.error()));
        expected = LookupSum(*entries, base);
        if (!expected) {
            if (options.requireChecksums) {
                return std::unexpected(Fail {base + " is missing from " + ToUtf8(sums)});
            }
            log::Warn(base + " is missing from " + ToUtf8(sums) + "; installing unverified");
        }
    } else if (options.requireChecksums) {
        return std::unexpected(Fail {
            base + " has no checksum file.\n"
                   "       Produce one with 'sha256sum *.tar.gz > SHA256SUMS', or drop "
                   "--require-checksums"
        });
    } else {
        log::Warn(base + " has no checksum; installing unverified");
    }

    if (expected) {
        auto digest = Sha256FileHex(archive);
        if (!digest)
            return std::unexpected(std::move(digest.error()));
        const std::string actual = *digest;
        if (AsciiLower(*expected) != actual) {
            return std::unexpected(Fail {base + ": checksum mismatch\n       expected " + *expected + "\n       actual   " + actual});
        }
        log::Say("  sha256 ok  " + actual.substr(0, 16) + "...");
    }

    const fs::path signatureFile     = directory / std::string(SignatureFile);
    const bool     signaturesPresent = PathExists(signatureFile);
    if (!signaturesPresent) {
        if (options.requireSignatures) {
            return std::unexpected(Fail {base + " has no " + std::string(SignatureFile) + "; refusing an unsigned archive"});
        }
        return {};
    }
    if (!PathExists(sums)) {
        if (options.requireSignatures)
            return std::unexpected(Fail {"nothing to verify a signature against"});
        log::Warn("a signature exists but no SHA256SUMS does; ignoring it");
        return {};
    }

    auto signatureText = ReadFile(signatureFile);
    if (!signatureText)
        return std::unexpected(std::move(signatureText.error()));
    auto signatures = ParseSignatureFile(*signatureText);
    if (!signatures)
        return std::unexpected(std::move(signatures.error()));
    if (signatures->empty()) {
        return std::unexpected(Fail {ToUtf8(signatureFile) + " contains no usable signature"});
    }
    std::string failure;
    for (const auto& signature: *signatures) {
        auto key = TrustedKey(signature.fingerprint);
        if (!key)
            return std::unexpected(std::move(key.error()));
        if (!key->has_value()) {
            failure += "\n       no trusted key with fingerprint " + signature.fingerprint;
            continue;
        }
        auto verified = Ed25519Verify(**key, sumsBytes, signature.signature);
        if (!verified)
            return std::unexpected(std::move(verified.error()));
        if (*verified) {
            log::Say("  signed     " + signature.fingerprint);
            return {};
        }
        failure += "\n       key " + signature.fingerprint + " does not verify this SHA256SUMS";
    }
    return std::unexpected(
        Fail {"no trusted signature covers " + ToUtf8(sums) + ":" + failure + "\n       Trust the publishing key with 'zpkg key add <key.pub> --name <who>'."}
    );
}

auto Store::KeyAdd(const fs::path& keyFile, std::string_view name) -> std::expected<void, Fail> {
    return ReadFile(keyFile).and_then([](std::string text) { return ParsePublicKey(text); }
    ).and_then([&](std::vector<uint8_t> key) -> std::expected<void, Fail> {
        const std::string fingerprint = KeyFingerprint(key);
        const fs::path    directory   = m_env.root / "keys";
        return WriteFile(directory / (fingerprint + ".key"), Base64Encode(key.data(), key.size()) + "\n")
            .and_then([&]() -> std::expected<void, Fail> {
                if (name.empty())
                    return {};
                return WriteFile(directory / (fingerprint + ".name"), std::string(name) + "\n");
            })
            .transform([&] {
                log::Say("trusted " + fingerprint + (name.empty() ? "" : " (" + std::string(name) + ")"));
#if !defined(ZPKG_HAVE_OPENSSL)
                // The trust file is just text, so this works in a build without
                // crypto -- but storing a key that this binary cannot check
                // would be a comfort with nothing behind it.
                log::Warn("this zpkg was built without OpenSSL, so it cannot verify "
                          "signatures; the key is trusted for builds that can");
#endif
            });
    });
}

auto Store::Keys() const -> std::expected<std::vector<std::pair<std::string, std::string>>, Fail> {
    std::vector<std::pair<std::string, std::string>> trusted;
    std::error_code                                  ec;
    const fs::path                                   directory = m_env.root / "keys";
    if (!fs::is_directory(Native(directory), ec))
        return trusted;
    for (const auto& entry: fs::directory_iterator(Native(directory), ec)) {
        if (entry.path().extension() != ".key")
            continue;
        const std::string fingerprint = entry.path().stem().string();
        std::string       name;
        const fs::path    named = entry.path().parent_path() / (fingerprint + ".name");
        if (PathExists(named)) {
            auto text = ReadFile(named);
            if (!text)
                return std::unexpected(std::move(text.error()));
            name = Trim(*text);
        }
        trusted.emplace_back(fingerprint, name);
    }
    std::sort(trusted.begin(), trusted.end());
    return trusted;
}

auto Store::KeyRemove(const std::string& fingerprint) -> std::expected<void, Fail> {
    std::error_code ec;
    const fs::path  directory = m_env.root / "keys";
    const fs::path  key       = directory / (fingerprint + ".key");
    if (!PathExists(key))
        return std::unexpected(Fail {"no trusted key " + fingerprint});
    fs::remove(Native(key), ec);
    fs::remove(Native(directory / (fingerprint + ".name")), ec);
    log::Say("forgot " + fingerprint);
    return {};
}

auto Store::TrustedKey(const std::string& fingerprint) const -> std::expected<std::optional<std::vector<uint8_t>>, Fail> {
    std::error_code ec;
    const fs::path  directory = m_env.root / "keys";
    if (!fs::is_directory(Native(directory), ec))
        return std::nullopt;
    for (const auto& entry: fs::directory_iterator(Native(directory), ec)) {
        if (entry.path().extension() != ".key")
            continue;
        const std::string candidate = entry.path().stem().string();
        // Prefix match, so a signature may name a short fingerprint.
        if (candidate != fingerprint && !StartsWith(candidate, fingerprint))
            continue;
        return ReadFile(entry.path()).and_then([](std::string text) { return ParsePublicKey(text); });
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Remotes
// ---------------------------------------------------------------------------
auto Store::RemoteAdd(std::string_view name, std::string_view url) -> std::expected<void, Fail> {
    if (!ValidName(std::string(name)))
        return std::unexpected(Fail {"invalid remote name '" + std::string(name) + "'"});
    std::string target(url);
    if (!StartsWith(target, "file://") && !StartsWith(target, "https://") && !StartsWith(target, "http://")) {
        target = ToFileUrl(FromUtf8(target));
    }
    if (StartsWith(target, "http://")) {
        log::Warn("'" + target + "' is not encrypted; prefer https:// or a local directory");
    }
    auto written = WriteFile(m_env.root / "remotes" / std::string(name), target + "\n");
    if (!written)
        return std::unexpected(std::move(written.error()));
    log::Say("remote " + std::string(name) + " -> " + target);
    return {};
}

auto Store::Remotes() const -> std::expected<std::map<std::string, std::string>, Fail> {
    std::map<std::string, std::string> entries;
    std::error_code                    ec;
    const fs::path                     directory = m_env.root / "remotes";
    if (!fs::is_directory(Native(directory), ec))
        return entries;
    for (const auto& entry: fs::directory_iterator(Native(directory), ec)) {
        if (!entry.is_regular_file())
            continue;
        auto text = ReadFile(entry.path());
        if (!text)
            return std::unexpected(std::move(text.error()));
        entries[entry.path().filename().string()] = Trim(*text);
    }
    return entries;
}

auto Store::RemoteRemove(std::string_view name) -> std::expected<void, Fail> {
    const fs::path file = m_env.root / "remotes" / std::string(name);
    if (!PathExists(file))
        return std::unexpected(Fail {"no remote named " + std::string(name)});
    std::error_code ec;
    fs::remove(Native(file), ec);
    log::Say("removed remote " + std::string(name));
    return {};
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------
auto Store::ApplyRecord(const std::string& id, const PackageInfo& info, const fs::path& archive, const std::string& sha, const std::vector<std::string>& files)
    -> std::expected<void, Fail> {
    const fs::path    directory = ToolchainDir(id) / "zpkg" / "installed";
    const std::string base      = info.package;
    auto              meta =
        WriteFile(directory / (base + ".meta"), "archive=" + LeafOf(ToUtf8(archive)) + "\nsha256=" + sha + "\nfiles=" + std::to_string(files.size()) + "\n");
    if (!meta)
        return std::unexpected(std::move(meta.error()));
    std::string listing;
    for (const auto& path: files)
        listing += path + "\n";
    return WriteFile(directory / (base + ".files"), listing);
}

auto Store::InstallArchive(const fs::path& archivePath, const InstallOptions& options, InstallReport report) -> std::expected<InstallReport, Fail> {
    if (!PathExists(archivePath)) {
        return std::unexpected(Fail {"no such archive: " + ToUtf8(archivePath)});
    }
    auto opened = Archive::Open(archivePath);
    if (!opened)
        return std::unexpected(std::move(opened.error()));
    Archive& archive = *opened;
    auto     scanned = archive.Scan();
    if (!scanned)
        return std::unexpected(std::move(scanned.error()));
    const PackageInfo& info = archive.Manifest();

    if (!MatchesOnly(options.only, info)) {
        report.skipped.push_back(info.package);
        log::Say("  skip       " + info.package + " (--only)");
        return report;
    }
    if (!report.installed.empty() && !report.toolchain.empty() && report.toolchain != archive.Toolchain().id) {
        return std::unexpected(Fail {"one install run may not mix toolchains: '" + report.toolchain + "' and '" + archive.Toolchain().id + "'"});
    }

    auto verified = VerifySource(archivePath, options);
    if (!verified)
        return std::unexpected(std::move(verified.error()));

    const std::string id    = archive.Toolchain().id;
    auto              named = CheckToolchainId(id);
    if (!named)
        return std::unexpected(std::move(named.error()));
    auto digest = Sha256FileHex(archivePath);
    if (!digest)
        return std::unexpected(std::move(digest.error()));
    const std::string sha         = *digest;
    const fs::path    destination = ToolchainDir(id);
    const fs::path    record      = destination / "zpkg" / "installed" / (info.package + ".json");

    if (PathExists(record)) {
        auto previousText = ReadFile(destination / "zpkg" / "installed" / (info.package + ".meta"));
        if (!previousText)
            return std::unexpected(std::move(previousText.error()));
        const std::string& previous = *previousText;
        if (previous.find(sha) != std::string::npos && !options.force) {
            log::Say("  up to date " + info.package + " (" + info.component + ")");
            report.installed.push_back(info.package);
            report.toolchain = id;
            return report;
        }
        log::Say("  reinstall  " + info.package + " (" + info.component + ")");
    }

    auto extracted = archive.ExtractInto(destination);
    if (!extracted)
        return std::unexpected(std::move(extracted.error()));

    // The metadata documents are copied, not regenerated: the manifest the build
    // wrote stays the single source of truth about this package, and zpkg's own
    // state is derived from it rather than competing with it.
    const fs::path payloadManifest = destination / "usr" / "share" / "zahlen" / "manifests" / (info.package + ".json");
    if (!PathExists(payloadManifest)) {
        return std::unexpected(Fail {"the package manifest vanished during extraction: " + ToUtf8(payloadManifest)});
    }
    auto payload = ReadFile(payloadManifest);
    if (!payload)
        return std::unexpected(std::move(payload.error()));
    auto recorded = WriteFile(record, *payload);
    if (!recorded)
        return std::unexpected(std::move(recorded.error()));
    auto descriptor = ReadFile(destination / "usr" / "share" / "zahlen" / "toolchain.json");
    if (!descriptor)
        return std::unexpected(std::move(descriptor.error()));
    auto copied = WriteFile(destination / "zpkg" / "toolchain.json", *descriptor);
    if (!copied)
        return std::unexpected(std::move(copied.error()));

    const auto&              members = archive.Members();
    std::vector<std::string> files;
    files.reserve(members.size());
    for (const auto& member: members) {
        if (member.type == '5')
            continue;
        // toolchain.json is shared by every package of a toolchain; counting it
        // as owned by one would let a removal blind the rest.
        if (member.path == "usr/share/zahlen/toolchain.json")
            continue;
        files.push_back(member.path);
    }
    auto applied = ApplyRecord(id, info, archivePath, sha, files);
    if (!applied)
        return std::unexpected(std::move(applied.error()));

    report.toolchain = id;
    report.installed.push_back(info.package);
    report.files += files.size();
    log::Say("  installed  " + info.package + " (" + info.component + ", " + std::to_string(files.size()) + " files) -> store/" + id);
    return report;
}

auto Store::InstallDirectory(const fs::path& directory, const InstallOptions& options, InstallReport report) -> std::expected<InstallReport, Fail> {
    std::error_code ec;
    if (!fs::is_directory(Native(directory), ec)) {
        return std::unexpected(Fail {"no such directory: " + ToUtf8(directory)});
    }
    std::vector<fs::path> archives;
    for (const auto& entry: fs::directory_iterator(Native(directory), ec)) {
        if (!entry.is_regular_file())
            continue;
        const std::string name = entry.path().filename().string();
        if (!StartsWith(name, "zahlen-"))
            continue;
        if (!EndsWith(name, ".tar.gz") && !EndsWith(name, ".tar.zst") && !EndsWith(name, ".tar")) {
            continue;
        }
        archives.push_back(entry.path());
    }
    if (archives.empty()) {
        return std::unexpected(Fail {"no zahlen-*.tar.{gz,zst} archives in " + ToUtf8(directory)});
    }
    std::sort(archives.begin(), archives.end());
    for (const auto& archive: archives) {
        auto next = InstallArchive(archive, options, std::move(report));
        if (!next)
            return std::unexpected(std::move(next.error()));
        report = std::move(*next);
    }
    return report;
}

auto Store::VerifyArchive(const fs::path& archivePath, const InstallOptions& options) -> std::expected<void, Fail> {
    auto opened = Archive::Open(archivePath);
    if (!opened)
        return std::unexpected(std::move(opened.error()));
    Archive& archive = *opened;
    auto     scanned = archive.Scan();
    if (!scanned)
        return std::unexpected(std::move(scanned.error()));
    auto verified = VerifySource(archivePath, options);
    if (!verified)
        return std::unexpected(std::move(verified.error()));
    log::Say("  ok         " + archive.Manifest().package + " (" + archive.Manifest().component + ") in " + archive.Toolchain().id);
    return {};
}

auto Store::Finish(InstallReport report, bool ignoreMissing) -> std::expected<InstallReport, Fail> {
    if (report.installed.empty()) {
        log::Warn("nothing installed");
        return report;
    }

    // Dependencies are stated, never solved -- but they are also never ignored.
    // An incomplete prefix installs fine and then fails at find_package(), which
    // is a long way from the command that caused it, so the command that caused
    // it refuses instead. --ignore-missing is for the one case where a later
    // command completes this toolchain.
    auto installed = Packages(report.toolchain);
    if (!installed)
        return std::unexpected(std::move(installed.error()));
    std::set<std::string> present;
    for (const auto& package: *installed)
        present.insert(package.info.package);
    std::vector<std::string> missing;
    for (const auto& package: *installed) {
        for (const auto& requirement: package.info.dependencies) {
            if (present.count(requirement) != 0)
                continue;
            if (std::find(missing.begin(), missing.end(), requirement) == missing.end())
                missing.push_back(requirement);
        }
    }
    report.missing = missing;

    // Activate and print the prefix first: a refused install still leaves a
    // consistent store, and the path it prints is where the missing package goes.
    auto active = Active();
    if (!active)
        return std::unexpected(std::move(active.error()));
    if (!active->has_value()) {
        auto activated = SetActive(report.toolchain);
        if (!activated)
            return std::unexpected(std::move(activated.error()));
        log::Say("active toolchain: " + report.toolchain);
    } else {
        log::Say("active toolchain: " + **active + " (unchanged; 'zpkg use <id>' to switch)");
    }
    auto prefix = Prefix();
    if (!prefix)
        return std::unexpected(std::move(prefix.error()));
    log::Say("prefix: " + ToUtf8(*prefix));

    if (!missing.empty()) {
        for (const auto& package: *installed) {
            for (const auto& requirement: package.info.dependencies) {
                if (present.count(requirement) == 0)
                    log::Warn(package.info.package + " needs " + requirement + ", which is not installed in " + report.toolchain);
            }
        }
        if (!ignoreMissing) {
            return std::unexpected(Fail {
                "this toolchain is incomplete: " + std::to_string(missing.size()) + " required package(s) are not installed (" + Join(missing, ", ") +
                ")\n       a prefix that cannot find_package() its own dependencies is not an install"
                "\n       add them with 'zpkg install <source> --only " + Join(missing, ",") + "', or pass --ignore-missing"
            });
        }
        log::Warn("--ignore-missing: the prefix will not configure against the packages listed above");
    }
    return report;
}

auto Store::RemoveToolchain(const std::string& id, bool force) -> std::expected<void, Fail> {
    auto named = CheckToolchainId(id);
    if (!named)
        return std::unexpected(std::move(named.error()));
    const fs::path directory = ToolchainDir(id);
    if (!PathExists(directory))
        return std::unexpected(Fail {"no toolchain '" + id + "' in " + ToUtf8(m_env.root)});
    auto active = Active();
    if (!active)
        return std::unexpected(std::move(active.error()));
    if (active->has_value() && **active == id) {
        if (!force) {
            return std::unexpected(Fail {"'" + id + "' is active; 'zpkg use <other-id>' first, or pass --force"});
        }
        std::error_code ec;
        fs::remove(Native(StoreDir() / std::string(ActiveFile)), ec);
        fs::remove(Native(m_env.root / "active"), ec);
        m_activeCache.reset();
    }
    auto removed = RemoveTree(directory);
    if (!removed)
        return std::unexpected(std::move(removed.error()));
    log::Say("removed toolchain " + id);
    return {};
}

auto Store::RemovePackage(const std::string& package, const std::string& id) -> std::expected<void, Fail> {
    std::string use = id;
    if (use.empty()) {
        auto active = Active();
        if (!active)
            return std::unexpected(std::move(active.error()));
        if (active->has_value())
            use = **active;
    }
    if (use.empty())
        return std::unexpected(Fail {"--package needs a toolchain id when nothing is active"});
    auto named = CheckPackageName(package);
    if (!named)
        return std::unexpected(std::move(named.error()));
    const fs::path directory = ToolchainDir(use);
    const fs::path files     = directory / "zpkg" / "installed" / (package + ".files");
    if (!PathExists(files))
        return std::unexpected(Fail {"'" + package + "' is not installed in " + use});

    size_t removed     = 0;
    auto   listingText = ReadFile(files);
    if (!listingText)
        return std::unexpected(std::move(listingText.error()));
    std::istringstream listing(*listingText);
    std::string        line;
    while (std::getline(listing, line)) {
        if (line.empty())
            continue;
        auto owned = CheckOwnedPath(line);
        if (!owned)
            return std::unexpected(std::move(owned.error()));
        const fs::path target = directory / FromUtf8(line);
        if (!IsWithin(directory, target)) {
            return std::unexpected(Fail {"refusing to remove '" + line + "'"});
        }
        std::error_code ec;
        const auto      status = fs::symlink_status(Native(target), ec);
        if (!ec && fs::exists(status)) {
            fs::remove(Native(target), ec);
            if (ec)
                return std::unexpected(Fail {"cannot remove " + ToUtf8(target) + ": " + ec.message()});
            ++removed;
        }
    }
    std::error_code ec;
    fs::remove(Native(files), ec);
    fs::remove(Native(directory / "zpkg" / "installed" / (package + ".meta")), ec);
    fs::remove(Native(directory / "zpkg" / "installed" / (package + ".json")), ec);
    log::Say("removed " + package + " from " + use + " (" + std::to_string(removed) + " files)");
    return {};
}

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------
auto Store::ShimsEnable() -> std::expected<void, Fail> {
#if defined(_WIN32)
    log::Note("shims are a POSIX convenience; on Windows run 'zpkg env --shell powershell' (or --shell "
              "cmd) and evaluate it, which puts the active toolchain on PATH");
    return {};
#else
    auto prefix = Prefix();
    if (!prefix)
        return std::unexpected(std::move(prefix.error()));
    const fs::path  bin = *prefix / "bin";
    std::error_code ec;
    if (!fs::is_directory(Native(bin), ec)) {
        log::Say("the active toolchain ships no executables (" + ToUtf8(bin) + " is absent)");
        return {};
    }
    auto made = EnsureDirectories(m_env.binDir);
    if (!made)
        return std::unexpected(std::move(made.error()));
    size_t count = 0;
    for (const auto& entry: fs::directory_iterator(Native(bin), ec)) {
        if (!entry.is_regular_file())
            continue;
        const std::string name = entry.path().filename().string();
        const fs::path    shim = m_env.binDir / name;
        // The shim resolves through ACTIVE at run time, so switching toolchains
        // does not require rewriting it.
        std::string body = "#!/bin/sh\n";
        body += "# zpkg shim for Zahlen " + name + "; regenerate with 'zpkg shims enable'.\n";
        body += "root=\"${ZHLN_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/zahlen}\"\n";
        body += "id=$(cat \"$root/store/ACTIVE\" 2>/dev/null) || exit 127\n";
        body += "exec \"$root/store/$id/usr/bin/" + name + "\" \"$@\"\n";
        auto written = WriteFile(shim, body);
        if (!written)
            return std::unexpected(std::move(written.error()));
        fs::permissions(
            Native(shim), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
            fs::perm_options::replace, ec
        );
        ++count;
    }
    log::Say("wrote " + std::to_string(count) + " shim(s) to " + ToUtf8(m_env.binDir));
    return {};
#endif
}

auto Store::ShimsDisable() -> std::expected<void, Fail> {
#if defined(_WIN32)
    log::Note("nothing to remove: shims are a POSIX convenience");
    return {};
#else
    std::error_code ec;
    size_t          removed = 0;
    if (fs::is_directory(Native(m_env.binDir), ec)) {
        for (const auto& entry: fs::directory_iterator(Native(m_env.binDir), ec)) {
            if (!entry.is_regular_file())
                continue;
            auto content = ReadFile(entry.path());
            if (!content)
                return std::unexpected(std::move(content.error()));
            if (content->find("zpkg shim for Zahlen") == std::string::npos)
                continue;
            fs::remove(entry.path(), ec);
            ++removed;
        }
    }
    log::Say("removed " + std::to_string(removed) + " zpkg shim(s) from " + ToUtf8(m_env.binDir));
    return {};
#endif
}

} // namespace ZHLN
