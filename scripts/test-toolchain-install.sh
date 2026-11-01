#!/usr/bin/env bash
# Run INSIDE the built builder image, with a writable /pkg-output bind mount.
#
# This is the client-side proof: a plain unprivileged user installs engine
# packages with zpkg, builds an app against them and runs it, without root,
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
# Stage the complete, self-contained consumer project, not engine sources.
cp tests/installed/* /tmp/zahlen-consumer/

profile=release
grep -q '^USE_SANITIZERS:BOOL=ON$' build/CMakeCache.txt && profile=asan-ubsan

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
components=(Engine CommandLine Signal HostRunner Application UISchema Serialization GLTF
    Screenshot Netpbm FreeCam Camera CharacterController SkeletalAnimation Animation Cooker)
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
# Trust the throwaway release key, then publish and sign the index that
# describes the artifacts.
"$zpkg" key add "$out/zahlen-release-key.pub" --name "zahlen CI" >/dev/null
"$zpkg" key list
fingerprint=$("$zpkg" key list | awk '{print $1; exit}')
sign_file "$out/SHA256SUMS"
"$zpkg" index "$out" --out "$out/index.json"
sign_file "$out/index.json"

"$zpkg" install "$out" \
    --only Engine,CommandLine,Signal,HostRunner,Application,Netpbm,Cooker \
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

for component in Engine CommandLine Signal HostRunner Application Netpbm Cooker; do
    test -f "$prefix/share/zahlen/manifests/zahlen-${component,,}.json"
done
for component in Camera Animation FreeCam CharacterController GLTF Serialization UISchema Screenshot; do
    if [ -e "$prefix/share/zahlen/manifests/zahlen-${component,,}.json" ]; then
        echo "FAIL: ${component} was installed despite --only" >&2
        exit 1
    fi
done
for leaf in Camera Animation FreeCam CharacterController glTF json toml UI Screenshot; do
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

cmake -S /tmp/zahlen-consumer -B /tmp/consumer-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" \
    -DZahlen_SANITIZER_PROFILE="$profile"
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

# The option hooks must end up inside the consumer executable. If the package
# fails to attach them, libasan's own weak stub wins and the run below reports
# the driver's one-time-init leaks as ours -- hours into CI, at the last step.
# Check it here instead: the definition is 'T' only when the object was linked.
if [ "$profile" = asan-ubsan ]; then
    for symbol in __lsan_default_options __lsan_default_suppressions __ubsan_default_suppressions; do
        # awk rather than grep -q: this script runs with pipefail, and grep -q
        # closing the pipe early would report success as a failed pipeline.
        if ! nm /tmp/consumer-build/cube |
            awk -v want="${symbol}" '$2 == "T" && $3 == want { hit = 1 } END { exit(hit ? 0 : 1) }'; then
            echo "FAIL: ${symbol} is not linked into cube (ZahlenEngineConfig skipped the hook object)" >&2
            nm /tmp/consumer-build/cube | grep -F "${symbol}" >&2 || true
            exit 1
        fi
    done
fi

# Shared-library resolution must not depend on the environment. The linked
# binaries record the store path of the .so they linked, every package is
# installed with an $ORIGIN rpath, and LD_LIBRARY_PATH stays unset here on
# purpose -- a required LD_LIBRARY_PATH would be a bug in the packages.
env -u LD_LIBRARY_PATH ldd /tmp/consumer-build/cube | tee "$out/consumer-ldd.txt"
if grep -q 'not found' "$out/consumer-ldd.txt"; then
    echo 'FAIL: the consumer needs LD_LIBRARY_PATH, so the packages are not self-contained' >&2
    exit 1
fi
printf 'PASS: no LD_LIBRARY_PATH required\n'

# The consumer owns its content: it cooks a pack with the toolchain's cooker from
# an asset root it owns, and never from the engine checkout. The font pair below
# is copied out because this script runs inside the checkout; a real project has
# its own assets in that place.
assets=/tmp/zahlen-consumer-assets
rm -rf "$assets"
mkdir -p "$assets/resources/assets/fonts"
cp /workspace/resources/fonts/JetBrainsMonoNerdFontRegular/*.fnt \
   /workspace/resources/fonts/JetBrainsMonoNerdFontRegular/*.png \
   "$assets/resources/assets/fonts/"
"$cooker" ninja --out "$assets/assets.ninja" --source "$assets" \
    --engine-tools "$tools" --self "$cooker" --no-blender
ninja -C "$assets" -f assets.ninja
test -s "$assets/data/base.pak"
printf 'PASS: consumer cooked its own pack with the installed zcook (%s bytes)\n' \
    "$(stat -c %s "$assets/data/base.pak")"

mkdir -p /tmp/zahlen-game/data
cp "$assets/data/base.pak" /tmp/zahlen-game/data/base.pak
cd /tmp/zahlen-game
# Force software Vulkan; never silently select a host GPU or skip rendering.
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d
test -r "$VK_DRIVER_FILES"
# No ZHLN_DATA_DIR: the engine finds data/base.pak in the working directory,
# which is where the cook above put it. Pointing it at the store would be
# pointing at a directory that no longer carries data.
if [ "$profile" = asan-ubsan ]; then
    # No suppressions= path on purpose: the rules are compiled into the hook
    # object, and only the reporting verbosity is set here. When a driver stops
    # leaking the count drops to zero, which is good news, so this is a report
    # rather than a failure -- but it must never say "nothing fired" because the
    # rules were never read.
    export LSAN_OPTIONS=print_suppressions=1
fi
timeout 180s /tmp/consumer-build/cube "$out/cube.ppm" \
    2>&1 | tee "$out/consumer-render.log"
if [ "$profile" = asan-ubsan ]; then
    if grep -q 'Suppressions used' "$out/consumer-render.log"; then
        grep -A3 'Suppressions used' "$out/consumer-render.log"
    else
        printf 'NOTE: no LeakSanitizer rule matched; the driver may simply have stopped leaking\n'
    fi
fi

# The pack the consumer cooked is the one the engine has to mount. Without this
# the cook above could produce a well-formed archive that nothing reads, and the
# run would still pass on the embedded defaults alone.
if ! grep -Fq 'Mounted asset pack' "$out/consumer-render.log"; then
    echo 'FAIL: the engine never mounted the pack the consumer cooked' >&2
    tail -20 "$out/consumer-render.log" >&2
    exit 1
fi
printf 'PASS: engine mounted the consumer-cooked pack\n'

# Negative test: reject this specific mismatch, not any configure failure.
# Keep expected CMake errors out of the CI console while preserving the log.
wrong=asan-ubsan
[[ "$profile" = asan-ubsan ]] && wrong=release
wrong_build=$(mktemp -d /tmp/zahlen-wrong-profile.XXXXXX)
wrong_log="$out/consumer-wrong-profile.log"
if cmake -S /tmp/zahlen-consumer -B "$wrong_build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$prefix" -DZahlen_SANITIZER_PROFILE="$wrong" \
    >"$wrong_log" 2>&1; then
    echo 'FAIL: mismatched sanitizer profile was accepted' >&2
    cat "$wrong_log" >&2
    exit 1
fi
if ! grep -Fq 'Zahlen profile mismatch:' "$wrong_log"; then
    echo 'FAIL: wrong-profile configure failed for an unexpected reason' >&2
    cat "$wrong_log" >&2
    exit 1
fi
rm -rf "$wrong_build"
printf 'PASS: incompatible sanitizer profile rejected (details: %s)\n' "$wrong_log"
printf 'PASS: installed prefab cube rendered and captured (%s toolchain, unprivileged)\n' "$profile"
exit 0
