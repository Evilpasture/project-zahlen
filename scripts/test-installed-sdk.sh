#!/usr/bin/env bash
# Run INSIDE the built GHCR builder, with a writable /sdk-output bind mount.
# Everything the consumer needs is copied before the checkout is hidden.
set -euo pipefail
cd /workspace
out=${1:-/sdk-output}
mkdir -p "$out" /tmp/zahlen-consumer
# Stage the complete, self-contained consumer project, not engine sources.
cp tests/installed/* /tmp/zahlen-consumer/
profile=release
grep -q '^USE_SANITIZERS:BOOL=ON$' build/CMakeCache.txt && profile=asan-ubsan
prefix=/tmp/zahlen-sdk
components=(Engine CommandLine Signal HostRunner Application UISchema Serialization GLTF Screenshot Netpbm FreeCam Camera CharacterController SkeletalAnimation Animation Data)
for component in "${components[@]}"; do
    cmake --install build --prefix "$prefix" --component "Zahlen$component"
    # Each artifact contains only this component (plus native dependency metadata).
    stage=$(mktemp -d)
    DESTDIR="$stage" cmake --install build --prefix /usr --component "Zahlen$component"
    tar -C "$stage" -czf "$out/zahlen-${component}-${profile}.tar.gz" usr
    rm -rf "$stage"
done
# Use a relocated prefix, not the installation location or the build tree.
mv "$prefix" /tmp/zahlen-sdk-relocated
# Test with only the requested packages and their actual transitive closure.
# No camera, animation, freecam or controller artifacts may be visible.
minimal=/tmp/zahlen-minimal
mkdir -p "$minimal"
for component in Engine CommandLine Signal HostRunner Application Netpbm Data; do
    tar -C "$minimal" -xzf "$out/zahlen-${component}-${profile}.tar.gz"
done
# The installed pack must be actual data, not a dangling build-cache symlink.
test -s "$minimal/usr/share/zahlen/data/base.pak"
test ! -L "$minimal/usr/share/zahlen/data/base.pak"
cd /tmp
mv /workspace /workspace-unavailable
trap 'mv /workspace-unavailable /workspace' EXIT
cmake -S /tmp/zahlen-consumer -B /tmp/consumer-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$minimal/usr" \
    -DZahlen_SANITIZER_PROFILE="$profile"
cmake --build /tmp/consumer-build --verbose
# The option hooks must end up inside the consumer executable. If the SDK fails
# to attach them, libasan's own weak stub wins and the run below reports the
# driver's one-time-init leaks as ours -- hours into CI, at the last step. Check
# it here instead: the definition is 'T' only when the object was linked in.
if [ "$profile" = asan-ubsan ]; then
    for symbol in __lsan_default_options __lsan_default_suppressions __ubsan_default_suppressions; do
        # awk rather than grep -q: this script runs with pipefail, and grep -q
        # closing the pipe early would report success as a failed pipeline.
        if ! nm /tmp/consumer-build/cube | awk -v want="${symbol}" '$2 == "T" && $3 == want { hit = 1 } END { exit(hit ? 0 : 1) }'; then
            echo "FAIL: ${symbol} is not linked into cube (ZahlenEngineConfig skipped the hook object)" >&2
            nm /tmp/consumer-build/cube | grep -F "${symbol}" >&2 || true
            exit 1
        fi
    done
fi
# Verify shared library resolution independently of the engine's build RPATH.
export LD_LIBRARY_PATH="$minimal/usr/lib"
ldd /tmp/consumer-build/cube | tee "$out/consumer-ldd.txt"
! grep -q 'not found' "$out/consumer-ldd.txt"
# Stage only installed loose resources, never the source resource directory.
mkdir -p /tmp/zahlen-game
cp -r "$minimal/usr/share/zahlen/resources" /tmp/zahlen-game/
cd /tmp/zahlen-game
# Force software Vulkan; never silently select a host GPU or skip rendering.
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d
test -r "$VK_DRIVER_FILES"
export ZHLN_DATA_DIR="$minimal/usr/share/zahlen"
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
# Negative test: reject this specific mismatch, not any configure failure.
# Keep expected CMake errors out of the CI console while preserving the log.
wrong=asan-ubsan
[[ "$profile" = asan-ubsan ]] && wrong=release
wrong_build=$(mktemp -d /tmp/zahlen-wrong-profile.XXXXXX)
wrong_log="$out/consumer-wrong-profile.log"
if cmake -S /tmp/zahlen-consumer -B "$wrong_build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$minimal/usr" -DZahlen_SANITIZER_PROFILE="$wrong" \
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
printf 'PASS: installed prefab cube rendered and captured (%s SDK)\n' "$profile"
exit 0
