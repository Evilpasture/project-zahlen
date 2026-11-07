# GCC 16.x compiler matrix

`.github/workflows/gcc16-matrix.yml` is a compiler-compatibility lane. Every GCC
16.x version gets its own job on Linux and (where a matching toolchain exists)
on MSYS2 UCRT64. The matrix is **data**, not YAML: `compilers.json` is the only
place a version, runner or download pin is named.

```
python3 scripts/gcc16_matrix.py verify          # validate pins before spending runners
python3 scripts/gcc16_matrix.py list            # show active / skipped cells
python3 scripts/gcc16_matrix.py plan --event push
```

## What runs when

| Event | Mode | Work per cell |
| --- | --- | --- |
| `push`, `pull_request` | `gate` | install the pinned compiler, assert its exact version, compile + link + run four probes (`scripts/check-gcc16-toolchain.sh`) |
| `schedule` (weekly Mon 04:23 UTC), `workflow_dispatch` with `mode=full` | `full` | the gate, then `cmake` configure + full build; `ztest` on Linux, build-only on UCRT64 |

`mode` comes from the manifest's `full_build.events` plus the dispatch input, so
changing which events do full builds does not touch the workflow. A cell can set
`full_build: false` when a known capability gap would stop CMake at configure
time; `full_build_reason` must then say why.

The four probes are: C++26 `-freflection` / `std::meta`, Jolt's incomplete
static-member pattern, a 600-entry constant table that trips `.base64` on old
assemblers, and a `std::format` link-and-run check for the C++ runtime. Probes 2
and 3 are the same constructs `cmake/ToolchainCapabilities.cmake` checks at
configure time. The full build runs `./build/bin/ztest --test-dir build -V`, not
`ctest`: this tree replaces CTest with `ztest` (`cmake/ZTest.cmake`), and a
`ctest` call here would report "no tests found" and pass vacuously.

Clang is intentionally absent: it takes the transpiler fallback for reflection
and answers a different question.

## Cells

| Cell | State | Toolchain | Full build |
| --- | --- | --- | --- |
| `16.1.1-linux` | active | Arch archive `gcc` + `libgcc` + `libstdc++` from the **same** `16.1.1+r12+g301eb08fa2c5-1` snapshot | no (Jolt gap) |
| `16.1.1-ucrt64` | **skipped** | no UCRT64 toolchain reports 16.1.1 | — |
| `16.2.0-linux` | active | the six conda-forge `.conda` artifacts already pinned by `scripts/install-gcc-prebuilt.sh` | yes |
| `16.2.0-ucrt64` | active | WinLibs GCC 16.2.0 UCRT64 (posix/SEH, mingw-w64 14.0.0 r3) zip | yes, build-only |
| `16.3.0-linux` | **skipped** | GCC 16.3 is not released yet | — |
| `16.3.0-ucrt64` | **skipped** | GCC 16.3 is not released yet | — |

Skipped cells are filtered out by `plan`, so they produce no jobs at all rather
than red Xs. `workflow_dispatch` → `all_cells=true` plans them anyway with
`continue-on-error`, which is useful for seeing exactly why one is not ready.

### Measured capability gap on 16.1.1

Both active Linux cells were installed from their real pins and probed end to
end. GCC 16.2.0 passes all four. The pinned 16.1.1 **fails the Jolt probe** with
`-freflection`:

```
error: 'StaticArray<T, N>::<unnamed union>::mElements' has incomplete type
```

This is not a script bug and not specific to these flags: it also fails with
`-freflection -O0` and with the extra `-fparameter-reflection` /
`-freflection-latest` combinations CMake may add, while the same source passes
with `-std=c++26` alone. It is the exact construct
`cmake/ToolchainCapabilities.cmake` makes `FATAL_ERROR` at configure time, so a
full engine build on 16.1.1 cannot start. Released 16.2.0 defers the
instantiation as that probe requires.

The manifest records this as `expected_probe_failures: ["jolt"]` on
`16.1.1-linux`. `scripts/check-gcc16-toolchain.sh` then:

- runs **every** probe (a known gap does not skip the rest),
- emits a `::warning::` annotation for the documented gap instead of failing,
- fails hard on any **new** probe failure,
- warns if a documented gap unexpectedly starts passing, so the manifest does
  not go stale.

Note this is evidence about the compiler, not a change to the tree's declared
`ZHLN_GCC_MIN=16.1.1`: the floor gate stays as it is. It means the 16.1.1 row
is a reflection/assembler/runtime compatibility lane, not a full-build lane.

### Why 16.1.1 is unusual

There is no `gcc-16.1.1.tar.xz`. GCC 16.1.0 shipped 2026-04-30 and 16.2.0 on
2026-08-07; `16.1.1` is the version the 16.1 release branch carried in between.
Fedora 44 GA and Arch packaged it as a dated snapshot, and that is where the
tree's `ZHLN_GCC_MIN=16.1.1` comes from. The Arch archive keeps that exact
snapshot as immutable `.pkg.tar.zst` files, so it can be pinned by sha256.

The three packages need glibc >= 2.38, so `ubuntu-24.04` (2.39) is the oldest
usable runner; `ubuntu-22.04` (2.35) is not. `scripts/install-gcc16.sh` builds
`gcc-16`/`g++-16` wrappers that point the driver at the host's glibc headers
and start files (`--sysroot=/`, `-isystem`, `-B`, `-L`) and at the pinned
libgcc/libstdc++ in the prefix, exactly as `install-gcc-prebuilt.sh` does for
the conda toolchain. Nothing is symlinked into `/usr`. On a plain
`/usr/include` + `/usr/lib` host the multiarch flags are omitted automatically.

### Why there is no 16.1.1 UCRT64 cell

WinLibs publishes 16.1.0 and 16.2.0 UCRT builds; MSYS2's UCRT64 archive has
`mingw-w64-ucrt-x86_64-gcc` 16.1.0-1..7 and 16.2.0-3..6. Neither has a 16.1.1
build. Substituting 16.1.0 would silently test a different compiler than the
row claims, so the cell stays skipped.

If a 16.1 *series* UCRT64 lane is wanted instead, this pin is verified and
ready to paste into a new `16.1.0-ucrt64` cell (set `reported_version: "16.1.0"`):

```
url:    https://github.com/brechtsanders/winlibs_mingw/releases/download/16.1.0posix-14.0.0-ucrt-r4/winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64ucrt-14.0.0-r4.7z
sha256: 08b46777f127b92f4ca9ca057b317331178133d028513d1c3632f9cdcfde9ae7
bytes:  109677594
```

## Adding GCC 16.3.0

1. Wait for the release. Then, for the Linux cell, add the six `16.3.0`
   conda-forge `.conda` artifacts (name, URL, sha256, byte count) and add that
   version's pin set to `scripts/install-gcc-prebuilt.sh` — its `ARTIFACTS`
   block is the authority for conda cells, and `verify` refuses a manifest that
   disagrees with it.
2. For the UCRT64 cell, pin the WinLibs or MSYS2 16.3.0 UCRT archive and its
   sidecar sha256.
3. Flip `available` to `true` on both cells. No workflow edit, no new job id.

`verify` enforces that an available cell has at least one pinned artifact, that
every sha256 is 64 lowercase hex characters, that URLs are https, that byte
counts are present, and that a cell's `gcc` label equals the version its
compiler actually reports. A cell with no pins cannot be enabled by accident.

## Running a cell by hand

```bash
python3 scripts/gcc16_matrix.py verify
bash scripts/install-gcc16.sh --cell 16.1.1-linux --prefix /tmp/gcc16/16.1.1
. /tmp/gcc16/16.1.1/env.sh
bash scripts/check-gcc16-toolchain.sh --cc "$CC" --cxx "$CXX"
```

Outside Actions the installer writes `$prefix/env.sh` instead of `GITHUB_ENV`.
Both cells install into their own prefix: sharing one would let an ABI hash pick
up another version's C++ runtime.

## `act`

Verified with nektos `act v0.2.97` and rootful Podman locally:

| Command | Result |
| --- | --- |
| `act -l`, `act -g`, `act -W <workflow> -n` | parse and graph OK; the `plan` job dry-runs |
| `act -W <workflow> -j plan --pull=false` | real run: manifest verify, planner, `::set-output:: cells/mode/summary`, report |
| `act -W <fixture> -j gcc16 --pull=false` | real run of one Linux cell (both 16.1.1 and 16.2.0 verified) |

Two `act` limits, neither a problem for real Actions:

1. **Dynamic matrix.** `act` parses `matrix.include` before running any job, so
   `${{ fromJSON(needs.plan.outputs.cells) }}` gives `Invalid JSON: unexpected
   end of JSON input`. Real Actions expands it. For a local run, generate a
   static single-cell copy (do not commit it):

   ```bash
   python3 scripts/gcc16_act_fixture.py --cell 16.1.1-linux -o /tmp/act.yml --no-apt
   act -W /tmp/act.yml -j gcc16 --pull=false
   ```

   `--no-apt` skips the two apt-get steps for runner images that already have
   `curl`, `zstd`, `unzip` and `python3`. Drop it when using a real Ubuntu
   runner image with network access.
2. **No Windows runner.** `act` only schedules Linux containers, so UCRT64 legs
   can be listed and graph-checked but not executed locally.

Useful local flags: `DOCKER_HOST=unix:///path/to/podman.sock`, `--pull=false`
(otherwise Podman tries to resolve a `localhost/...` image as a registry), and
`-P ubuntu-24.04=<image>` to pick the runner image. The fixtures above were run
with `localhost/zahlen-ci-base:verify` (Arch) and
`localhost/zahlen-release-builder:verify` (Ubuntu 22.04).
