// zpkg -- command line.
//
// Every command here is a thin layer over ZHLN::Store; if a behaviour is not
// testable through Store, it belongs in Store instead of here.
#include "zpkg/zpkg.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN { namespace {

constexpr int UsageError = 2;

void Usage() {
    std::puts(R"(Usage: zpkg <command> [options]

Packages (the common case)
  install <path|dir>...        Install archives from a directory or file
  install <remote>             Install a configured remote's whole toolchain
  install <remote>:<name>      Install that package and what it requires
  verify <path|dir|remote>     Check archives without installing anything
  list [--files] [--json]      Installed toolchains, active one marked
  info <toolchain-id>          Compiler, profile, ABI, packages, requirements
  use <toolchain-id>           Make a toolchain active (future builds only)
  prefix [toolchain-id]        The prefix to hand to CMake
  env [toolchain-id] [--shell posix|cmd|powershell]
                               Exports for building and running
  which <tool>                 Resolve an engine tool, active or not
  remove <toolchain-id>        Delete a whole toolchain
  remove --package <name>      Delete one package from a toolchain
  shims enable|disable         ~/.local/bin shims for engine tools

Publishing and trust
  index <dir> [--out <file>]   Write index.json describing the archives in <dir>
  key add <file> [--name <n>]  Trust a publisher's ed25519 key
  key list                     Trusted keys
  key remove <fingerprint>     Forget a key
  remote add <name> <url>      Register a remote (path, file://, https://)
  remote list                  Configured remotes
  remote remove <name>         Forget a remote

Install options
  --only <a,b,...>        Only these packages or components, with what they require
  --toolchain <id>        Which toolchain to take from a remote
  --require-checksums     Fail unless SHA256SUMS covers every archive
  -s, --require-signatures
                          Fail unless a trusted key signs the checksums
  --ignore-missing        Install even if a required package is absent, and say so
  --force                 Overwrite, or remove the active toolchain
  -q, --quiet             Suppress per-file progress

Environment
  ZHLN_ROOT      Store root.
                 default: POSIX   ${XDG_DATA_HOME:-$HOME/.local/share}/zahlen
                          Windows %LOCALAPPDATA%\zahlen
  ZHLN_BIN_DIR   Shim directory (POSIX default: $HOME/.local/bin)

Nothing zpkg does requires privileges. It refuses to run elevated unless
ZHLN_ROOT is set explicitly, and refuses /usr, /opt, Program Files and other
system paths as a store root even then.)");
}

std::string Join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0)
            out += separator;
        out += items[i];
    }
    return out;
}

std::vector<std::string> SplitList(std::string_view text) {
    std::vector<std::string> items;
    std::string              current;
    for (char c: text) {
        if (c == ',') {
            if (!current.empty())
                items.push_back(Trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        items.push_back(Trim(current));
    return items;
}

struct Args {
    std::vector<std::string>           positional;
    std::map<std::string, std::string> values;
    std::set<std::string>              flags;

    bool Has(const std::string& flag) const {
        return flags.count(flag) != 0;
    }
    std::string Value(const std::string& key, std::string fallback = {}) const {
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second;
    }
};

// Options that carry a value; everything else starting with '-' is a flag.
auto ParseArgs(int argc, char** argv, int from, const std::set<std::string>& valued) -> std::expected<Args, Fail> {
    Args args;
    for (int i = from; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument.size() > 1 && argument[0] == '-') {
            if (StartsWith(argument, "--") && argument.find('=') != std::string::npos) {
                const size_t equals                         = argument.find('=');
                args.values[argument.substr(2, equals - 2)] = argument.substr(equals + 1);
                continue;
            }
            const std::string key = StartsWith(argument, "--") ? argument.substr(2) : argument.substr(1);
            if (valued.count(key) != 0) {
                if (i + 1 >= argc)
                    return std::unexpected(Fail {"option '--" + key + "' needs a value", UsageError});
                args.values[key] = argv[++i];
            } else {
                args.flags.insert(key);
            }
        } else {
            args.positional.push_back(argument);
        }
    }
    return args;
}

struct Source {
    enum class Kind { Archive, Directory, Remote } kind = Kind::Archive;
    std::string value;
    // The part after the colon in `remote:name`. It is a selection, not a name
    // for the remote: `install up:zahlen-gltf` is `install up --only zahlen-gltf`
    // plus that package's closure. Dropping it -- which is what this program used
    // to do -- turns every package-named install into the whole toolchain.
    std::string packages;
};

Source Classify(const std::string& text) {
    Source          source;
    std::error_code ec;
    if (fs::is_directory(Native(FromUtf8(text)), ec)) {
        source.kind  = Source::Kind::Directory;
        source.value = text;
        return source;
    }
    if (fs::is_regular_file(Native(FromUtf8(text)), ec)) {
        source.kind  = Source::Kind::Archive;
        source.value = text;
        return source;
    }
    const size_t colon = text.find(':');
    if (colon != std::string::npos && colon > 0 && text.find('/') > colon) { // "name:rest", not "C:\dir" or "https://"
        source.kind     = Source::Kind::Remote;
        source.value    = text.substr(0, colon);
        source.packages = text.substr(colon + 1);
        return source;
    }
    source.kind  = Source::Kind::Archive; // will fail with "no such archive"
    source.value = text;
    return source;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
auto CommandInstall(Store& store, const Args& args) -> std::expected<void, Fail> {
    InstallOptions options;
    options.only              = SplitList(args.Value("only"));
    options.requireChecksums  = args.Has("require-checksums");
    options.requireSignatures = args.Has("require-signatures") || args.Has("s");
    options.force             = args.Has("force");
    options.ignoreMissing     = args.Has("ignore-missing");
    options.toolchain         = args.Value("toolchain");
    if (args.positional.empty())
        return std::unexpected(Fail {
            "install needs at least one archive, directory or "
            "remote:<name>",
            UsageError
        });

    InstallReport report;
    for (const auto& text: args.positional) {
        const Source source = Classify(text);
        if (source.kind == Source::Kind::Remote && !source.packages.empty()) {
            // `remote:pkg` and `--only pkg` are the same request written twice.
            // Accepting both invites a reader to think they mean different things.
            if (!options.only.empty()) {
                return std::unexpected(Fail {
                    "'" + text + "' already names a package; drop --only, or drop the name and keep --only",
                    UsageError
                });
            }
            options.only = SplitList(source.packages);
            if (options.only.empty())
                return std::unexpected(Fail {"'" + text + "' has an empty package name after the colon", UsageError});
        }
        switch (source.kind) {
            case Source::Kind::Archive: {
                auto next = store.InstallArchive(FromUtf8(source.value), options, std::move(report));
                if (!next)
                    return std::unexpected(std::move(next.error()));
                report = std::move(*next);
                break;
            }
            case Source::Kind::Directory: {
                auto next = store.InstallDirectory(FromUtf8(source.value), options, std::move(report));
                if (!next)
                    return std::unexpected(std::move(next.error()));
                report = std::move(*next);
                break;
            }
            case Source::Kind::Remote: {
                if (args.positional.size() > 1) {
                    return std::unexpected(Fail {"install one remote at a time", UsageError});
                }
                auto next = InstallFromRemote(store, source.value, options);
                if (!next)
                    return std::unexpected(std::move(next.error()));
                report = std::move(*next);
                break;
            }
        }
    }
    auto finished = store.Finish(std::move(report), options.ignoreMissing);
    if (!finished)
        return std::unexpected(std::move(finished.error()));
    return {};
}

auto CommandVerify(Store& store, const Args& args) -> std::expected<void, Fail> {
    InstallOptions options;
    options.only              = SplitList(args.Value("only"));
    options.requireChecksums  = args.Has("require-checksums");
    options.requireSignatures = args.Has("require-signatures") || args.Has("s");
    if (args.positional.empty())
        return std::unexpected(Fail {"verify needs a path or remote:<name>", UsageError});

    size_t checked = 0;
    for (const auto& text: args.positional) {
        const Source source = Classify(text);
        if (source.kind == Source::Kind::Remote) {
            if (!source.packages.empty()) {
                if (!options.only.empty()) {
                    return std::unexpected(Fail {"'" + text + "' already names a package; drop --only", UsageError});
                }
                options.only = SplitList(source.packages);
            }
            auto remoteChecked = VerifyRemote(store, source.value, options);
            if (!remoteChecked)
                return std::unexpected(std::move(remoteChecked.error()));
            checked += *remoteChecked;
            continue;
        }
        if (source.kind == Source::Kind::Directory) {
            // Every archive gets the full treatment: layout, metadata, checksums
            // and signatures, so `verify` reports exactly what `install` would
            // refuse -- and writes nothing while doing it.
            std::vector<fs::path> archives;
            std::error_code       ec;
            for (const auto& entry: fs::directory_iterator(Native(FromUtf8(source.value)), ec)) {
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
            if (archives.empty()) {
                return std::unexpected(Fail {"no zahlen-*.tar.{gz,zst} archives in " + source.value});
            }
            for (const auto& archive: archives) {
                auto verified = store.VerifyArchive(archive, options);
                if (!verified)
                    return std::unexpected(std::move(verified.error()));
                ++checked;
            }
            continue;
        }
        auto verified = store.VerifyArchive(FromUtf8(source.value), options);
        if (!verified)
            return std::unexpected(std::move(verified.error()));
        ++checked;
    }
    log::Say("verified " + std::to_string(checked) + " source(s)");
    return {};
}

auto CommandIndex(const Args& args) -> std::expected<void, Fail> {
    if (args.positional.size() != 1)
        return std::unexpected(Fail {"index needs exactly one directory", UsageError});
    const fs::path directory  = FromUtf8(args.positional[0]);
    auto           toolchains = BuildIndex(directory);
    if (!toolchains)
        return std::unexpected(std::move(toolchains.error()));
    const std::string json = BuildIndexJson(*toolchains);
    const std::string out  = args.Value("out");
    if (out.empty()) {
        std::fwrite(json.data(), 1, json.size(), stdout);
    } else {
        auto written = WriteFile(FromUtf8(out), json);
        if (!written)
            return std::unexpected(std::move(written.error()));
        log::Say("wrote " + out);
    }
    return {};
}

auto CommandList(Store& store, const Args& args) -> std::expected<void, Fail> {
    auto toolchains = store.Toolchains();
    if (!toolchains)
        return std::unexpected(std::move(toolchains.error()));
    const auto& ids    = *toolchains;
    auto        active = store.Active();
    if (!active)
        return std::unexpected(std::move(active.error()));
    const auto& current = *active;
    if (ids.empty()) {
        log::Say("no toolchains installed in " + ToUtf8(store.Root()));
        return {};
    }
    if (args.Has("files")) {
        for (const auto& id: ids) {
            std::printf("# %s\n", ToUtf8(store.ToolchainDir(id)).c_str());
            auto packages = store.Packages(id);
            if (!packages)
                return std::unexpected(std::move(packages.error()));
            for (const auto& package: *packages) {
                for (const auto& file: package.files)
                    std::printf("%s\n", file.c_str());
            }
        }
        return {};
    }
    if (args.Has("json")) {
        std::printf("{\"format\":%d,\"active\":\"%s\",\"toolchains\":[", PackageFormat, current ? current->c_str() : "");
        bool first = true;
        for (const auto& id: ids) {
            auto info = store.ReadToolchain(id);
            if (!info)
                return std::unexpected(std::move(info.error()));
            std::printf(
                "%s\n  {\"id\":\"%s\",\"profile\":\"%s\",\"version\":\"%s\",\"packages\":[", first ? "" : ",", id.c_str(), info->profile.c_str(),
                info->version.c_str()
            );
            first             = false;
            bool firstPackage = true;
            auto packages     = store.Packages(id);
            if (!packages)
                return std::unexpected(std::move(packages.error()));
            for (const auto& package: *packages) {
                std::printf("%s\"%s\"", firstPackage ? "" : ",", package.info.package.c_str());
                firstPackage = false;
            }
            std::printf("]}");
        }
        std::printf("\n]}\n");
        return {};
    }

    std::printf("%-2s %-56s %-11s %s\n", "", "TOOLCHAIN", "PROFILE", "PACKAGES");
    for (const auto& id: ids) {
        std::string profile = "-";
        // A toolchain whose own descriptor is unreadable is still worth listing:
        // the row says so instead of the command failing.
        if (auto info = store.ReadToolchain(id)) {
            profile = info->profile.empty() ? "-" : info->profile;
        }
        size_t count = 0;
        if (auto packages = store.Packages(id))
            count = packages->size();
        std::printf("%-2s %-56s %-11s %zu\n", (current && *current == id) ? "*" : " ", id.c_str(), profile.c_str(), count);
    }
    if (current) {
        auto prefix = store.Prefix();
        if (!prefix)
            return std::unexpected(std::move(prefix.error()));
        std::printf("\n* active. Point CMake at: %s\n", ToUtf8(*prefix).c_str());
    }
    return {};
}

auto CommandInfo(Store& store, const Args& args) -> std::expected<void, Fail> {
    if (args.positional.size() != 1)
        return std::unexpected(Fail {"info needs a toolchain id", UsageError});
    const std::string id   = args.positional[0];
    auto              info = store.ReadToolchain(id);
    if (!info)
        return std::unexpected(std::move(info.error()));
    std::printf("toolchain   %s\n", id.c_str());
    std::printf("version     %s  (%s %s, %s)\n", info->version.c_str(), info->compiler.c_str(), info->compilerVersion.c_str(), info->processor.c_str());
    std::printf("commit      %s\n", info->commit.c_str());
    std::printf("profile     %s\n", info->profile.c_str());
    std::printf("abi         %s\n", info->abi.c_str());
    auto prefix = store.Prefix(id);
    if (!prefix)
        return std::unexpected(std::move(prefix.error()));
    std::printf("prefix      %s\n\n", ToUtf8(*prefix).c_str());
    auto packages = store.Packages(id);
    if (!packages)
        return std::unexpected(std::move(packages.error()));
    for (const auto& package: *packages) {
        std::printf(
            "package     %-24s -> %s\n", package.info.package.c_str(),
            package.info.dependencies.empty() ? "(no dependencies)" : Join(package.info.dependencies, " ").c_str()
        );
    }
    return {};
}

auto CommandEnv(Store& store, const Args& args) -> std::expected<void, Fail> {
    const std::string id       = args.positional.empty() ? "" : args.positional[0];
    auto              resolved = store.Prefix(id);
    if (!resolved)
        return std::unexpected(std::move(resolved.error()));
    const std::string prefix = ToUtf8(*resolved);
    const std::string shell  = args.Value("shell", "posix");
    if (shell == "posix" || shell == "sh" || shell == "bash") {
        std::printf("# zahlen toolchain environment (eval \"$(zpkg env)\")\n");
        std::printf("export ZHLN_PREFIX='%s'\n", prefix.c_str());
        std::printf("export CMAKE_PREFIX_PATH='%s'\n", prefix.c_str());
        std::printf("export PATH='%s/bin':\"$PATH\"\n", prefix.c_str());
        std::printf("export ZHLN_DATA_DIR='%s/share/zahlen'\n", prefix.c_str());
        std::printf("export LD_LIBRARY_PATH='%s/lib'${LD_LIBRARY_PATH:+\":$LD_LIBRARY_PATH\"}\n", prefix.c_str());
    } else if (shell == "powershell" || shell == "pwsh") {
        std::printf("$env:ZHLN_PREFIX = '%s'\n", prefix.c_str());
        std::printf("$env:CMAKE_PREFIX_PATH = '%s'\n", prefix.c_str());
        std::printf("$env:PATH = '%s\\bin;' + $env:PATH\n", prefix.c_str());
        std::printf("$env:ZHLN_DATA_DIR = '%s\\share\\zahlen'\n", prefix.c_str());
    } else if (shell == "cmd") {
        std::printf("set ZHLN_PREFIX=%s\n", prefix.c_str());
        std::printf("set CMAKE_PREFIX_PATH=%s\n", prefix.c_str());
        std::printf("set PATH=%s\\bin;%%PATH%%\n", prefix.c_str());
        std::printf("set ZHLN_DATA_DIR=%s\\share\\zahlen\n", prefix.c_str());
    } else {
        return std::unexpected(Fail {"unknown shell '" + shell + "' (posix, cmd or powershell)", UsageError});
    }
    return {};
}

auto CommandWhich(Store& store, const Args& args) -> std::expected<void, Fail> {
    if (args.positional.size() != 1)
        return std::unexpected(Fail {"which needs one tool name", UsageError});
    const std::string tool = args.positional[0];
    if (tool.find('/') != std::string::npos || tool.find('\\') != std::string::npos) {
        return std::unexpected(Fail {"give a tool name, not a path", UsageError});
    }
    std::error_code ec;
    if (auto active = store.Prefix()) {
        const fs::path activeTool = *active / "bin" / tool;
        if (fs::is_regular_file(Native(activeTool), ec)) {
            std::printf("%s\n", ToUtf8(activeTool).c_str());
            return {};
        }
    }
    auto toolchains = store.Toolchains();
    if (!toolchains)
        return std::unexpected(std::move(toolchains.error()));
    bool found = false;
    for (const auto& id: *toolchains) {
        const fs::path candidate = store.ToolchainDir(id) / "usr" / "bin" / tool;
        if (fs::is_regular_file(Native(candidate), ec)) {
            std::printf("%s (inactive: %s)\n", ToUtf8(candidate).c_str(), id.c_str());
            found = true;
        }
    }
    if (!found)
        return std::unexpected(Fail {"'" + tool + "' is not provided by any installed toolchain"});
    // Found, but not in the active toolchain: exit 1 without an error line, which
    // is what a shell asking "can I run this after `zpkg use`?" wants to see.
    return std::unexpected(Fail {"", 1});
}

auto CommandKey(Store& store, const Args& args) -> std::expected<void, Fail> {
    if (args.positional.empty())
        return std::unexpected(Fail {"key needs add, list or remove", UsageError});
    const std::string action = args.positional[0];
    if (action == "list") {
        auto trusted = store.Keys();
        if (!trusted)
            return std::unexpected(std::move(trusted.error()));
        if (trusted->empty()) {
            log::Say("no trusted keys in " + ToUtf8(store.Root() / "keys"));
            return {};
        }
        for (const auto& [fingerprint, name]: *trusted) {
            std::printf("%s  %s\n", fingerprint.c_str(), name.empty() ? "" : name.c_str());
        }
        return {};
    }
    if (action == "add") {
        if (args.positional.size() != 2)
            return std::unexpected(Fail {"key add needs a key file", UsageError});
        return store.KeyAdd(FromUtf8(args.positional[1]), args.Value("name"));
    }
    if (action == "remove") {
        if (args.positional.size() != 2) {
            return std::unexpected(Fail {"key remove needs a fingerprint", UsageError});
        }
        return store.KeyRemove(AsciiLower(args.positional[1]));
    }
    return std::unexpected(Fail {"unknown key action '" + action + "'", UsageError});
}

auto CommandRemote(Store& store, const Args& args) -> std::expected<void, Fail> {
    if (args.positional.empty())
        return std::unexpected(Fail {"remote needs add, list or remove", UsageError});
    const std::string action = args.positional[0];
    if (action == "list") {
        auto remotes = store.Remotes();
        if (!remotes)
            return std::unexpected(std::move(remotes.error()));
        if (remotes->empty()) {
            log::Say("no remotes configured");
            return {};
        }
        for (const auto& [name, url]: *remotes) {
            std::printf("%-16s %s\n", name.c_str(), url.c_str());
        }
        return {};
    }
    if (action == "add") {
        if (args.positional.size() != 3) {
            return std::unexpected(Fail {"remote add needs a name and a url", UsageError});
        }
        return store.RemoteAdd(args.positional[1], args.positional[2]);
    }
    if (action == "remove") {
        if (args.positional.size() != 2) {
            return std::unexpected(Fail {"remote remove needs a name", UsageError});
        }
        return store.RemoteRemove(args.positional[1]);
    }
    return std::unexpected(Fail {"unknown remote action '" + action + "'", UsageError});
}

auto Run(int argc, char** argv) -> std::expected<void, Fail> {
    if (argc < 2) {
        Usage();
        return std::unexpected(Fail {"no command given", UsageError});
    }
    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        Usage();
        return {};
    }
    if (command == "--version" || command == "version") {
        std::printf("zpkg %s (package format %d)\n", std::string(ToolVersion).c_str(), PackageFormat);
        return {};
    }

    // Options that take a value. Everything else that starts with '-' is a flag.
    const std::set<std::string> valued = {"only", "toolchain", "shell", "out", "name", "package"};
    auto                        parsed = ParseArgs(argc, argv, 2, valued);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    const Args& args = *parsed;
    log::SetQuiet(args.Has("quiet") || args.Has("q"));

    // `index` reads the directory it is handed and writes --out; it never opens a
    // store, so the elevated-install guard in ResolveEnv does not apply to it. That
    // guard has to stay for the commands that do write into a user's data directory
    // (`zpkg install` as root is still refused below), but gating `index` on it broke
    // the release packaging: scripts/package-toolchain.sh runs zpkg inside a build
    // container as root, where ResolveEnv failed with "refusing to run elevated".
    if (command == "index")
        return CommandIndex(args);

    auto resolved = ResolveEnv(args.Has("force"));
    if (!resolved)
        return std::unexpected(std::move(resolved.error()));
    const Env env = *resolved;
    Store     store(env, args.Has("force"));

    // Mutating commands take the store lock; read-only ones do not, so `zpkg
    // list` still answers while an install is running.
    const bool mutating = command == "install" || command == "remove" || command == "use" || command == "key" || command == "remote" || command == "shims";
    std::optional<StoreLock> lock;
    if (mutating) {
        auto acquired = StoreLock::Acquire(env.root);
        if (!acquired)
            return std::unexpected(std::move(acquired.error()));
        lock.emplace(std::move(*acquired));
    }

    if (command == "install")
        return CommandInstall(store, args);
    if (command == "verify")
        return CommandVerify(store, args);
    if (command == "list")
        return CommandList(store, args);
    if (command == "info")
        return CommandInfo(store, args);
    if (command == "env")
        return CommandEnv(store, args);
    if (command == "which")
        return CommandWhich(store, args);
    if (command == "key")
        return CommandKey(store, args);
    if (command == "remote")
        return CommandRemote(store, args);
    if (command == "prefix") {
        auto prefix = store.Prefix(args.positional.empty() ? "" : args.positional[0]);
        if (!prefix)
            return std::unexpected(std::move(prefix.error()));
        std::printf("%s\n", ToUtf8(*prefix).c_str());
        return {};
    }
    if (command == "use") {
        if (args.positional.size() != 1)
            return std::unexpected(Fail {"use needs exactly one toolchain id", UsageError});
        const std::string id     = args.positional[0];
        auto              active = store.Active();
        if (!active)
            return std::unexpected(std::move(active.error()));
        if (active->has_value() && **active != id) {
            log::Warn("switching changes future builds only: already-linked binaries keep the "
                      "toolchain they were built against");
        }
        auto switched = store.SetActive(id);
        if (!switched)
            return std::unexpected(std::move(switched.error()));
        log::Say("active toolchain: " + id);
        auto prefix = store.Prefix();
        if (!prefix)
            return std::unexpected(std::move(prefix.error()));
        log::Say("prefix: " + ToUtf8(*prefix));
        return {};
    }
    if (command == "remove") {
        if (args.positional.empty() && args.Value("package").empty()) {
            return std::unexpected(Fail {"remove needs a toolchain id, or --package <name>", UsageError});
        }
        if (!args.Value("package").empty()) {
            auto removed = store.RemovePackage(args.Value("package"), args.positional.empty() ? "" : args.positional[0]);
            if (!removed)
                return std::unexpected(std::move(removed.error()));
        } else {
            auto removed = store.RemoveToolchain(args.positional[0], args.Has("force"));
            if (!removed)
                return std::unexpected(std::move(removed.error()));
        }
        return {};
    }
    if (command == "shims") {
        if (args.positional.empty())
            return std::unexpected(Fail {"shims needs enable or disable", UsageError});
        if (args.positional[0] == "enable") {
            return store.ShimsEnable();
        } else if (args.positional[0] == "disable") {
            return store.ShimsDisable();
        } else {
            return std::unexpected(Fail {"shims needs enable or disable", UsageError});
        }
        return {};
    }
    return std::unexpected(Fail {"unknown command '" + command + "' ('zpkg --help' lists them)", UsageError});
}

}} // namespace ZHLN

// The single place that turns a failure into an exit status. Everything below
// reports by returning; nothing unwinds.
int main(int argc, char** argv) {
    auto result = ZHLN::Run(argc, argv);
    if (!result) {
        std::fprintf(stderr, "zpkg: error: %s\n", result.error().message.c_str());
        return result.error().code;
    }
    return 0;
}
