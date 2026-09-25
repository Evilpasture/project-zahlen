#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# run_fidelity.sh -- orchestrate the Zahlen glTF-Render-Fidelity suite the Unix
# way: one C++ tool (tools/fidelity) + one Bash driver + Ninja for scheduling.
#
#   ./scripts/run_fidelity.sh                 # resolve, list, render, compare, report
#   ./scripts/run_fidelity.sh --list          # resolve + list scenarios, then exit
#   ./scripts/run_fidelity.sh --limit 8       # stop after 8 scenarios
#   ./scripts/run_fidelity.sh -j8             # 8 scenarios in parallel
#   SCENARIO=khronos-ToyCar ./scripts/run_fidelity.sh
#
# Ninja gives the caching for free: a scenario re-renders only when its model,
# scenario JSON, the harness binary or this driver changed (mtime); everything
# else is skipped. ZHLN_CACHE_DIR points at a shared cache so parallel
# FidelityHarness processes do not race on build/cache/pipeline_cache.bin.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
cd "$ROOT"

# ---------------------------------------------------------------------------
# knobs (env overrides first, then flags)
# ---------------------------------------------------------------------------
FIDELITY="${ZHLN_FIDELITY_REPO:-}"
SAMPLES="${ZHLN_SAMPLE_ASSETS:-}"
ENGINE_BIN="${ZHLN_FIDELITY_BIN:-}"
TOOL_BIN="${ZHLN_FIDELITY_TOOL:-}"
OUT_DIR="${FIDELITY_OUT_DIR:-build/fidelity_output}"
SCRATCH="build/fidelity"
FIDELITY_REPO_URL="https://github.com/KhronosGroup/glTF-Render-Fidelity-Generator.git"
SAMPLES_REPO_URL="https://github.com/KhronosGroup/glTF-Sample-Assets.git"

JOBS="-j$(nproc)"
LIMIT=0
SCENARIO_FILTER="${SCENARIO:-}"
KEEP_PPM=0
DO_LIST=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --list)       DO_LIST=1 ;;
        --keep-ppm)   KEEP_PPM=1 ;;
        --limit)      LIMIT="${2:?--limit needs a number}"; shift ;;
        --fidelity)   FIDELITY="$2"; shift ;;
        --samples)    SAMPLES="$2"; shift ;;
        --engine)     ENGINE_BIN="$2"; shift ;;
        --output)     OUT_DIR="$2"; shift ;;
        --scenario)   SCENARIO_FILTER="$2"; shift ;;
        -j*|--jobs*)  JOBS="$1"; [[ "$1" == --jobs ]] && { JOBS="-j$2"; shift; } ;;
        -h|--help)    sed -n '3,17p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

mkdir -p "$OUT_DIR" "$SCRATCH"

# ---------------------------------------------------------------------------
# repositories (submodule-first, env override, then clone)
# ---------------------------------------------------------------------------
if [[ -z "$FIDELITY" || ! -f "$FIDELITY/test/config.json" ]]; then
    if [[ -f "$SCRATCH/glTF-Render-Fidelity-Generator/test/config.json" ]]; then
        FIDELITY="$SCRATCH/glTF-Render-Fidelity-Generator"
    else
        echo "[setup] Cloning $FIDELITY_REPO_URL -> $SCRATCH/glTF-Render-Fidelity-Generator"
        git clone --depth 1 "$FIDELITY_REPO_URL" "$SCRATCH/glTF-Render-Fidelity-Generator"
        FIDELITY="$SCRATCH/glTF-Render-Fidelity-Generator"
    fi
else
    # tolerate a relative override
    FIDELITY="$(cd "$FIDELITY" && pwd)"
fi

if [[ -z "$SAMPLES" || ! -d "$SAMPLES/Models" ]]; then
    if [[ -d "$FIDELITY/glTF-Sample-Assets/Models" ]]; then
        SAMPLES="$FIDELITY/glTF-Sample-Assets"
    elif git -C "$FIDELITY" submodule status glTF-Sample-Assets &>/dev/null; then
        echo "[setup] Initializing the generator's pinned glTF-Sample-Assets submodule"
        if git -C "$FIDELITY" submodule update --init --depth 1 glTF-Sample-Assets 2>/dev/null \
           && [[ -d "$FIDELITY/glTF-Sample-Assets/Models" ]]; then
            SAMPLES="$FIDELITY/glTF-Sample-Assets"
        fi
    fi
    if [[ -z "$SAMPLES" || ! -d "$SAMPLES/Models" ]]; then
        echo "[setup] Cloning $SAMPLES_REPO_URL -> $SCRATCH/glTF-Sample-Assets"
        git clone --depth 1 "$SAMPLES_REPO_URL" "$SCRATCH/glTF-Sample-Assets"
        SAMPLES="$SCRATCH/glTF-Sample-Assets"
    fi
fi

# ---------------------------------------------------------------------------
# binaries
# ---------------------------------------------------------------------------
if [[ -z "$ENGINE_BIN" ]]; then
    shopt -s nullglob
    for cand in build/*/samples/FidelityHarness build/samples/FidelityHarness; do
        [[ -f "$cand" && -x "$cand" ]] && { ENGINE_BIN="$cand"; break; }
    done
    shopt -u nullglob
fi
[[ -z "$ENGINE_BIN" || ! -x "$ENGINE_BIN" ]] && {
    echo "FidelityHarness binary not found; build the samples target or pass --engine." >&2
    exit 1
}

if [[ -z "$TOOL_BIN" ]]; then
    shopt -s nullglob
    for cand in build/*/tools/fidelity/fidelity build/tools/fidelity/fidelity tools/fidelity/fidelity; do
        [[ -f "$cand" && -x "$cand" ]] && { TOOL_BIN="$cand"; break; }
    done
    shopt -u nullglob
fi
[[ -z "$TOOL_BIN" || ! -x "$TOOL_BIN" ]] && {
    echo "fidelity tool not found; build it or pass ZHLN_FIDELITY_TOOL." >&2
    exit 1
}

# ---------------------------------------------------------------------------
# list
# ---------------------------------------------------------------------------
"$TOOL_BIN" list \
    --config "$FIDELITY/test/config.json" \
    --fidelity "$FIDELITY" \
    --samples "$SAMPLES" \
    --out-dir "$OUT_DIR" >/dev/null

if [[ "$DO_LIST" -eq 1 ]]; then
    cat "$OUT_DIR/scenarios.tsv"
    exit 0
fi

# ---------------------------------------------------------------------------
# the Ninja graph
# ---------------------------------------------------------------------------
NINJA_FILE="$OUT_DIR/fidelity.ninja"
SHARED_CACHE="$SCRATCH/cache"
mkdir -p "$SHARED_CACHE"

# Parse the TSV with `read -a` (tab-split). The `|| {...; break}` arm handles a
# final line without a trailing newline: bash returns non-zero from `read` at
# EOF only when that last line lacked a \n, in which case the row must still be
# emitted. `read` consuming $IFS tabs canonicalises the column count, so
# leading/trailing blank or comment lines skip cleanly. (Plain `read -r a b`
# would drop the last row, silently omitting the final scenario.)
NAMES=()
MODELS=()
while IFS=$'\t' read -r -a row || { [[ -n "${row[*]//[[:space:]]/}" ]] && { name="${row[0]:-}"; [[ -n "$name" && "$name" != \#* ]] && NAMES+=("$name") && MODELS+=("${row[1]:-}"); }; break; }; do
    name="${row[0]:-}"
    model="${row[1]:-}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    [[ -n "$SCENARIO_FILTER" && "$name" != *"$SCENARIO_FILTER"* ]] && continue
    [[ "$LIMIT" -gt 0 && "${#NAMES[@]}" -ge "$LIMIT" ]] && break
    NAMES+=("$name")
    MODELS+=("$model")
done < "$OUT_DIR/scenarios.tsv"
[[ "${#NAMES[@]}" -eq 0 ]] && { echo "[fidelity] no scenarios matched" >&2; exit 1; }

{
    echo "# generated by scripts/run_fidelity.sh -- do not edit"
    echo
    echo "rule render"
    echo "  command = env ZHLN_CACHE_DIR=$SHARED_CACHE $ENGINE_BIN --headless --scenario=$OUT_DIR/\$scenario.json --output=$OUT_DIR/\$scenario.ppm"
    echo "  description = render \$scenario"
    echo "rule compare"
    echo "  command = $TOOL_BIN compare --candidate $OUT_DIR/\$scenario.ppm --goldens-dir $FIDELITY/test/goldens --name \$scenario --out-dir $OUT_DIR"
    echo "  description = compare \$scenario"
    echo

    # One render+compare pair per scenario. A Ninja build statement's indented
    # `key = value` lines attach to the INPUTS of that very build statement, so
    # `scenario` is defined per edge (the rule command references it expanded
    # at execution time). Ninja only merges repeated build lines for PHONY
    # outputs, so the golden PNGs must ride on the compare build line itself as
    # extra inputs: a refreshed golden then re-runs compare even when the
    # render is still fresh. Same for the tool binary (metric/code changes) and
    # the model (asset changes) on their respective edges.
    for i in "${!NAMES[@]}"; do
        name="${NAMES[$i]}"
        model="${MODELS[$i]}"
        goldens=""
        for renderer in filament blender-cycles gltf-sample-viewer model-viewer babylon; do
            g="$FIDELITY/test/goldens/$name/$renderer-golden.png"
            [[ -f "$g" ]] && goldens="$goldens $g"
        done

        echo "build $OUT_DIR/$name.ppm: render $OUT_DIR/$name.json $ENGINE_BIN ${BASH_SOURCE[0]} $model"
        echo "  scenario = $name"
        echo "build $OUT_DIR/$name.db: compare $OUT_DIR/$name.ppm$goldens $TOOL_BIN ${BASH_SOURCE[0]}"
        echo "  scenario = $name"
        echo
    done

    # `default` statements accumulate, so one per scenario is the explicit
    # build-all list (bare `ninja` builds every .db).
    for name in "${NAMES[@]}"; do
        echo "default $OUT_DIR/$name.db"
    done
} > "$NINJA_FILE"

# ---------------------------------------------------------------------------
# render + compare (parallel, mtime-cached), then the report
# ---------------------------------------------------------------------------
echo "[fidelity] ${#NAMES[@]} scenarios -> ninja $JOBS"
# Ninja paths in the graph are repo-root-relative, so run from the root the
# graph was written for. -k0 = keep going: one failed scenario does not stop
# the suite; its .db simply stays missing and the report shows the gap.
ninja $JOBS -k0 -f "$NINJA_FILE" \
    || echo "[fidelity] some scenarios did not finish; see the report for gaps." >&2
"$TOOL_BIN" report --out-dir "$OUT_DIR" >/dev/null
echo "[fidelity] report -> $OUT_DIR/report.md ($OUT_DIR/report.html)"
