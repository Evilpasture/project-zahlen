#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# One-shot capture of a single Khronos fidelity scenario through Zahlen.
# This is the verbose sibling of scripts/run_fidelity.sh, useful for dialling
# in a scenario or a BRDF by hand:
#
#   ./scripts/capture_fidelity.sh khronos-AlphaBlendModeTest
#   ./scripts/capture_fidelity.sh khronos-NormalTangentTest build/out.ppm
#
# The Khronos repos land under build/fidelity/ on first use (git-ignored,
# outside workspace snapshots). Override with ZHLN_FIDELITY_REPO and
# ZHLN_SAMPLE_ASSETS to use trees you already have. Scenario resolution comes
# from `fidelity list` (tools/fidelity), not an inline Python merge.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
cd "$ROOT"

SCENARIO_NAME="${1:?usage: capture_fidelity.sh <scenario-name-or-substring> [output.ppm]}"
OUT_PPM="${2:-build/fidelity_output/_last.ppm}"

mkdir -p build/fidelity_output build/fidelity

if [[ -n "${ZHLN_FIDELITY_REPO:-}" && -f "$ZHLN_FIDELITY_REPO/test/config.json" ]]; then
    FIDELITY="$ZHLN_FIDELITY_REPO"
elif [[ -f build/fidelity/glTF-Render-Fidelity-Generator/test/config.json ]]; then
    FIDELITY="build/fidelity/glTF-Render-Fidelity-Generator"
else
    echo "[setup] cloning KhronosGroup/glTF-Render-Fidelity-Generator"
    git clone --depth 1 https://github.com/KhronosGroup/glTF-Render-Fidelity-Generator.git build/fidelity/glTF-Render-Fidelity-Generator
    FIDELITY="build/fidelity/glTF-Render-Fidelity-Generator"
fi

if [[ -n "${ZHLN_SAMPLE_ASSETS:-}" && -d "$ZHLN_SAMPLE_ASSETS/Models" ]]; then
    SAMPLES="$ZHLN_SAMPLE_ASSETS"
elif [[ -d "$FIDELITY/glTF-Sample-Assets/Models" ]]; then
    SAMPLES="$FIDELITY/glTF-Sample-Assets"
elif [[ -d build/fidelity/glTF-Sample-Assets/Models ]]; then
    SAMPLES="build/fidelity/glTF-Sample-Assets"
else
    echo "[setup] cloning KhronosGroup/glTF-Sample-Assets"
    git clone --depth 1 https://github.com/KhronosGroup/glTF-Sample-Assets.git build/fidelity/glTF-Sample-Assets
    SAMPLES="build/fidelity/glTF-Sample-Assets"
fi

# The tool binary: resolution (list) + compare reuse the same C++ surface.
TOOL_BIN="${ZHLN_FIDELITY_TOOL:-}"
if [[ -z "$TOOL_BIN" || ! -x "$TOOL_BIN" ]]; then
    shopt -s nullglob
    for cand in build/*/tools/fidelity/fidelity build/tools/fidelity/fidelity tools/fidelity/fidelity; do
        [[ -f "$cand" && -x "$cand" ]] && { TOOL_BIN="$cand"; break; }
    done
    shopt -u nullglob
fi
if [[ -z "$TOOL_BIN" || ! -x "$TOOL_BIN" ]]; then
    echo "fidelity tool not found; build it or pass ZHLN_FIDELITY_TOOL." >&2
    exit 1
fi

OUT_DIR="build/fidelity_output"
mkdir -p "$OUT_DIR"

"$TOOL_BIN" list \
    --config "$FIDELITY/test/config.json" \
    --fidelity "$FIDELITY" \
    --samples "$SAMPLES" \
    --out-dir "$OUT_DIR" >/dev/null

# The list TSV already merged the defaults and resolved model/lighting paths.
# Pick the first scenario whose name contains the argument.
MATCH=""
while IFS=$'\t' read -r name _rest; do
    if [[ "$name" == *"$SCENARIO_NAME"* ]]; then
        MATCH="$name"
        break
    fi
done < "$OUT_DIR/scenarios.tsv"

if [[ -z "$MATCH" ]]; then
    echo "no scenario matches '$SCENARIO_NAME'" >&2
    exit 1
fi
echo "[*] scenario: $MATCH"

BIN="${ZHLN_FIDELITY_BIN:-}"
if [[ -z "$BIN" ]]; then
    # CMake presets set binaryDir to build/<preset>, so samples land in
    # build/<preset>/samples/; fall back to the flat build/samples/ layout.
    for cand in build/*/samples/FidelityHarness build/samples/FidelityHarness; do
        [[ -f "$cand" && -x "$cand" ]] && { BIN="$cand"; break; }
    done
fi
if [[ -z "$BIN" || ! -x "$BIN" ]]; then
    echo "FidelityHarness binary not found; build it first (samples target)." >&2
    exit 1
fi

echo "[*] rendering -> $OUT_PPM"
exec "$BIN" --headless "--scenario=$OUT_DIR/$MATCH.json" "--output=$OUT_PPM" --ambient-scale="${FIDELITY_AMBIENT_SCALE:-1.0}"
