#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# One-shot capture of a single Khronos fidelity scenario through Zahlen.
# This is the verbose sibling of scripts/run_fidelity.py, useful for dialling
# in a scenario or a BRDF by hand:
#
#   ./scripts/capture_fidelity.sh khronos-AlphaBlendModeTest
#   ./scripts/capture_fidelity.sh khronos-NormalTangentTest build/out.ppm
#
# The Khronos repos land under build/fidelity/ on first use (git-ignored,
# outside workspace snapshots). Override with ZHLN_FIDELITY_REPO and
# ZHLN_SAMPLE_ASSETS to use trees you already have.

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

# Resolve the first scenario whose name contains the argument.
MATCH="$(python3 - "$FIDELITY" "$SCENARIO_NAME" <<'PY'
import json, sys
from pathlib import Path

fidelity = Path(sys.argv[1])
want = sys.argv[2].lower()
config = json.loads((fidelity / "test" / "config.json").read_text())
for raw in config.get("scenarios", []):
    if want in raw["name"].lower():
        print(raw["name"])
        sys.exit(0)
print("__none__")
PY
)"

if [[ "$MATCH" == "__none__" || -z "$MATCH" ]]; then
    echo "no scenario matches '$SCENARIO_NAME'" >&2
    exit 1
fi
echo "[*] scenario: $MATCH"

# Generate a single-scenario file the harness can read directly.
python3 - "$FIDELITY" "$SAMPLES" "$MATCH" "build/fidelity_output/$MATCH.json" <<'PY'
import json, sys
from pathlib import Path

fidelity, samples, name, out = sys.argv[1:5]
config = json.loads((Path(fidelity) / "test" / "config.json").read_text())
defaults = {
    "lighting": "../../../environments/lightroom_14b.hdr",
    "dimensions": {"width": 768, "height": 768},
    "target": {"x": 0, "y": 0, "z": 0},
    "orbit": {"theta": 0, "phi": 90, "radius": 1},
    "verticalFoV": 45,
    "renderSkybox": False,
}
for raw in config.get("scenarios", []):
    if raw["name"] != name:
        continue
    merged = dict(defaults)
    merged.update({k: v for k, v in raw.items() if k not in ("dimensions", "target", "orbit")})
    merged["dimensions"] = {**defaults["dimensions"], **raw.get("dimensions", {})}
    merged["target"] = {**defaults["target"], **raw.get("target", {})}
    merged["orbit"] = {**defaults["orbit"], **raw.get("orbit", {})}
    rel = merged["model"]
    direct = (Path(fidelity) / "test" / rel).resolve()
    if not direct.exists() and "glTF-Sample-Assets/" in rel:
        direct = (Path(samples) / rel.split("glTF-Sample-Assets/", 1)[1]).resolve()
    merged["model"] = str(direct)
    Path(out).write_text(json.dumps(merged, indent=2))
    sys.exit(0)
PY

BIN="${ZHLN_FIDELITY_BIN:-build/samples/FidelityHarness}"
if [[ ! -x "$BIN" ]]; then
    echo "FidelityHarness binary not found at $BIN; build it first (samples target)." >&2
    exit 1
fi

echo "[*] rendering -> $OUT_PPM"
exec "$BIN" --headless "--scenario=build/fidelity_output/$MATCH.json" "--output=$OUT_PPM" --ambient-scale="${FIDELITY_AMBIENT_SCALE:-1.0}"
