#!/usr/bin/env bash
# Run INSIDE the built builder image, with a writable /pkg-output bind mount.
#
# This is the client-side proof: a plain unprivileged user installs engine
# packages with zpkg and builds/links an app against them, without root,
# without LD_LIBRARY_PATH, and without the engine checkout on disk. The
# container starts as root only to hand the checkout to the test user and then
# re-executes this script as that user; every step below runs unprivileged.
set -euo pipefail

out=${1:-/pkg-output}

if [ "$(id -u)" -eq 0 ]; then
    user=zahlen
    id -u "$user" >/dev/null 2>&1 || useradd -m -s /bin/bash "$user"
    mkdir -p "$out"
    # The builder wrote these as root; ownership is the only thing that changes.
    # Nothing below this block runs with any privilege at all.
    chown -R "$user" /workspace "$out"
    command -v setpriv >/dev/null || {
        echo 'FAIL: setpriv is required to drop privileges for this test' >&2
        exit 1
    }
    exec setpriv --reuid="$(id -u "$user")" --regid="$(id -g "$user")" --init-groups \
        env HOME="/home/${user}" bash "$0" "$@"
fi

cd /workspace
mkdir -p "$out" /tmp/zahlen-consumer
# Generate the consumer smoke fixture. It is a TEST, not an example: no
# checked-in sample is compiled by CI, no release ships a consumer tarball,
# and a renamed example can never break this job again.
cat >/tmp/zahlen-consumer/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(ZahlenConsumerSmoke LANGUAGES CXX)
find_package(ZahlenApplication CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Zahlen::Application)
EOF
cat >/tmp/zahlen-consumer/main.cpp <<'EOF'
#include <Zahlen/App.hpp>

int main(int argc, char* argv[]) {
    return ZHLN::App().Main(argc, argv);
}
EOF

profile=release
if grep -q '^USE_SANITIZERS:BOOL=ON$' build/CMakeCache.txt; then
    echo 'FAIL: sanitizer-enabled builds cannot be packaged as release toolchains' >&2
    exit 1
fi

# The toolchain id is derived by cmake/Toolchains.cmake from the same inputs as
# the ABI hash. Read it back instead of reconstructing it here: if the two ever
# disagree, the artifacts would be mislabelled and this test must notice.
id=$(sed -n 's/.*"id":"\([^"]*\)".*/\1/p' build/toolchain/toolchain.json)
test -n "$id"
printf 'toolchain: %s (%s)\n' "$id" "$profile"

# zpkg is built by the image (utils/zpkg is a standalone CMake project, and the
# image ran its test suite before this script did anything). Copy the binary
# somewhere the store move below cannot reach.
mkdir -p /tmp/zpkg-bin
cp /workspace/build-zpkg/zpkg /tmp/zpkg-bin/zpkg
chmod 755 /tmp/zpkg-bin/zpkg
zpkg=/tmp/zpkg-bin/zpkg
"$zpkg" --version

# The toolchain ships libraries, headers and the cooker -- never content. This
# script therefore packages no data pack: the consumer below cooks its own with
# the zcook from the store, which is the whole point of the boundary.
components=(Engine CommandLine Image Signal Screenshot HostRunner Application Netpbm UI
    UISchema Scene Serialization GLTF Fonts Lua Widgets Benchmark BenchmarkViewer Scripting
    Console Editor Terrain VFX FreeCam Camera CharacterController AnimationControls Animation Cooker)
for component in "${components[@]}"; do
    # Each artifact contains only this component plus its metadata. Only the
    # DESTDIR staging tree is what gets packaged: the /usr spelling is what the
    # packages mean, not where zpkg will put them.
    stage=$(mktemp -d)
    DESTDIR="$stage" cmake --install build --prefix /usr --component "Zahlen$component"
    tar -C "$stage" -czf "$out/zahlen-${component,,}-${id}.tar.gz" usr
    rm -rf "$stage"
done
(cd "$out" && sha256sum zahlen-*.tar.gz >SHA256SUMS)

# A release is signed once, by the publisher. The private key never leaves this
# container; the public key and the signatures are published with the artifacts,
# so a user can install with --require-signatures and get the same answer.
openssl genpkey -algorithm ed25519 -out /tmp/release-key.pem 2>/dev/null
openssl pkey -in /tmp/release-key.pem -pubout -outform DER 2>/dev/null >/tmp/release-key.der
tail -c 32 /tmp/release-key.der | base64 -w0 > "$out/zahlen-release-key.pub"
printf '\n' >>"$out/zahlen-release-key.pub"

sign_file() {
    # sign_file <file>: writes <file>.ed25519 as "<fingerprint> <base64 signature>"
    openssl pkeyutl -sign -inkey /tmp/release-key.pem -rawin -in "$1" -out "$1.sig" 2>/dev/null
    { printf '%s ' "$fingerprint"; base64 -w0 "$1.sig"; printf '\n'; } >"$1.ed25519"
    rm -f "$1.sig"
}

printf 'packaged %d components\n' "${#components[@]}"

# --- install as a user, into the default per-user store --------------------

# No ZHLN_ROOT on purpose: the default is ${XDG_DATA_HOME:-~/.local/share}/zahlen,
# which is exactly what a developer gets with no configuration at all.
unset ZHLN_ROOT ZHLN_BIN_DIR
if [ "$(id -u)" -eq 0 ]; then
    echo 'FAIL: zpkg is being exercised as root; this test must run unprivileged' >&2
    exit 1
fi

# Only the components this consumer needs. Everything else must stay invisible.
# Unprivileged by construction: nothing below uses sudo.
# This is a local test key for the fixture packages below, not the project
# release key: publishing uses ZHLN_RELEASE_KEY_PEM and the pinned public key.
"$zpkg" key add "$out/zahlen-release-key.pub" --name "zahlen CI" >/dev/null
"$zpkg" key list
# Derive the ephemeral CI key fingerprint directly rather than taking the first
# line of `zpkg key list`, where the built-in official key is listed first.
fingerprint=$(tail -c 32 /tmp/release-key.der | sha256sum | cut -c1-32)
test -n "$fingerprint"
sign_file "$out/SHA256SUMS"
"$zpkg" index "$out" --out "$out/index.json"
sign_file "$out/index.json"

"$zpkg" install "$out" \
    --only Engine,CommandLine,Image,Signal,Screenshot,HostRunner,Application,Netpbm,Cooker \
    --require-checksums --require-signatures

prefix=$("$zpkg" prefix)
printf 'prefix: %s\n' "$prefix"
case $prefix in
    "$HOME"/*) ;;
    *)
        echo "FAIL: zpkg installed outside the user's home: ${prefix}" >&2
        exit 1
        ;;
esac

"$zpkg" list | tee "$out/zpkg-list.txt"
"$zpkg" info "$id" | tee "$out/zpkg-info.txt"

for component in Engine CommandLine Image Signal Screenshot HostRunner Application Netpbm Cooker; do
    test -f "$prefix/share/zahlen/manifests/zahlen-${component,,}.json"
done
# Single-.so boundary assertions: no libJolt, no Jolt headers, no prebuilt .a archives,
# and zero leaked internal, third-party, stdlib, or allocator symbols in .dynsym.
if [ -e "$prefix/include/Jolt" ] || compgen -G "$prefix/lib*/libJolt*" >/dev/null || compgen -G "$prefix/lib*/*.a" >/dev/null; then
    echo 'FAIL: installed prefix leaked Jolt or a prebuilt static archive' >&2
    exit 1
fi
engine_so_matches=("$prefix"/lib*/libzahlen_engine.so)
if [ "${#engine_so_matches[@]}" -ne 1 ] || [ ! -f "${engine_so_matches[0]}" ]; then
    echo "FAIL: expected exactly one installed libzahlen_engine.so under ${prefix}" >&2
    exit 1
fi
python3 /workspace/configure/check_engine_exports.py --dso "${engine_so_matches[0]}"
for component in Engine; do
    test -f "$prefix/share/zahlen/manifests/zahlen-${component,,}.json"
done
for component in Camera Animation FreeCam CharacterController GLTF Serialization UI UISchema Fonts Lua Widgets Benchmark BenchmarkViewer Scripting Console Editor Terrain VFX; do
    if [ -e "$prefix/share/zahlen/manifests/zahlen-${component,,}.json" ]; then
        echo "FAIL: ${component} was installed despite --only" >&2
        exit 1
    fi
done
for leaf in Camera Animation FreeCam CharacterController glTF json toml UI Fonts lua Widget Benchmark Scripting Console editor Terrain VFX; do
    if [ -e "$prefix/include/${leaf}" ]; then
        echo "FAIL: headers for excluded component (${leaf}) are visible" >&2
        exit 1
    fi
done

# The cooker is the toolchain's answer to "where do assets come from", so it
# has to resolve through the store and run from there: an rpath that only works
# in the build tree would make the package a lie.
cooker=$("$zpkg" which zcook)
test -x "$cooker"
tools="$prefix/share/zahlen/tools"
test -f "$tools/export_metadata.py"

# Probe the loader, not the program. zcook has no --help: argv[1] is an action
# and an unknown one exits 1 on purpose, so asking it to print usage would fail
# this test for the wrong reason. The question that matters is whether the
# dynamic linker resolves everything the binary needs from inside the store.
# awk rather than grep -q: this script runs with pipefail, and grep -q closing
# the pipe early turns ldd's SIGPIPE into a false pass.
unresolved_libraries() {
    ldd "$1" 2>&1 | awk '/not found/'
}
if [ -n "$(unresolved_libraries "$cooker")" ]; then
    echo 'FAIL: the installed zcook cannot resolve its libraries from the store' >&2
    ldd "$cooker" >&2
    exit 1
fi
printf 'PASS: installed cooker resolves from the store (%s)\n' "$cooker"

# Installed COM runtime plugin (.so + .manifest.toml) verification: Screenshot
# was requested in --only, so both its plugin library and manifest must be
# present under lib*/zahlen/plugins/ and resolve libzahlen_engine.so via
# $ORIGIN/../..; excluded components' plugins must remain absent.
screenshot_plugin_matches=("$prefix"/lib*/zahlen/plugins/libzahlen_plugin_screenshot.so)
if [ "${#screenshot_plugin_matches[@]}" -ne 1 ] || [ ! -f "${screenshot_plugin_matches[0]}" ]; then
    echo "FAIL: expected installed libzahlen_plugin_screenshot.so under ${prefix}/lib*/zahlen/plugins" >&2
    exit 1
fi
plugin_dir=$(dirname "${screenshot_plugin_matches[0]}")
test -f "${plugin_dir}/zahlen_plugin_screenshot.manifest.toml"
for excluded_plugin in freecam character_controller animation terrain vfx gltf; do
    if [ -e "${plugin_dir}/libzahlen_plugin_${excluded_plugin}.so" ] || [ -e "${plugin_dir}/zahlen_plugin_${excluded_plugin}.manifest.toml" ]; then
        echo "FAIL: excluded runtime plugin '${excluded_plugin}' was installed despite --only" >&2
        exit 1
    fi
done
if [ -n "$(unresolved_libraries "${screenshot_plugin_matches[0]}")" ]; then
    echo 'FAIL: installed libzahlen_plugin_screenshot.so cannot resolve libzahlen_engine.so' >&2
    ldd "${screenshot_plugin_matches[0]}" >&2
    exit 1
fi
printf 'PASS: installed COM plugin and manifest resolve from the store (%s)\n' "${screenshot_plugin_matches[0]}"

# The store is relocatable: `active` is a relative link and the rpaths zpkg
# leaves in installed binaries are $ORIGIN-relative, so moving the whole
# directory must break neither. zpkg's default root still names the old path,
# which no longer exists -- a relocated store is reached with ZHLN_ROOT, which is
# the only thing a user could do either.
default_root=$HOME/.local/share/zahlen
moved_root=$default_root-moved
mv "$default_root" "$moved_root"
moved_prefix=$(ZHLN_ROOT="$moved_root" "$zpkg" prefix)
case $moved_prefix in
    "$moved_root"/*) ;;
    *)
        echo "FAIL: prefix after the move is ${moved_prefix}, expected it under ${moved_root}" >&2
        exit 1
        ;;
esac
# The marker has to still resolve, and it has to still be relative: an absolute
# one would point back at the directory that no longer exists.
if [ ! -e "$moved_root/active" ]; then
    echo 'FAIL: the active marker no longer resolves after the store moved' >&2
    exit 1
fi
if [ "$(readlink "$moved_root/active")" != "store/${id}" ]; then
    echo "FAIL: active points at $(readlink "$moved_root/active"), expected store/${id}" >&2
    exit 1
fi
# A relative rpath is what makes this work: the store moved, the binary did not
# change, and zcook still resolves libzahlen_engine.so inside the new location.
moved_cooker=$(ZHLN_ROOT="$moved_root" "$zpkg" which zcook)
test -x "$moved_cooker"
if [ -n "$(unresolved_libraries "$moved_cooker")" ]; then
    echo 'FAIL: the cooker broke when the store moved; its rpath is absolute' >&2
    ldd "$moved_cooker" >&2
    exit 1
fi
moved_plugin_matches=("$moved_prefix"/lib*/zahlen/plugins/libzahlen_plugin_screenshot.so)
if [ "${#moved_plugin_matches[@]}" -ne 1 ] || [ -n "$(unresolved_libraries "${moved_plugin_matches[0]}")" ]; then
    echo 'FAIL: installed COM plugin broke when the store moved; its rpath is absolute' >&2
    ldd "${moved_plugin_matches[0]}" >&2
    exit 1
fi
# Nothing zpkg wrote may record where the store used to be: a store that leaked an
# absolute path works until the next move, and then fails somewhere else.
# -I keeps debug info out of it; only text zpkg wrote is interesting here.
if grep -rlF -I -- "$default_root/" "$moved_root/store" >/dev/null 2>&1; then
    echo 'FAIL: the store recorded the absolute path it was installed into' >&2
    grep -rlF -I -- "$default_root/" "$moved_root/store" >&2
    exit 1
fi
mv "$moved_root" "$default_root"
printf 'PASS: store relocates without rewriting anything\n'

# --- the manager refuses what it must refuse ------------------------------

# A tampered checksum must be fatal. If this passes, --require-checksums is a lie.
tamper=$(mktemp -d)
# Any archive will do; naming one component would tie this to whether the Data
# package was packaged. No pipe: `ls | head` under pipefail can abort the script
# with SIGPIPE, which is a hostile way to fail a test.
victim=
for archive in "$out"/zahlen-*.tar.gz; do
    victim=$(basename "$archive")
    break
done
test -n "$victim"
cp "$out/$victim" "$tamper/"
( cd "$tamper" && printf '%064d  %s\n' 0 "$victim" >SHA256SUMS )
if "$zpkg" install "$tamper" --require-checksums >/dev/null 2>&1; then
    echo 'FAIL: zpkg accepted an archive whose checksum does not match' >&2
    exit 1
fi
rm -rf "$tamper"
printf 'PASS: checksum mismatch rejected\n'

# A signature that no longer covers the checksums must be fatal. If this passes,
# --require-signatures is decorative.
signed=$(mktemp -d)
cp "$out/$victim" "$out/SHA256SUMS" "$out/SHA256SUMS.ed25519" "$signed/"
printf '# tampered after signing\n' >>"$signed/SHA256SUMS"
if "$zpkg" install "$signed" --require-checksums --require-signatures >/dev/null 2>&1; then
    echo 'FAIL: zpkg accepted checksums that the signature does not cover' >&2
    exit 1
fi
rm -rf "$signed"
printf 'PASS: signature that does not cover the checksums is rejected\n'

# An archive that would write outside usr/ must never be extracted, no matter
# what it is called. zpkg cannot rely on the caller having built it.
escape=$(mktemp -d)
mkdir -p "$escape/stage"
printf x >"$escape/stage/payload"
tar -C "$escape/stage" -czf "$escape/zahlen-evil-${id}.tar.gz" payload
if "$zpkg" install "$escape/zahlen-evil-${id}.tar.gz" >/dev/null 2>&1; then
    echo 'FAIL: zpkg extracted an archive that is not confined to usr/' >&2
    exit 1
fi
rm -rf "$escape"
printf 'PASS: archive outside usr/ rejected\n'

# --- build and run the consumer against the store -------------------------
# --- build and link a consumer against the store --------------------------
#
# No rendered frame, no cooked pack and no device creation here: those belong
# to the engine's own test suite (ci.yml "Test (release)") on a driver that
# meets the Vulkan floor. This job's claim is narrower and still valuable:
# find_package() resolves from the store, the imported targets link, the
# loader resolves everything without LD_LIBRARY_PATH, and the CLI runs.

test -s "$prefix/share/zahlen/ZahlenToolchain.cmake" || {
    echo 'FAIL: ZahlenToolchain.cmake was not installed with the Engine package' >&2
    exit 1
}
cmake -S /tmp/zahlen-consumer -B /tmp/consumer-build -G Ninja \
    --toolchain="$prefix/share/zahlen/ZahlenToolchain.cmake"
cmake --build /tmp/consumer-build --verbose

# Nothing may reach back into the engine checkout: the store is the only source.
for generated in /tmp/consumer-build/build.ninja /tmp/consumer-build/CMakeCache.txt; do
    if grep -Fq '/workspace' "$generated"; then
        echo "FAIL: ${generated} still references the engine checkout" >&2
        grep -Fn '/workspace' "$generated" >&2 || true
        exit 1
    fi
done
printf 'PASS: consumer build references the store only\n'

# Shared-library resolution must not depend on the environment. Every package
# is installed with an $ORIGIN rpath and LD_LIBRARY_PATH stays unset here.
env -u LD_LIBRARY_PATH ldd /tmp/consumer-build/consumer | tee "$out/consumer-ldd.txt"
if grep -q 'not found' "$out/consumer-ldd.txt"; then
    echo 'FAIL: the consumer needs LD_LIBRARY_PATH, so the packages are not self-contained' >&2
    exit 1
fi
printf 'PASS: no LD_LIBRARY_PATH required\n'

# --help is handled before host/device creation (AppExitReason::CliHelpRequested),
# so the CLI and the loader are exercised without needing a Vulkan driver.
if ! env -u LD_LIBRARY_PATH /tmp/consumer-build/consumer --help \
        >"$out/consumer-help.txt" 2>&1; then
    echo 'FAIL: consumer --help did not exit cleanly' >&2
    cat "$out/consumer-help.txt" >&2
    exit 1
fi
test -s "$out/consumer-help.txt" || {
    echo 'FAIL: consumer --help printed nothing' >&2
    exit 1
}
printf 'PASS: consumer CLI runs without creating a device\n'

printf 'PASS: installed consumer links and loads (%s toolchain, unprivileged)\n' "$profile"
exit 0
