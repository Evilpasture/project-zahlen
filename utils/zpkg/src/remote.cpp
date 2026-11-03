// Remotes: where packages can come from, and how an index is published.
//
// A remote is a directory of files. It may be a path, a file:// URL, or an
// https:// URL when zpkg was built with libcurl. There is no protocol to
// implement and no server to run: publishing a toolchain means running
// `zpkg index` over the artifacts and copying the directory somewhere.
#include "zpkg/zpkg.hpp"
#include <algorithm>
#include <cstdio>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(ZPKG_HAVE_CURL)
#include <curl/curl.h>
#endif

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

bool IsHttp(std::string_view url) {
    return StartsWith(url, "https://") || StartsWith(url, "http://");
}

std::string StripFileScheme(std::string_view url) {
    std::string text(url);
    if (StartsWith(text, "file://")) {
        text.erase(0, 7);
#if defined(_WIN32)
        // file:///C:/dir parses as "/C:/dir" once the scheme is gone.
        if (text.size() >= 3 && text[0] == '/' && text[2] == ':')
            text.erase(0, 1);
#endif
    }
    return text;
}

#if defined(ZPKG_HAVE_CURL)
size_t WriteToFile(void* data, size_t size, size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<std::FILE*>(user));
}

size_t WriteToString(void* data, size_t size, size_t count, void* user) {
    auto* text = static_cast<std::string*>(user);
    text->append(static_cast<const char*>(data), size * count);
    return size * count;
}

struct CurlHandle {
    CURL* handle = nullptr;
    CurlHandle() = default;
    [[nodiscard]] auto Init() -> std::expected<void, Fail> {
        handle = curl_easy_init();
        if (handle == nullptr)
            return std::unexpected(Fail {"cannot initialise libcurl"});
        return {};
    }
    ~CurlHandle() {
        curl_easy_cleanup(handle);
    }
    CurlHandle(const CurlHandle&)            = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

void Configure(CURL* handle, const std::string& url) {
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
    // A package manager is not a browser: no cookies, no redirects to other
    // schemes, no proxy guessing beyond the environment's own settings.
#if LIBCURL_VERSION_NUM >= 0x075502
    // As in extensions/net/HTTP: 7.85.2 replaced the CURLPROTO_* bitmask with a
    // string, and the bitmask spelling is what an older libcurl has. Both mean the
    // same thing here, and Ubuntu's 7.81 has only the bitmask -- zpkg is built
    // against the distro's libcurl, so the string spelling cannot be assumed. The
    // name lives in the branch that uses it, so the other has nothing to warn about.
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}

auto Check(CURLcode code, const std::string& url) -> std::expected<void, Fail> {
    if (code != CURLE_OK) {
        return std::unexpected(Fail {std::string("cannot fetch ") + url + ": " + curl_easy_strerror(code)});
    }
    return {};
}
#endif

} // namespace

std::string UrlJoin(std::string_view base, std::string_view leaf) {
    std::string text(base);
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    text += '/';
    text += leaf;
    return text;
}

auto FetchToFile(std::string_view url, const fs::path& destination) -> std::expected<void, Fail> {
    const std::string target(url);
    if (!IsHttp(target)) {
        const fs::path  source = FromUtf8(StripFileScheme(target));
        std::error_code ec;
        if (!fs::is_regular_file(Native(source), ec)) {
            return std::unexpected(Fail {"remote has no file at " + ToUtf8(source)});
        }
        if (source.lexically_normal() == destination.lexically_normal())
            return {};
        EnsureDirectories(destination.parent_path());
        fs::copy_file(Native(source), Native(destination), fs::copy_options::overwrite_existing, ec);
        if (ec)
            return std::unexpected(Fail {"cannot copy " + ToUtf8(source) + ": " + ec.message()});
        return {};
    }
#if defined(ZPKG_HAVE_CURL)
    return OutputFile::Create(destination).and_then([&](OutputFile file) -> std::expected<void, Fail> {
        CurlHandle curl;
        return curl.Init()
            .and_then([&]() -> std::expected<void, Fail> {
                Configure(curl.handle, target);
                curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, WriteToFile);
                curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, file.Handle());
                const CURLcode code = curl_easy_perform(curl.handle);
                if (code != CURLE_OK) {
                    std::error_code ignored;
                    fs::remove(Native(destination), ignored);
                    return std::unexpected(Fail {std::string("cannot fetch ") + target + ": " + curl_easy_strerror(code)});
                }
                return {};
            })
            .and_then([&file]() { return file.Flush(); });
    });
#else
    return std::unexpected(Fail {
        "this zpkg was built without libcurl, so it cannot fetch " + target + "\n       Use a local directory or a file:// remote, or rebuild with libcurl."
    });
#endif
}

auto FetchToString(std::string_view url) -> std::expected<std::optional<std::string>, Fail> {
    const std::string target(url);
    if (!IsHttp(target)) {
        const fs::path  source = FromUtf8(StripFileScheme(target));
        std::error_code ec;
        if (!fs::is_regular_file(Native(source), ec))
            return std::optional<std::string> {};
        auto text = ReadFile(source);
        if (!text)
            return std::unexpected(std::move(text.error()));
        return std::optional<std::string>(std::move(*text));
    }
#if defined(ZPKG_HAVE_CURL)
    CurlHandle curl;
    auto       ready = curl.Init();
    if (!ready)
        return std::unexpected(std::move(ready.error()));
    Configure(curl.handle, target);
    std::string body;
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &body);
    const CURLcode code = curl_easy_perform(curl.handle);
    if (code == CURLE_HTTP_RETURNED_ERROR || code == CURLE_FILE_COULDNT_READ_FILE || code == CURLE_REMOTE_FILE_NOT_FOUND) {
        return std::optional<std::string> {};
    }
    auto checked = Check(code, target);
    if (!checked)
        return std::unexpected(std::move(checked.error()));
    return std::optional<std::string>(std::move(body));
#else
    return std::unexpected(Fail {"this zpkg was built without libcurl, so it cannot fetch " + target});
#endif
}

// ---------------------------------------------------------------------------
// Remote installation
// ---------------------------------------------------------------------------
namespace {

struct RemoteSelection {
    std::string               baseUrl;
    IndexToolchain            toolchain;
    std::vector<IndexPackage> packages;
    // False for a format-1 index, which names no dependencies: the closure then
    // has to be discovered from the archives' manifests as they are fetched.
    bool indexHasDependencies = false;
};

// The package that satisfies `requirement`: by package name first, then by
// component or provided target, the same way a user's token is matched.
auto FindProvider(const IndexToolchain& toolchain, const std::string& requirement) -> const IndexPackage* {
    for (const auto& candidate: toolchain.packages) {
        if (candidate.package == requirement)
            return &candidate;
    }
    for (const auto& candidate: toolchain.packages) {
        if (PackageMatches(candidate, requirement))
            return &candidate;
    }
    return nullptr;
}

// A requirement no package in the toolchain provides is the one thing an install
// cannot work around. The message names the missing requirements, not the
// packages that wanted them, because the fix is the same for all of them.
auto MissingFailure(const std::string& toolchain, const std::vector<std::string>& missing, const std::string& origin) -> Fail {
    return Fail {
        origin + ": toolchain " + toolchain + " does not ship " + Join(missing, ", ") +
        "\n       a package in this selection requires it, so the prefix would install and then fail at find_package()"
        "\n       install the package that provides it, pick a complete toolchain, or pass --ignore-missing"
    };
}

auto SelectRemote(Store& store, const std::string& name, const InstallOptions& options, bool requireSignatures) -> std::expected<RemoteSelection, Fail> {
    auto remotes = store.Remotes();
    if (!remotes)
        return std::unexpected(std::move(remotes.error()));
    const auto found = remotes->find(name);
    if (found == remotes->end()) {
        return std::unexpected(Fail {
            "no remote named '" + name +
            "'. 'zpkg remote list' shows the configured ones, "
            "'zpkg remote add " +
            name + " <url>' adds it."
        });
    }
    const std::string indexUrl = UrlJoin(found->second, "index.json");
    auto              fetched  = FetchToString(indexUrl);
    if (!fetched)
        return std::unexpected(std::move(fetched.error()));
    if (!fetched->has_value()) {
        return std::unexpected(Fail {"remote '" + name + "' has no index.json at " + indexUrl + "\n       Publish one with 'zpkg index <dir>'."});
    }

    // For a remote, the index is the signed document: it carries the hash of
    // every archive, so one signature covers the whole release.
    const std::string signatureUrl  = indexUrl + ".ed25519";
    auto              signatureText = FetchToString(signatureUrl);
    if (!signatureText)
        return std::unexpected(std::move(signatureText.error()));
    if (requireSignatures) {
        if (!signatureText->has_value()) {
            return std::unexpected(Fail {"remote '" + name + "' publishes an unsigned index (" + signatureUrl + " is missing)"});
        }
        auto signatures = ParseSignatureFile(**signatureText);
        if (!signatures)
            return std::unexpected(std::move(signatures.error()));
        std::string failure;
        bool        verified = false;
        for (const auto& signature: *signatures) {
            auto key = store.TrustedKey(signature.fingerprint);
            if (!key)
                return std::unexpected(std::move(key.error()));
            if (!key->has_value()) {
                failure += "\n       no trusted key with fingerprint " + signature.fingerprint;
                continue;
            }
            auto checked = Ed25519Verify(**key, **fetched, signature.signature);
            if (!checked)
                return std::unexpected(std::move(checked.error()));
            if (*checked) {
                log::Say("  signed     " + signature.fingerprint);
                verified = true;
                break;
            }
            failure += "\n       key " + signature.fingerprint + " does not verify the index";
        }
        if (!verified) {
            return std::unexpected(Fail {"no trusted signature covers " + indexUrl + ":" + failure});
        }
    } else if (signatureText->has_value()) {
        auto signatures = ParseSignatureFile(**signatureText);
        if (!signatures)
            return std::unexpected(std::move(signatures.error()));
        for (const auto& signature: *signatures) {
            auto key = store.TrustedKey(signature.fingerprint);
            if (!key)
                return std::unexpected(std::move(key.error()));
            if (!key->has_value())
                continue;
            auto checked = Ed25519Verify(**key, **fetched, signature.signature);
            if (!checked)
                return std::unexpected(std::move(checked.error()));
            if (*checked) {
                log::Say("  signed     " + signature.fingerprint);
                break;
            }
        }
    }

    auto index = ParseIndexJson(**fetched, indexUrl);
    if (!index)
        return std::unexpected(std::move(index.error()));
    RemoteSelection selection;
    selection.baseUrl = found->second;

    std::optional<IndexToolchain> chosen;
    for (const auto& toolchain: index->toolchains) {
        if (!options.toolchain.empty() && toolchain.id == options.toolchain)
            chosen = toolchain;
    }
    if (!chosen) {
        if (index->toolchains.empty()) {
            return std::unexpected(Fail {"remote '" + name + "' publishes no toolchains"});
        }
        if (options.toolchain.empty()) {
            if (index->toolchains.size() > 1) {
                return std::unexpected(Fail {
                    "remote '" + name + "' publishes " + std::to_string(index->toolchains.size()) + " toolchains; pick one with --toolchain <id>:\n         " +
                    Join(
                        [&] {
                            std::vector<std::string> ids;
                            for (const auto& toolchain: index->toolchains) {
                                ids.push_back(toolchain.id + (toolchain.profile.empty() ? "" : "  (" + toolchain.profile + ")"));
                            }
                            return ids;
                        }(),
                        "\n         "
                    )
                });
            }
            chosen = index->toolchains.front();
        } else {
            return std::unexpected(Fail {"remote '" + name + "' has no toolchain '" + options.toolchain + "'"});
        }
    }
    selection.toolchain = *chosen;

    selection.indexHasDependencies = ToolchainHasDependencies(selection.toolchain);
    for (const auto& package: selection.toolchain.packages) {
        if (!options.only.empty()) {
            bool keep = false;
            for (const auto& want: options.only) {
                if (PackageMatches(package, want))
                    keep = true;
            }
            if (!keep)
                continue;
        }
        selection.packages.push_back(package);
    }
    if (selection.packages.empty()) {
        // A name that matches nothing is a mistake with a short list of right
        // answers, so print them instead of installing the whole toolchain the
        // way a token-blind parser used to.
        std::vector<std::string> published;
        for (const auto& package: selection.toolchain.packages)
            published.push_back(package.package);
        return std::unexpected(Fail {
            "remote '" + name + "' has nothing matching '" + Join(options.only, "', '") + "' in toolchain " + selection.toolchain.id +
            "\n       it publishes: " + Join(published, ", ") + "\n       omit the name to install the whole toolchain"
        });
    }
    return selection;
}

} // namespace


// ---------------------------------------------------------------------------
// Selection and resolution
// ---------------------------------------------------------------------------
bool PackageMatches(const IndexPackage& package, std::string_view want) {
    if (want.empty())
        return false;
    const std::string token = AsciiLower(std::string(want));
    if (token == AsciiLower(package.package) || token == AsciiLower(package.component))
        return true;
    if ("zahlen-" + token == AsciiLower(package.package))
        return true;
    for (const auto& provided: package.provides) {
        if (AsciiLower(provided) == token)
            return true;
        // "Zahlen::Engine" answers to "engine" as well as to its full name.
        const size_t separator = provided.find("::");
        if (separator != std::string::npos && AsciiLower(provided.substr(separator + 2)) == token)
            return true;
    }
    return false;
}

bool ToolchainHasDependencies(const IndexToolchain& toolchain) {
    for (const auto& package: toolchain.packages) {
        if (!package.dependencies.empty())
            return true;
    }
    return false;
}

auto ResolveClosure(const IndexToolchain& toolchain, const std::vector<IndexPackage>& roots) -> Resolution {
    Resolution               resolution;
    std::vector<std::string> done;
    std::vector<std::string> active;
    // Depth-first so dependencies land before their dependents, which is the
    // order a store wants to write and a reader wants to read. A repeated visit
    // is dropped (`done`); a cycle is cut (`active`) rather than refused, because
    // a package that needs itself is a packaging bug the install still survives.
    auto visit = [&](auto&& self, const IndexPackage& package) -> void {
        if (std::find(done.begin(), done.end(), package.package) != done.end())
            return;
        if (std::find(active.begin(), active.end(), package.package) != active.end())
            return;
        active.push_back(package.package);
        for (const auto& requirement: package.dependencies) {
            const IndexPackage* provider = FindProvider(toolchain, requirement);
            if (provider == nullptr) {
                if (std::find(resolution.missing.begin(), resolution.missing.end(), requirement) == resolution.missing.end())
                    resolution.missing.push_back(requirement);
                continue;
            }
            self(self, *provider);
        }
        active.pop_back();
        done.push_back(package.package);
        resolution.packages.push_back(package);
    };
    for (const auto& root: roots)
        visit(visit, root);
    return resolution;
}

auto InstallFromRemote(Store& store, const std::string& name, const InstallOptions& options) -> std::expected<InstallReport, Fail> {
    auto selected = SelectRemote(store, name, options, options.requireSignatures);
    if (!selected)
        return std::unexpected(std::move(selected.error()));
    const RemoteSelection& selection = *selected;
    log::Say(
        "remote " + name + ": toolchain " + selection.toolchain.id + (selection.toolchain.profile.empty() ? "" : " (" + selection.toolchain.profile + ")")
    );

    auto cacheMade = EnsureDirectories(store.CacheDir());
    if (!cacheMade)
        return std::unexpected(std::move(cacheMade.error()));
    InstallOptions local = options;
    // The index hash already covers these archives, and the index is what the
    // signature covers; re-reading a SHA256SUMS the remote may not even publish
    // would only reject valid installs.
    local.prehashed         = true;
    local.requireChecksums  = false;
    local.requireSignatures = false;
    // The selection was resolved below, against the index. Passing --only down to
    // the store would filter a second time and drop exactly the dependencies the
    // resolver had just added -- which is how `--only Engine` installed nothing.
    local.only.clear();

    std::set<std::string> present;
    if (auto installed = store.Packages(selection.toolchain.id)) {
        for (const auto& package: *installed)
            present.insert(package.info.package);
    }

    // Fetch to the content-addressed cache and check the digest against the
    // signed index. Idempotent: a second call for the same package is a no-op.
    const auto ensureCached = [&](const IndexPackage& package) -> std::expected<fs::path, Fail> {
        const fs::path cached = store.CacheDir() / package.sha256;
        if (PathExists(cached))
            return cached;
        const std::string url = UrlJoin(selection.baseUrl, package.file);
        log::Say("  fetch      " + package.file);
        auto fetched = FetchToFile(url, cached);
        if (!fetched)
            return std::unexpected(std::move(fetched.error()));
        auto digest = Sha256FileHex(cached);
        if (!digest)
            return std::unexpected(std::move(digest.error()));
        const std::string actual = *digest;
        if (AsciiLower(package.sha256) != actual) {
            std::error_code ec;
            fs::remove(Native(cached), ec);
            return std::unexpected(Fail {
                package.file +
                ": the downloaded archive does not match the signed "
                "index\n       expected " +
                package.sha256 + "\n       actual   " + actual
            });
        }
        return cached;
    };

    // A selection -- `remote:pkg` or --only -- means that package and everything
    // it needs; no selection means the whole toolchain, which is what a first
    // install of a published release asks for.
    Resolution               resolution;
    std::vector<std::string> missing;

    if (selection.indexHasDependencies) {
        resolution = ResolveClosure(selection.toolchain, selection.packages);
        for (const auto& requirement: resolution.missing) {
            if (present.count(requirement) != 0)
                continue;
            if (std::find(missing.begin(), missing.end(), requirement) == missing.end())
                missing.push_back(requirement);
        }
    } else {
        // A format-1 index names no graph -- every release published before this
        // change is one -- so the closure is read out of the archives' own
        // manifests. Fetching first and installing second means a requirement
        // nothing provides is still refused before a single file is extracted.
        std::vector<IndexPackage>          scanned;
        std::vector<IndexPackage>          frontier = selection.packages;
        std::set<std::string>              known;
        for (const auto& package: frontier)
            known.insert(package.package);
        for (size_t index = 0; index < frontier.size(); ++index) {
            IndexPackage package = frontier[index];
            auto         cached  = ensureCached(package);
            if (!cached)
                return std::unexpected(std::move(cached.error()));
            auto opened = Archive::Open(*cached);
            if (!opened)
                return std::unexpected(std::move(opened.error()));
            auto scannedArchive = opened->Scan();
            if (!scannedArchive)
                return std::unexpected(std::move(scannedArchive.error()));
            package.dependencies = opened->Manifest().dependencies;
            scanned.push_back(package);
            for (const auto& requirement: package.dependencies) {
                if (present.count(requirement) != 0 || known.count(requirement) != 0)
                    continue;
                const IndexPackage* provider = FindProvider(selection.toolchain, requirement);
                if (provider == nullptr) {
                    if (std::find(missing.begin(), missing.end(), requirement) == missing.end())
                        missing.push_back(requirement);
                    continue;
                }
                known.insert(provider->package);
                frontier.push_back(*provider);
            }
        }
        IndexToolchain discovered;
        discovered.id      = selection.toolchain.id;
        discovered.version = selection.toolchain.version;
        discovered.profile = selection.toolchain.profile;
        discovered.packages = std::move(scanned);
        resolution          = ResolveClosure(discovered, discovered.packages);
        resolution.missing.clear(); // already collected, with `present` taken into account
    }

    if (!missing.empty() && !options.ignoreMissing)
        return std::unexpected(MissingFailure(selection.toolchain.id, missing, "remote '" + name + "'"));
    for (const auto& requirement: missing)
        log::Warn("--ignore-missing: no package provides " + requirement);

    if (options.only.empty()) {
        log::Say("  toolchain  " + std::to_string(resolution.packages.size()) + " package(s)");
    } else {
        std::vector<std::string> names;
        for (const auto& package: resolution.packages)
            names.push_back(package.package);
        log::Say(
            "  closure    " + std::to_string(resolution.packages.size()) + " of " + std::to_string(selection.toolchain.packages.size()) + ": " + Join(names, " ")
        );
    }

    InstallReport report;
    for (const auto& package: resolution.packages) {
        if (present.count(package.package) != 0 && !options.force) {
            log::Say("  present    " + package.package);
            continue;
        }
        auto cached = ensureCached(package);
        if (!cached)
            return std::unexpected(std::move(cached.error()));
        auto installed = store.InstallArchive(*cached, local, std::move(report));
        if (!installed)
            return std::unexpected(std::move(installed.error()));
        report = std::move(*installed);
        present.insert(package.package);
    }
    return report;
}

auto VerifyRemote(Store& store, const std::string& name, const InstallOptions& options) -> std::expected<size_t, Fail> {
    return SelectRemote(store, name, options, options.requireSignatures).and_then([&name](RemoteSelection selection) -> std::expected<size_t, Fail> {
        log::Say("remote " + name + ": " + std::to_string(selection.packages.size()) + " package(s) in toolchain " + selection.toolchain.id);
        for (const auto& package: selection.packages) {
            log::Say("  ok         " + package.package + " (" + package.component + ") " + package.sha256.substr(0, 16) + "...");
        }
        return selection.packages.size();
    });
}

auto BuildIndex(const fs::path& directory) -> std::expected<std::vector<IndexToolchain>, Fail> {
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
    std::sort(archives.begin(), archives.end());

    std::vector<IndexToolchain> toolchains;
    for (const auto& archivePath: archives) {
        auto opened = Archive::Open(archivePath);
        if (!opened)
            return std::unexpected(std::move(opened.error()));
        Archive& archive = *opened;
        auto     scanned = archive.Scan();
        if (!scanned)
            return std::unexpected(std::move(scanned.error()));
        const ToolchainInfo& info = archive.Toolchain();
        IndexPackage         package;
        package.package   = archive.Manifest().package;
        package.component = archive.Manifest().component;
        package.file      = archivePath.filename().string();
        auto digest       = Sha256FileHex(archivePath);
        if (!digest)
            return std::unexpected(std::move(digest.error()));
        package.sha256   = *digest;
        package.dependencies = archive.Manifest().dependencies;
        package.provides = archive.Manifest().provides;
        auto size      = FileSizeOf(archivePath);
        if (!size)
            return std::unexpected(std::move(size.error()));
        package.size = *size;

        auto existing = std::find_if(toolchains.begin(), toolchains.end(), [&](const IndexToolchain& candidate) { return candidate.id == info.id; });
        if (existing == toolchains.end()) {
            IndexToolchain toolchain;
            toolchain.id      = info.id;
            toolchain.version = info.version;
            toolchain.profile = info.profile;
            toolchains.push_back(std::move(toolchain));
            existing = std::prev(toolchains.end());
        }
        existing->packages.push_back(std::move(package));
    }
    return toolchains;
}

} // namespace ZHLN
