#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install the pinned toolchain for one .github/gcc16/compilers.json cell.
#
#   install-gcc16.sh --cell 16.2.0-linux [--prefix /opt/zahlen-gcc16/16.2.0-linux]
#
# Every download comes from the manifest, is checked against its sha256 AND its
# byte count, and is extracted into the cell's own prefix. Two cells never share
# a prefix, so an ABI hash cannot accidentally pick up another version's C++
# runtime. The compiler is then asked for its version: a pin that drifted is a
# hard error, not a matrix leg that quietly tests the wrong compiler.
#
# In GitHub Actions the paths are written to GITHUB_ENV / GITHUB_PATH. Outside
# Actions a `$prefix/env.sh` is written that can be sourced instead.

set -Eeuo pipefail
trap 'printf "error: exit %s at line %s\n  command: %s\n" "$?" "${LINENO}" "${BASH_COMMAND}" >&2' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PY="${PYTHON:-python3}"
MATRIX_PY="${SCRIPT_DIR}/gcc16_matrix.py"
MANIFEST="${ZHLN_GCC16_MANIFEST:-${ROOT}/.github/gcc16/compilers.json}"
CELL_ID=""
PREFIX=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --cell)    CELL_ID="$2"; shift 2 ;;
        --prefix)  PREFIX="$2"; shift 2 ;;
        --manifest) MANIFEST="$2"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 64 ;;
    esac
done
[ -n "$CELL_ID" ] || { echo "error: --cell is required" >&2; exit 64; }
[ -n "$PREFIX" ] || PREFIX="${TMPDIR:-/tmp}/zahlen-gcc16/${CELL_ID}"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

matrix() { "$PY" "$MATRIX_PY" --manifest "$MANIFEST" "$@"; }

# ---------------------------------------------------------------- 1. cell --
declare -A C=()
while IFS='=' read -r key value; do
    [ -n "$key" ] || continue
    C["$key"]="$value"
done < <(matrix cell --id "$CELL_ID")

kind="${C[kind]:-}"
gcc_version="${C[gcc]:-}"
reported="${C[reported_version]:-}"
cc_rel="${C[cc]:-}"
cxx_rel="${C[cxx]:-}"
libdir_rel="${C[libdir]:-}"
installer="${C[installer]:-}"
expected_failures=$(matrix probes --id "$CELL_ID")
[ -n "$kind" ] && [ -n "$gcc_version" ] || die "cell $CELL_ID has no kind/gcc"

case "$kind" in
    conda|arch-pkg|winlibs) ;;
    *) die "cell $CELL_ID: unsupported kind '$kind'" ;;
esac

# The planner refuses to select a cell without pins; this second check is for
# someone running the installer by hand against a skipped cell.
n_art=$(matrix artifacts --id "$CELL_ID" | wc -l)
if [ "$kind" != "conda" ] && [ "$n_art" -eq 0 ]; then
    die "cell $CELL_ID has no pinned artifacts (publish them and set available: true)"
fi

mkdir -p "$PREFIX" "$PREFIX/bin" "$PREFIX/.downloads"
PREFIX="$(cd "$PREFIX" && pwd)"
dl="${PREFIX}/.downloads"

# ------------------------------------------------------------- 2. download --
# A conda cell delegates the whole install (download, checksum, wrappers) to the
# existing pinned release installer; gcc16_matrix.py verify cross-checks the two
# pin sets so this manifest cannot describe a different compiler than that
# script installs. Re-downloading here would just double 140 MiB of traffic.
if [ "$kind" = "conda" ]; then
    [ -n "$installer" ] || die "cell $CELL_ID: conda kind needs an `installer`"
    step "gcc $gcc_version via $installer (pins verified by verify)"
    # That installer ends by running a program its own compiler produced. When
    # it is not root it cannot register the runtime with ldconfig, so the probe
    # needs the prefix lib dir on LD_LIBRARY_PATH. The wrappers it writes set
    # this for driver invocations, but not for the programs those drivers emit.
    LD_LIBRARY_PATH="${PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
        bash "${ROOT}/${installer}" "$gcc_version" "$PREFIX"
else
    missing=()
    for tool in curl sha256sum tar; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    case "$kind" in
        arch-pkg)
            command -v zstd >/dev/null 2>&1 || "$PY" -c 'import zstandard' >/dev/null 2>&1 \
                || die "need zstd or python3 -m pip install zstandard"
            ;;
        winlibs)
            command -v unzip >/dev/null 2>&1 || missing+=(unzip)
            ;;
    esac
    [ "${#missing[@]}" -eq 0 ] || die "missing tools: ${missing[*]}"

    step "gcc $gcc_version: $n_art pinned artifact(s) for $CELL_ID"
    while IFS=$'\t' read -r name url sha want_bytes; do
        [ -n "$name" ] || continue
        dest="${dl}/${name}"
        if [ ! -f "$dest" ]; then
            log "downloading $name"
            curl -fsSL --retry 3 -A "zahlen-gcc16-matrix" "$url" -o "$dest"
        fi
        got=$(sha256sum "$dest" | awk '{print $1}')
        [ "$got" = "$sha" ] || die "checksum mismatch for $name (want $sha, got $got)"
        got_bytes=$(wc -c < "$dest" | tr -d ' ')
        [ "$got_bytes" = "$want_bytes" ] || die "size mismatch for $name (want $want_bytes, got $got_bytes)"
        log "ok $name ($got_bytes bytes)"
    done < <(matrix artifacts --id "$CELL_ID")

    case "$kind" in
        arch-pkg)
            for archive in "$dl"/*.pkg.tar.zst; do
                [ -e "$archive" ] || continue
                if command -v zstd >/dev/null 2>&1; then
                    zstd -dc --quiet "$archive" | tar -x -C "$PREFIX" --no-same-owner
                else
                    "$PY" - "$archive" "$PREFIX" <<'PY'
import sys, tarfile, zstandard
src, dest = sys.argv[1], sys.argv[2]
with open(src, 'rb') as fh:
    reader = zstandard.ZstdDecompressor().stream_reader(fh)
    with tarfile.open(fileobj=reader, mode='r|') as tf:
        tf.extractall(dest, filter='data')
PY
                fi
            done
            ;;
        winlibs)
            for archive in "$dl"/*.zip "$dl"/*.7z; do
                [ -e "$archive" ] || continue
                case "$archive" in
                    *.zip) unzip -q -o "$archive" -d "$PREFIX" ;;
                    *.7z)  7z x -y -o"$PREFIX" "$archive" >/dev/null ;;
                esac
            done
            ;;
    esac
fi

# --------------------------------------------------------- 3. host glue ----
# An Arch `gcc` package expects Arch's multiarch layout. Point the driver at the
# host's glibc headers/start files by flag (never by symlink into /usr), and at
# the pinned libgcc/libstdc++ in $PREFIX/usr/lib. This is the same --sysroot=/
# trick scripts/install-gcc-prebuilt.sh uses for the conda toolchain. The pinned
# -L must come BEFORE the host multiarch -L: otherwise -lstdc++ resolves to the
# distro's older runtime while the compiler uses the pinned C++ headers.
detect_multiarch() {
    if command -v dpkg-architecture >/dev/null 2>&1; then
        local m
        m=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
        [ -n "$m" ] && { printf '%s\n' "$m"; return 0; }
    fi
    if command -v dpkg >/dev/null 2>&1; then
        case $(dpkg --print-architecture 2>/dev/null) in
            amd64) printf 'x86_64-linux-gnu\n'; return 0 ;;
            arm64) printf 'aarch64-linux-gnu\n'; return 0 ;;
        esac
    fi
    # Non-dpkg glibc systems that still use a multiarch triplet (or a local
    # toolchain that reports one): only accept it if the directory really
    # holds libc, otherwise fall through to the native /usr/include layout.
    if command -v gcc >/dev/null 2>&1; then
        local triplet
        triplet=$(gcc -dumpmachine 2>/dev/null || true)
        [ -n "$triplet" ] && [ -e "/usr/lib/${triplet}/libc.so.6" ] \
            && { printf '%s\n' "$triplet"; return 0; }
    fi
    local dir
    for dir in /usr/lib/*-linux-gnu*; do
        [ -e "${dir}/libc.so.6" ] || continue
        printf '%s\n' "${dir#/usr/lib/}"; return 0
    done
    return 1
}

if [ "$kind" = "arch-pkg" ]; then
    # Debian/Ubuntu need the multiarch triplet pointed at explicitly. A plain
    # /usr/include + /usr/lib layout (Arch, Fedora, a custom act runner) needs
    # none of those flags: --sysroot=/ already reaches the host's glibc.
    multiarch="${ZHLN_GCC16_MULTIARCH:-$(detect_multiarch || true)}"
    host_flags=""
    if [ -n "$multiarch" ]; then
        host_flags="        -isystem \"/usr/include/${multiarch}\" \\
        -B\"/usr/lib/${multiarch}/\" \\
        -L\"/usr/lib/${multiarch}\" \\
"
        log "host glibc layout: multiarch ${multiarch}"
    else
        log "host glibc layout: native /usr/include + /usr/lib"
    fi
    for pair in "gcc-16:gcc" "g++-16:g++"; do
        short=${pair%%:*}; real=${pair#*:}
        cat > "${PREFIX}/bin/${short}" <<EOF
#!/bin/sh
# GCC ${gcc_version} (Arch 16.1 branch snapshot) aimed at the host's glibc.
prefix=\$(CDPATH= cd -- "\$(dirname -- "\$0")/.." && pwd)
PATH="\${prefix}/usr/bin\${PATH:+:\${PATH}}" \\
LD_LIBRARY_PATH="\${prefix}/usr/lib\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}" \\
    exec "\${prefix}/usr/bin/${real}" --sysroot=/ \\
${host_flags}        -L"\${prefix}/usr/lib" \\
        -Wl,-rpath,"\${prefix}/usr/lib" "\$@"
EOF
        chmod 755 "${PREFIX}/bin/${short}"
    done
    cc_posix="${PREFIX}/bin/gcc-16"
    cxx_posix="${PREFIX}/bin/g++-16"
    lib_path="${PREFIX}/usr/lib"
elif [ "$kind" = "winlibs" ]; then
    cc_posix=$(find "$PREFIX" -type f -path '*/bin/gcc.exe' -print -quit)
    cxx_posix=$(find "$PREFIX" -type f -path '*/bin/g++.exe' -print -quit)
    [ -n "$cc_posix" ] && [ -n "$cxx_posix" ] || die "winlibs archive has no bin/gcc.exe + bin/g++.exe"
    lib_path="$(cd "$(dirname "$cc_posix")" && pwd)"
else
    cc_posix="${PREFIX}/bin/gcc-16"
    cxx_posix="${PREFIX}/bin/g++-16"
    lib_path="${PREFIX}/lib"
fi

# ------------------------------------------------------- 4. version check --
step "verifying the compiler reports $reported"
got_version=$("$cc_posix" -dumpfullversion -dumpversion 2>/dev/null | head -1)
[ -n "$got_version" ] || die "$cc_posix did not report a version"
if [ "$got_version" != "$reported" ]; then
    die "$CELL_ID: compiler reports $got_version, manifest says $reported -- pin drifted"
fi
log "$cc_posix -> $got_version"
log "$cxx_posix -> $("$cxx_posix" -dumpfullversion -dumpversion 2>/dev/null | head -1)"

# CMake on Windows wants a native path; MSYS2 shells can still execute the
# POSIX form for the version check above.
cc="$cc_posix"; cxx="$cxx_posix"
if command -v cygpath >/dev/null 2>&1; then
    cc=$(cygpath -m "$cc_posix")
    cxx=$(cygpath -m "$cxx_posix")
fi

# ------------------------------------------------------------- 5. export ---
# What belongs on PATH differs by cell: the conda toolchain keeps its assembler
# under the target triple, an Arch package keeps cc1/as in usr/bin, and a
# WinLibs archive keeps its DLLs beside gcc.exe (that one really must be on
# PATH on Windows). $lib_path is for LD_LIBRARY_PATH / CMAKE_BUILD_RPATH, not
# for PATH.
case "$kind" in
    conda)    tool_bin="${PREFIX}/x86_64-conda-linux-gnu/bin" ;;
    arch-pkg) tool_bin="${PREFIX}/usr/bin" ;;
    winlibs)  tool_bin="$lib_path" ;;
    *)        tool_bin="${PREFIX}/bin" ;;
esac

cat > "${PREFIX}/env.sh" <<EOF
# Generated by scripts/install-gcc16.sh --cell ${CELL_ID}. Source this file.
export ZHLN_GCC16_PREFIX="${PREFIX}"
export ZHLN_GCC16_VERSION="${got_version}"
export ZHLN_GCC16_KIND="${kind}"
export ZHLN_GCC16_EXPECTED_FAILURES="${expected_failures}"
export CC="${cc}"
export CXX="${cxx}"
export PATH="${PREFIX}/bin:${tool_bin}\${PATH:+:\${PATH}}"
EOF
if [ "$kind" != "winlibs" ]; then
    printf 'export LD_LIBRARY_PATH="%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n' "$lib_path" >> "${PREFIX}/env.sh"
fi

if [ -n "${GITHUB_ENV:-}" ]; then
    {
        printf 'ZHLN_GCC16_PREFIX=%s\n' "$PREFIX"
        printf 'ZHLN_GCC16_VERSION=%s\n' "$got_version"
        printf 'ZHLN_GCC16_KIND=%s\n' "$kind"
        printf 'ZHLN_GCC16_EXPECTED_FAILURES=%s\n' "$expected_failures"
        printf 'ZHLN_GCC16_CC_POSIX=%s\n' "$cc_posix"
        printf 'ZHLN_GCC16_CXX_POSIX=%s\n' "$cxx_posix"
        printf 'CC=%s\n' "$cc"
        printf 'CXX=%s\n' "$cxx"
    } >> "$GITHUB_ENV"
    # GITHUB_PATH is consumed by Actions, not by this MSYS2 shell: on Windows it
    # needs native paths. Without this, the compiler's own DLLs (libwinpthread,
    # libstdc++-6, libgcc_s_seh) are not found when a probe binary is started.
    path_a="${PREFIX}/bin"; path_b="$tool_bin"
    if command -v cygpath >/dev/null 2>&1; then
        path_a=$(cygpath -w "$path_a")
        path_b=$(cygpath -w "$path_b")
    fi
    printf '%s\n%s\n' "$path_a" "$path_b" >> "${GITHUB_PATH:-/dev/null}"
    log "wrote CC/CXX to GITHUB_ENV"
else
    log "no GITHUB_ENV: source ${PREFIX}/env.sh to use this compiler"
fi

step "ready: $CELL_ID ($kind, $got_version) at $PREFIX"
