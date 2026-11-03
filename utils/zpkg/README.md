# zpkg

The Zahlen toolchain manager: installs engine packages into a directory the
developer owns, keeps every toolchain separate, and switches between them with a
pointer. No daemon, no database, no dependency solver, no root.

```
$ zpkg install ./toolchain-output --require-checksums --require-signatures
  sha256 ok  0e2199a923a4d561...
  signed     a56f53d2564fcab20024ca3e0fddcfac
  installed  zahlen-engine (Engine, 241 files) -> store/0.1.0-7c6e062-gcc-16.2.1-x86_64-release
active toolchain: 0.1.0-7c6e062-gcc-16.2.1-x86_64-release
prefix: /home/dev/.local/share/zahlen/store/0.1.0-7c6e062-gcc-16.2.1-x86_64-release/usr

$ cmake -S my_game -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(zpkg prefix)"
```

## Why it is a standalone project

`utils/zpkg/CMakeLists.txt` is its own `project()`, and it is never added to the
engine build. Three things in this repository would otherwise make the manager
depend on what it manages:

* `cmake/Sanitizers.cmake` calls `add_compile_options(-fsanitize=...)` from the
  top level, so an in-tree zpkg would need `libasan.so` on a user's machine.
* The engine asks for `-mavx2 -mbmi -mlzcnt -mf16c -mfma`. A manager that only
  runs on the CPU it installs packages for cannot install the first toolchain on
  the machine that needs one.
* `cmake/toolchain/PackageConfig.cmake.in` rejects a consumer built against a
  different libstdc++ ABI. A manager built inside that build has the same
  question pointed at itself; zpkg links libstdc++ statically on GCC so it never
  has to answer it.

## Building

```sh
cmake -S utils/zpkg -B build-zpkg -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-zpkg
ctest --test-dir build-zpkg --output-on-failure   # 13 suites, 225 checks
```

Dependencies, all of them already in the project's dependency set:

| Requirement | Why | If missing |
|---|---|---|
| simdjson | parses `toolchain.json`, package manifests and the remote index | build error; pass `-DZPKG_SIMDJSON_DIR=` or init `extern/simdjson` |
| zlib | `.tar.gz` | build error |
| zstd | `.tar.zst` | `.tar.zst` refused, `.tar.gz` still works |
| OpenSSL | ed25519 signature verification | built without `--require-signatures` |
| libcurl | `https://` remotes | `file://` and local paths still work |

## How failures are reported

zpkg does not throw. It is built with `-fno-exceptions -fno-rtti`, so a `throw`
or a `try` is a compile error rather than something to remember, and every
fallible function says so in its signature:

```cpp
struct Fail { std::string message; int code = 1; };

auto InstallArchive(const fs::path& archive, const InstallOptions&, InstallReport report)
    -> std::expected<InstallReport, Fail>;          // failed, or the report so far
[[nodiscard]] static auto Open(fs::path path) -> std::expected<Archive, Fail>;
```

`std::expected` is spelled out in every signature -- there is no `Status` or
`Result` alias, and no helper standing in for `std::unexpected` -- because a
reader meeting `Status` in a parameter list cannot tell it from an enum return,
and `std::expected<void, Fail>` already says "failed or not" without a
dictionary. Sequential work reads as a chain, the way the engine writes it:

```cpp
return CheckToolchainId(id)
    .and_then([&]() -> std::expected<std::string, Fail> {
        if (!PathExists(file)) return std::unexpected(Fail {"no toolchain '" + id + "'"});
        return ReadFile(file);
    })
    .and_then([&](std::string text) { return ParseToolchainJson(text, ToUtf8(file)); });
```

That is why the project is `CMAKE_CXX_STANDARD 23`: `std::expected` is the
error channel, and a manager that runs before any toolchain exists may not
assume a newer standard library than the machine already has. Four
consequences worth knowing when reading the code:

* A constructor cannot report failure, so the types that own a handle have
  named factories instead of throwing constructors: `InputFile::Open`,
  `OutputFile::Create`, `StoreLock::Acquire`, `Archive::Open`,
  `ByteStream::Open`, `CurlHandle::Init`.
* Errors travel outward explicitly: `return std::unexpected(Fail {...})` to
  fail, `if (!next) return std::unexpected(std::move(next.error()))` to pass a
  failure on unchanged, and `return {};` for success. Nothing is swallowed,
  because there is no catch site anywhere to swallow it in.
* Where a step is a loop or a stateful validation pass -- walking tar blocks,
  installing a directory of archives -- the body stays imperative and returns
  early. A chain is for steps that consume the previous value; forcing the
  other shape over a `for` loop would hide the loop, not the plumbing.
* `std::expected` is internal on purpose. It is not a stable ABI and nothing
  links against zpkg, so the boundary that matters -- the store on disk and
  the CLI -- is unchanged by it.

The namespace is `ZHLN`, as in the rest of the repository, and the public
header is still `include/zpkg/zpkg.hpp`: zpkg is a Zahlen tool that happens to
be built separately, not a foreign program bolted on.

## Windows

`cmake -S utils\zpkg -B build-zpkg -G "Visual Studio 17 2022" -A x64` with the
same dependencies (vcpkg provides simdjson/zlib/zstd/openssl/curl), or
`-DZPKG_SIMDJSON_DIR=` pointed at the amalgamated simdjson. A mingw-w64 cross
build works too and is how this was tested:

```sh
cmake -S utils/zpkg -B build-zpkg -G Ninja -DCMAKE_TOOLCHAIN_FILE=<mingw toolchain file> \
      -DZPKG_SIMDJSON_DIR=<amalgamation>
```

The default store is `%LOCALAPPDATA%\zahlen`; `ZHLN_ROOT` overrides it. Four
things behave differently, all of them because Windows has no POSIX file model:

* `shims` is refused. There is no `~/.local/bin` and no symlink to follow;
  `zpkg env --shell powershell` (or `cmd`) emits the export that puts the active
  toolchain on `PATH` instead.
* Archives carrying symbolic links need developer mode or elevation, and zpkg
  says so instead of failing obscurely. Hard links fall back to a copy.
* File modes are not applied. Windows reports the same `rw-rw-rw-` for every
  file and can only express the read-only attribute, so an archive's `0755` is
  ignored -- executability there comes from the extension. The invariant zpkg
  keeps instead is that installed files stay replaceable.
* Paths past `MAX_PATH` use the extended-length form, which only some Win32
  entry points accept, so directory creation walks components with
  `CreateDirectoryW` and file access goes through `_wfopen` rather than
  `std::ofstream` (which would also convert the path to the ANSI code page).

`build-zpkg/` is ignored by git and by `.dockerignore`. That second half is not
tidiness: the image runs this same configure step at `/workspace`, and a
`CMakeCache.txt` from a host build at a different path makes it fail outright.

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

Invariants, each with a test behind it:

* **No privileges.** zpkg refuses to run elevated unless `ZHLN_ROOT` is set
  explicitly, and refuses `/usr`, `/opt`, `/etc`, `%ProgramFiles%` and friends as
  a store root even then.
* **Toolchains never share a directory.** `zpkg use` moves one pointer, so it
  cannot change the runtime of a binary that already exists: that binary
  recorded the store path of the library it linked.
* **Nothing lands outside `usr/`.** Every tar member is validated before a byte
  is written: absolute paths, `..`, backslashes, drive letters, colons, control
  characters, devices, duplicate files, and any symlink that is a prefix of
  another member are all refused. The archive is validated in a first pass and
  extracted in a second, which also detects a file that changed between them.
* **Removal is exact.** Each package's file list is recorded at install time, so
  `remove --package` deletes that package and nothing else.
* **The store is relocatable.** `ACTIVE` holds an id, the POSIX link is relative,
  and no file in the store records an absolute path.

## Commands

| Command | Effect |
|---|---|
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

Install options: `--only a,b`, `--toolchain <id>`, `--require-checksums`,
`-s/--require-signatures`, `--ignore-missing`, `--force`, `--quiet`.

## What a name selects

`install <remote>:<name>` installs `name` **and its closure**, dependencies
first. `<name>` may be a package (`zahlen-gltf`), a component (`GLTF`) or an
exported target (`Zahlen::GLTF`); a name the remote does not publish is an error
that lists what it does publish. Omitting the name installs the whole toolchain,
which is what a first install of a release wants.

```
$ zpkg install upstream:zahlen-gltf
  closure    3 of 16: zahlen-engine zahlen-uischema zahlen-serialization zahlen-gltf
```

`--only a,b` is the same request written the other way, and takes the same
closure. Naming a package and passing `--only` together is refused rather than
merged, because the two spellings would then disagree about what was asked for.

The graph comes from the remote index when it carries one (`index.json` format 2,
which `zpkg index` writes). A format-1 index -- every release published before
this -- names no dependencies, so zpkg fetches the selected archives, reads their
`requires[]` from `usr/share/zahlen/manifests/<package>.json`, and resolves from
those. Either way the archives stay the authority; the index only saves the
download of everything that is not in the closure.

**A missing requirement is refused, not warned about.** An incomplete prefix
installs cleanly and then fails at `find_package()`, which is far from the
command that caused it, so `install` exits non-zero and names the packages that
are absent. `--ignore-missing` keeps the old behaviour for the case where a
second command is about to complete the toolchain.

## Signing

`SHA256SUMS.ed25519` and `index.json.ed25519` are one line per signer:

```
<fingerprint-hex> <base64 ed25519 signature>
```

The signature covers the exact bytes of the file named before the extension. A
key is trusted per store with `zpkg key add <key.pub> --name <who>`, so the same
signature is accepted on a machine where the publisher is trusted and refused
everywhere else. zpkg never signs; a release job does:

```sh
openssl genpkey -algorithm ed25519 -out release-key.pem
openssl pkey -in release-key.pem -pubout -outform DER | tail -c 32 | base64 > key.pub
openssl pkeyutl -sign -inkey release-key.pem -rawin -in SHA256SUMS -out sig.bin
printf '%s ' "$(fingerprint)" && base64 sig.bin
```

`scripts/test-toolchain-install.sh` does exactly this in CI, then installs with
`--require-signatures` and checks that a tampered `SHA256SUMS` is rejected.

## Publishing a toolchain

```sh
zpkg index ./toolchain-output --out ./toolchain-output/index.json
# sign index.json (and SHA256SUMS), then copy the directory anywhere:
zpkg remote add upstream https://packages.example.com/zahlen
zpkg install upstream:upstream -s
```

The index carries the hash and size of every archive, so one signature over the
index covers the whole release. Downloads are content-addressed into
`cache/<sha256>`, so re-installing needs no network.

## Tests

`utils/zpkg/tests/main.cpp` builds with the project and runs under `ctest`. It
covers the SHA-256 and base64 primitives, all three `SHA256SUMS` dialects, strict
JSON refusals, an index round trip, archive extraction and fifteen hostile
archives, the store lifecycle and its lock, checksum/signature policy with real
ed25519 keys, remotes over `file://`, and the path rules.

The suite is also the Windows acceptance test: built with mingw-w64 and run under
Wine it caught five defects that a compile-only check cannot -- `\` vs `/` under
the extended-length prefix, `file://C:\dir` instead of `file:///C:/dir`, POSIX
file modes applied to a filesystem without them, `create_directories`
answering `EINVAL` to a prefixed path, and an archive that stayed open after
being scanned, which Windows reports as a sharing violation when the caller
deletes it. That last one is why the two passes over an archive each open and
close their own stream instead of sharing one.

A Windows build has no OpenSSL in it, so the ed25519 checks are compiled out and
the run reports 204 checks instead of 225.

## Style

The naming here is not zpkg's own taste: it is what `include/Zahlen` and the
sibling tool `utils/zcook` already do -- PascalCase functions and types,
camelCase data, `m_` on private members, no `k` prefix on constants, and
snake_case only where an API being called (libc, libcurl, zlib, the standard
library) spells it that way.

That is written down in `.clang-tidy` because the repository root config turns
on the whole `readability-*` family but never sets
`readability-identifier-naming`, so casing was the one part of the house style
nothing checked -- which is how this tree ended up in snake_case first. To
enforce it:

```sh
cmake -S utils/zpkg -B build/zpkg -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build/zpkg --fix --fix-notes src/*.cpp tests/main.cpp
```

zpkg links nothing from the engine on purpose -- it installs the engine, so it
cannot depend on it. That is also why the error path is its own `Fail` plus an
exit code rather than `ZHLN::Error`, which lives in a library zpkg must not
link: an installer that needed the thing it installs would be a bootstrap
problem. Everything that can be shared without a link is shared: the namespace
(`ZHLN`, `ZHLN::log`), the vocabulary (`log::Say` beside the engine's
`Log(...)`), the helper names (`EndsWith`, `Trim`, `AsciiLower` as `zcook`'s
`EndsWith` and `Lower`), the no-exception discipline the engine and `zcook`
already build with, and the casing rules above.
