# zpkg

zpkg installs engine packages into a per-user store and selects the active
toolchain. For game setup, see the [consumer guide](../../docs/CONSUMING.md).
For command syntax, see [Commands](#commands).

## Version and trust

`zpkg --version` reports the manager's own CalVer (`YYYY.MM.DD`). The installed
engine toolchain is shown by `zpkg list` / `zpkg info`.

The project signing key is pinned at
[`packaging/keys/zahlen-release.pub`](../../packaging/keys/zahlen-release.pub)
and embedded in `zpkg` at build time. Official release signatures are verified
against this key. Other publishers' keys are stored per user and can be added
with `zpkg key add`; planned key rotations use a dual-signed transition release.
See [Upgrading](../../docs/UPGRADING.md).

## Install a release

Run `zpkg install` to get Application and its dependencies. Component names
can be shortened:

```sh
zpkg install application
zpkg install application image --version v0.4.0
zpkg install application --from studio
```

The built-in `official` source uses GitHub Releases, with `latest` as the default
release. `--version` selects an exact release tag. Official installs and
`--from` component installs require a trusted signature over the index and
verify archive hashes. Keys are never downloaded; the official key is built
in, and other publishers' keys are trusted per store with `zpkg key add`.

Existing local paths take precedence over component names. A bare configured
remote installs its whole toolchain; `remote:component` retains the explicit
remote syntax. Those advanced forms retain their existing verification flags.
Use `./name` to disambiguate a local path, or `--from official` to force release
component selection. `official` is a built-in source, not a configurable remote.

The first successful installation activates its toolchain; subsequent installs
preserve the active selection. Use `zpkg use <id>` to switch. `zpkg install
--help` lists install options. Unknown options and missing values are errors.

## Build isolation

`utils/zpkg` is a standalone C++23 project and does not link the engine. GCC
builds link libstdc++ statically.

## Building

```sh
cmake -S utils/zpkg -B build-zpkg -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-zpkg
ctest --test-dir build-zpkg --output-on-failure
```

Build dependencies:

| Requirement | Why | If missing |
|---|---|---|
| simdjson | parses `toolchain.json`, package manifests and the remote index | build error; pass `-DZPKG_SIMDJSON_DIR=` or init `extern/simdjson` |
| zlib | `.tar.gz` | build error |
| zstd | `.tar.zst` | `.tar.zst` refused, `.tar.gz` still works |
| OpenSSL | ed25519 signature verification | built without `--require-signatures` |
| libcurl | `https://` remotes | `file://` and local paths still work |

## How failures are reported

Fallible operations return `std::expected<T, Fail>`:

```cpp
struct Fail { std::string message; int code = 1; };

auto InstallArchive(const fs::path& archive, const InstallOptions&, InstallReport report)
    -> std::expected<InstallReport, Fail>;
[[nodiscard]] static auto Open(fs::path path) -> std::expected<Archive, Fail>;
```

The build disables exceptions and RTTI. Resource acquisition uses factories
such as `InputFile::Open`, `StoreLock::Acquire`, and `Archive::Open`.
Propagate errors with `std::unexpected`; use `and_then` for dependent operations
and early returns for validation loops. These types are internal; the external
interfaces are the CLI and store format.

## Windows

Build with a C++23 compiler and the dependencies listed above.

The default store is `%LOCALAPPDATA%\zahlen`; `ZHLN_ROOT` overrides it. Four
things behave differently, all of them because Windows has no POSIX file model:

* `shims` is refused. There is no `~/.local/bin` and no symlink to follow;
  `zpkg env --shell powershell` (or `cmd`) emits the export that puts the active
  toolchain on `PATH` instead.
* Archives carrying symbolic links need developer mode or elevation, which zpkg
  reports when unavailable. Hard links fall back to a copy.
* File modes are not applied. Windows reports the same `rw-rw-rw-` for every
  file and can only express the read-only attribute, so an archive's `0755` is
  ignored -- executability there comes from the extension. The invariant zpkg
  keeps instead is that installed files stay replaceable.
* Paths past `MAX_PATH` use the extended-length form, which only some Win32
  entry points accept, so directory creation walks components with
  `CreateDirectoryW` and file access goes through `_wfopen` rather than
  `std::ofstream` (which would also convert the path to the ANSI code page).

## Store layout

```
$ZHLN_ROOT/store/<toolchain-id>/usr/{bin,lib,include,share}   # immutable payload
$ZHLN_ROOT/store/<toolchain-id>/zpkg/                         # what zpkg installed
$ZHLN_ROOT/store/ACTIVE                                       # the active id
$ZHLN_ROOT/active -> store/<toolchain-id>                     # POSIX mirror of ACTIVE
$ZHLN_ROOT/keys/<fingerprint>.key                             # trusted publishers
$ZHLN_ROOT/remotes/<name>                                     # where a name resolves
$ZHLN_ROOT/cache/<sha256>                                     # downloaded archives
```

`$ZHLN_ROOT` defaults to `${XDG_DATA_HOME:-~/.local/share}/zahlen` on POSIX and
`%LOCALAPPDATA%\zahlen` on Windows. `ACTIVE` is a text file, not a symlink,
because Windows only creates symlinks in developer mode; on POSIX zpkg also
maintains the relative `active` link for humans and tools that expect a path.

Store behavior:

* **Privilege restrictions.** zpkg refuses to run elevated unless `ZHLN_ROOT` is set
  explicitly, and refuses `/usr`, `/opt`, `/etc`, `%ProgramFiles%` and friends as
  a store root even then.
* **Toolchain selection.** Each toolchain has its own directory. `zpkg use`
  updates `ACTIVE` for subsequent lookups; it does not rewrite existing binaries
  or their runtime search paths.
* **Archive validation.** Payload paths must be under `usr/`. Every tar
  member is validated before extraction: absolute paths, `..`, backslashes, drive letters, colons, control
  characters, devices, duplicate files, and any symlink that is a prefix of
  another member are all refused. The archive is validated in a first pass and
  extracted in a second, which also detects a file that changed between them.
* **Package removal.** `remove --package` uses the file list recorded at install
  time to select files for deletion.
* **The store is relocatable.** `ACTIVE` holds an id, the POSIX link is relative,
  and no file in the store records an absolute path.

## Commands

| Command | Effect |
|---|---|
| `install [component ...] [--version <tag>] [--from <remote>]` | signed component install (default: Application from the latest official release) |
| `sync [--version <tag>] [--from <remote>] [--activate] [--dry-run]` | install the active toolchain's package set from a newer release, optionally activate it |
| `install <path\|dir>...` | verify, extract into `store/<id>/`, record the package |
| `install <remote>` | install that remote's whole toolchain |
| `install <remote>:<name>` | install that package and everything it requires |
| `verify <path\|dir\|remote>` | the same checks as install, writing nothing |
| `list [--files] [--json]` | installed toolchains, active marked |
| `use <id>` | repoint `ACTIVE` (future builds only) |
| `prefix [id]` | the prefix to hand to CMake |
| `env [id] [--shell posix\|cmd\|powershell]` | exports for building and running |
| `info <id>` | compiler, profile, ABI, packages, requirements |
| `which <tool>` | resolve an engine tool, active or not |
| `remove <id>` / `remove --package <name>` | delete a toolchain / one package |
| `shims enable\|disable` | `~/.local/bin` shims that follow `ACTIVE` |
| `index <dir>` | write `index.json` describing the archives in a directory |
| `key add/list/remove` | trust a publisher's ed25519 key |
| `remote add/list/remove` | register a path, `file://` or `https://` remote |

Install options: `--from <remote>`, `--version <tag>`, `--only a,b`, `--toolchain <id>`, `--require-checksums`,
`-s/--require-signatures`, `--ignore-missing`, `--force`, `--quiet`.
Sync options: `--from`, `--version`, `--activate`, `--dry-run`, `--force`, `--quiet`.

`sync` never updates a toolchain in place. It reads the package names recorded
in the active toolchain, resolves their closure against the target release's
signed `index.json`, and installs into that release's own immutable toolchain
directory. Without `--activate` it leaves `ACTIVE` alone and prints the
`zpkg use <id>` command; with `--activate` it switches and warns that binaries
already linked keep the toolchain they were built against.

## What a name selects

`install <remote>:<name>` installs `name` **and its closure**, dependencies
first. `<name>` may be a package (`zahlen-gltf`), a component (`GLTF`) or an
exported target (`Zahlen::GLTF`); a name the remote does not publish is an error
that lists what it does publish. Omitting the name installs the whole toolchain,
including all available packages.

```
$ zpkg install upstream:zahlen-gltf
  closure    3 of 16: zahlen-engine zahlen-uischema zahlen-serialization zahlen-gltf
```

`--only a,b` is the same request written the other way, and takes the same
closure. Naming a package and passing `--only` together is refused rather than
merged, because the two spellings would then disagree about what was asked for.

The graph comes from the remote index when it carries one (`index.json` format 2,
which `zpkg index` writes). A format-1 index names no dependencies, so zpkg
fetches the selected archives, reads their
`requires[]` from `usr/share/zahlen/manifests/<package>.json`, and resolves from
those. Either way the archives stay the authority; the index only saves the
download of everything that is not in the closure.

`install` exits non-zero and lists missing required packages. Use
`--ignore-missing` only for staged installs that will supply those packages later.

## Signing

`SHA256SUMS.ed25519` and `index.json.ed25519` are one line per signer:

```
<fingerprint-hex> <base64 ed25519 signature>
```

The signature covers the exact bytes of the file named before the extension.
The project key is built into `zpkg` and pinned at
[`packaging/keys/`](../../packaging/keys/README.md). Other publishers' keys are
trusted per user with `zpkg key add <key.pub> --name <who>`. Multiple signer
lines are allowed; a signature from any trusted key is accepted. A planned key
rotation uses a dual-signed transition release. zpkg verifies signatures but
does not sign releases; a release job does:

```sh
openssl genpkey -algorithm ed25519 -out release-key.pem
openssl pkey -in release-key.pem -pubout -outform DER | tail -c 32 | base64 > key.pub
openssl pkeyutl -sign -inkey release-key.pem -rawin -in SHA256SUMS -out sig.bin
printf '%s ' "$(fingerprint)" && base64 sig.bin
```

For publishing, see [package metadata](../../packaging/README.md#publishing-metadata).

## Tests

`utils/zpkg/tests/main.cpp` builds with the project and runs under `ctest`. It
covers the SHA-256 and base64 primitives, all three `SHA256SUMS` dialects, strict
JSON refusals, an index round trip, archive extraction and fifteen hostile
archives, the store lifecycle and its lock, checksum/signature policy with real
ed25519 keys, remotes over `file://`, and the path rules.

Signature test coverage depends on whether the build includes OpenSSL.

## Style

Use PascalCase for functions and types, camelCase for data, and `m_` for private
members. Constants have no `k` prefix. Preserve external API spellings.
The rules are configured in `utils/zpkg/.clang-tidy`. To apply them:

```sh
cmake -S utils/zpkg -B build/zpkg -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build/zpkg --fix --fix-notes src/*.cpp tests/main.cpp
```
