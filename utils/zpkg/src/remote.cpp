// Remotes: where packages can come from, and how an index is published.
//
// A remote is a directory of files. It may be a path, a file:// URL, or an
// https:// URL when zpkg was built with libcurl. There is no protocol to
// implement and no server to run: publishing a toolchain means running
// `zpkg index` over the artifacts and copying the directory somewhere.
#include "zpkg/zpkg.hpp"
#include <algorithm>
#include <cstdio>
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
};

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

    for (const auto& package: selection.toolchain.packages) {
        PackageInfo probe;
        probe.package   = package.package;
        probe.component = package.component;
        if (!options.only.empty()) {
            bool keep = false;
            for (const auto& want: options.only) {
                if (want == package.package || want == package.component || "zahlen-" + AsciiLower(want) == package.package) {
                    keep = true;
                }
            }
            if (!keep) {
                log::Say("  skip       " + package.package + " (--only)");
                continue;
            }
        }
        selection.packages.push_back(package);
    }
    if (selection.packages.empty())
        return std::unexpected(Fail {"nothing selected from remote '" + name + "'"});
    return selection;
}

} // namespace

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

    InstallReport report;
    for (const auto& package: selection.packages) {
        const fs::path cached = store.CacheDir() / package.sha256;
        if (!PathExists(cached)) {
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
        }
        auto installed = store.InstallArchive(cached, local, std::move(report));
        if (!installed)
            return std::unexpected(std::move(installed.error()));
        report = std::move(*installed);
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
        package.sha256 = *digest;
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
