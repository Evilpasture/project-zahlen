#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

"""Drive Zahlen's FidelityHarness against the Khronos glTF-Render-Fidelity suite.

The Khronos generator (KhronosGroup/glTF-Render-Fidelity-Generator) defines test
cases as scenario objects merged over a set of defaults, renders each with a
list of reference renderers, and scores a candidate against the reference
"goldens" with the pixelmatch YIQ metric expressed as an RMS distance ratio in
dB (fail above -22 dB).

This script reproduces that contract for Zahlen:

  1. Locate (or clone) the two Khronos repositories. By default they land in
     `build/fidelity/` (git-ignored and outside workspace snapshots); the
     `--fidelity` / `--samples` flags or the `ZHLN_FIDELITY_REPO` /
     `ZHLN_SAMPLE_ASSETS` env vars point anywhere else. The fidelity repo's own
     pinned `glTF-Sample-Assets` submodule is preferred when present.
  2. Read `test/config.json`, merge each scenario over the generator's defaults,
     and resolve every model / lighting path against the on-disk layout.
  3. Run `FidelityHarness --headless --scenario ... --output ...` per scenario.
  4. Convert the PPM to PNG (pure-Python decoder; ImageMagick used only if the
     PPM decoder ever fails), diff against a chosen reference golden, and
     compute the same `rmsDistanceRatio` (dB) the generator reports.
  5. Emit `report.md` / `report.html` and per-scenario PNG + diff PNG.

Dependencies: Python 3.8+ only (stdlib). `git` for the optional clones.

Usage:
  python3 scripts/run_fidelity.py --help
  python3 scripts/run_fidelity.py
  python3 scripts/run_fidelity.py --scenario NormalTangentTest MetalRoughSpheres
  python3 scripts/run_fidelity.py --engine ./build/samples/FidelityHarness --renderer filament blender-cycles
"""

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Default scenario values — the exact Object.assign defaults from the
# generator's src/config-reader.ts, so a partial scenario resolves identically.
# ---------------------------------------------------------------------------

DEFAULT_SCENARIO = {
    "lighting": "../../../environments/lightroom_14b.hdr",
    "dimensions": {"width": 768, "height": 768},
    "target": {"x": 0, "y": 0, "z": 0},
    "orbit": {"theta": 0, "phi": 90, "radius": 1},
    "verticalFoV": 45,
    "renderSkybox": False,
}

# Maximum possible value of the YIQ colour-difference metric (pixelmatch).
MAX_COLOR_DISTANCE = 35215.0

# The generator's pass/fail threshold (src/common.ts), in dB.
FIDELITY_TEST_THRESHOLD = -22.0

# Reference golden preference order per scenario.
GOLDEN_RENDERERS = [
    "filament",
    "blender-cycles",
    "gltf-sample-viewer",
    "model-viewer",
    "babylon",
]

FIDELITY_REPO_URL = "https://github.com/KhronosGroup/glTF-Render-Fidelity-Generator.git"
SAMPLES_REPO_URL = "https://github.com/KhronosGroup/glTF-Sample-Assets.git"


def clone(url: str, dest: Path, shallow: bool = True) -> None:
    args = ["git", "clone", url, str(dest)]
    if shallow:
        args = ["git", "clone", "--depth", "1", url, str(dest)]
    subprocess.run(args, check=True)


def _env_path(name: str):
    """os.getenv -> Path, treating an unset/empty var as 'not provided'."""
    value = os.environ.get(name, "")
    return Path(value) if value.strip() else None


# Auto-clone scratch directory. `build/` is both git-ignored and excluded from
# the workspace snapshot, so the multi-GB Sample-Assets tree never lands in a
# commit or a turn artifact. `--fidelity` / `--samples` / the two env vars
# override to anywhere (e.g. the task's `external/` layout).
def scratch_dir(repo_root: Path) -> Path:
    return repo_root / "build" / "fidelity"


def find_fidelity_repo(specified: Path, repo_root: Path) -> Path:
    """Return the fidelity repo, cloning it on demand into the scratch dir."""
    candidates = [specified] if specified else []
    if not specified:
        candidates = [_env_path("ZHLN_FIDELITY_REPO"), scratch_dir(repo_root) / "glTF-Render-Fidelity-Generator"]
    for cand in candidates:
        if cand is None or str(cand) in (".", ""):
            continue
        if (cand / "test" / "config.json").exists():
            return cand.resolve()

    dest = scratch_dir(repo_root) / "glTF-Render-Fidelity-Generator"
    print(f"[setup] Cloning {FIDELITY_REPO_URL} -> {dest}")
    clone(FIDELITY_REPO_URL, dest)
    return dest.resolve()


def _init_submodule(fidelity_repo: Path) -> Path:
    """Initialize the generator's own pinned glTF-Sample-Assets submodule.

    The generator records the exact Sample-Assets commit its goldens were made
    against; using that tree is the most faithful reproduction. Falls back to a
    stand-alone shallow clone when the submodule has been vendored differently.
    """
    sub = fidelity_repo / "glTF-Sample-Assets"
    if (sub / "Models").exists():
        return sub.resolve()
    if (sub / ".git").exists() or (fidelity_repo / ".gitmodules").exists():
        print(f"[setup] Initializing the generator's pinned glTF-Sample-Assets submodule -> {sub}")
        run = subprocess.run(
            ["git", "-C", str(fidelity_repo), "submodule", "update", "--init", "--depth", "1", "glTF-Sample-Assets"],
            capture_output=True, text=True,
        )
        if run.returncode == 0 and (sub / "Models").exists():
            return sub.resolve()
        print(f"[setup] submodule init failed ({run.stderr.strip()[:200]}); cloning stand-alone")
    return None


def find_samples_repo(specified: Path, fidelity_repo: Path, repo_root: Path) -> Path:
    """Return glTF-Sample-Assets, cloning it on demand into the scratch dir."""
    env_repo = _env_path("ZHLN_SAMPLE_ASSETS")
    candidates = [specified] if specified else []
    if not specified:
        candidates = [env_repo, fidelity_repo / "glTF-Sample-Assets", scratch_dir(repo_root) / "glTF-Sample-Assets"]
    for cand in candidates:
        if cand is None or str(cand) in (".", ""):
            continue
        if (cand / "Models").exists():
            return cand.resolve()

    pinned = _init_submodule(fidelity_repo)
    if pinned is not None:
        return pinned

    dest = env_repo or (scratch_dir(repo_root) / "glTF-Sample-Assets")
    print(f"[setup] Cloning {SAMPLES_REPO_URL} -> {dest}")
    clone(SAMPLES_REPO_URL, dest)
    return dest.resolve()


def merge_scenario(raw: dict) -> dict:
    """The generator's Object.assign merge (deep for dimensions/target/orbit)."""
    merged = dict(DEFAULT_SCENARIO)
    merged.update({k: v for k, v in raw.items() if k != "dimensions" and k != "target" and k != "orbit"})
    merged["dimensions"] = {**DEFAULT_SCENARIO["dimensions"], **raw.get("dimensions", {})}
    merged["target"] = {**DEFAULT_SCENARIO["target"], **raw.get("target", {})}
    merged["orbit"] = {**DEFAULT_SCENARIO["orbit"], **raw.get("orbit", {})}
    return merged


def resolve_model(rel: str, fidelity_repo: Path, samples_dir: Path) -> Path:
    """Resolve a scenario's model path (author relative to the repo's test/)."""
    # Priority 1: exactly as authored, relative to test/ (the in-repo layout).
    direct = (fidelity_repo / "test" / rel).resolve()
    if direct.exists():
        return direct
    # Priority 2: the same asset inside the sample-assets clone.
    marker = "glTF-Sample-Assets/"
    if marker in rel:
        tail = rel.split(marker, 1)[1]
        alt = (samples_dir / tail).resolve()
        if alt.exists():
            return alt
    return direct  # return the canonical path even if absent; the harness reports


def find_engine_binary(repo_root: Path, specified: Path):
    """Locate the FidelityHarness binary, or None.

    Preference order: the explicit --engine path, then ZHLN_FIDELITY_BIN, then
    the CMake preset layout (build/<preset>/samples/FidelityHarness — the
    presets in CMakePresets.json set binaryDir to build/<preset>), then the
    flat build/samples/ layouts some users create by hand.
    """
    if specified is not None:
        return specified.resolve()
    env_bin = os.environ.get("ZHLN_FIDELITY_BIN", "")
    if env_bin.strip():
        return Path(env_bin).resolve()
    candidates = sorted(repo_root.glob("build/*/samples/FidelityHarness"))
    candidates += [
        repo_root / "build" / "samples" / "FidelityHarness",
        repo_root / "build" / "samples" / "RelWithDebInfo" / "FidelityHarness",
    ]
    for cand in candidates:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand.resolve()
    return None


# ---------------------------------------------------------------------------
# Minimal PNG codec so the runner needs nothing but the stdlib. ImageMagick is
# only attempted as a fallback if the PPM decode ever fails.
# ---------------------------------------------------------------------------

def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            len(payload).to_bytes(4, "big")
            + tag
            + payload
            + zlib.crc32(tag + payload).to_bytes(4, "big")
        )

    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter type 0 per scanline
        raw.extend(rgb[y * stride : (y + 1) * stride])

    ihdr = (
        width.to_bytes(4, "big")
        + height.to_bytes(4, "big")
        + bytes([8, 2, 0, 0, 0])  # bit depth 8, colour type 2 (truecolour)
    )
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def read_ppm(path: Path):
    """Return (width, height, rgb-bytes) for a P6 PPM."""
    with open(path, "rb") as f:
        data = f.read()

    def token(pos):
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] != b"\n":
                pos += 1
            return token(pos)
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        return data[start:pos], pos

    magic, pos = token(0)
    if magic != b"P6":
        raise ValueError(f"Not a binary P6 PPM: {magic!r}")
    w, pos = token(pos)
    h, pos = token(pos)
    maxval, pos = token(pos)
    pos += 1  # single whitespace after maxval
    width, height = int(w), int(h)
    if int(maxval) != 255:
        raise ValueError(f"Unsupported PPM maxval {maxval!r}")
    return width, height, data[pos : pos + width * height * 3]


# ---------------------------------------------------------------------------
# pixelmatch YIQ metric — a faithful port of the generator's
# src/third_party/pixelmatch/color-delta.ts and ImageComparator.analyze().
# ---------------------------------------------------------------------------

def _blend(c, a):
    return 255 + (c - 255) * a


def _rgb2y(r, g, b):
    return r * 0.29889531 + g * 0.58662247 + b * 0.11448223


def _rgb2i(r, g, b):
    return r * 0.59597799 - g * 0.27417610 - b * 0.32180189


def _rgb2q(r, g, b):
    return r * 0.21147017 - g * 0.52261711 + b * 0.31114694


def color_delta(buf1, buf2, k, m):
    a1, a2 = buf1[k + 3] / 255.0, buf2[m + 3] / 255.0
    r1, g1, b1 = _blend(buf1[k], a1), _blend(buf1[k + 1], a1), _blend(buf1[k + 2], a1)
    r2, g2, b2 = _blend(buf2[m], a2), _blend(buf2[m + 1], a2), _blend(buf2[m + 2], a2)
    y = _rgb2y(r1, g1, b1) - _rgb2y(r2, g2, b2)
    i = _rgb2i(r1, g1, b1) - _rgb2i(r2, g2, b2)
    q = _rgb2q(r1, g1, b1) - _rgb2q(r2, g2, b2)
    return 0.5053 * y * y + 0.299 * i * i + 0.1957 * q * q


def analyze(candidate_rgba, golden_rgba, width, height):
    """Return the generator's rmsDistanceRatio and per-pixel delta grid."""
    assert len(candidate_rgba) == width * height * 4
    assert len(golden_rgba) == width * height * 4
    square_sum = 0.0
    model_pixels = 0
    deltas = []
    for i in range(width * height):
        pos = i * 4
        alpha = candidate_rgba[pos + 3] / 255.0
        if alpha == 0:
            deltas.append(0.0)
            continue
        delta = color_delta(candidate_rgba, golden_rgba, pos, pos)
        deltas.append(delta)
        square_sum += delta * delta
        model_pixels += 1
    if model_pixels == 0:
        return 1.0, deltas  # no comparable pixels -> worst
    return (square_sum / model_pixels) ** 0.5 / MAX_COLOR_DISTANCE, deltas


def to_decibel(value: float) -> float:
    import math

    if value <= 0.0:
        return -float("inf")
    return 10.0 * math.log10(value)


def write_diff_png(path: Path, candidate_rgba, golden_rgba, deltas, width, height):
    """Red = candidate brighter, blue = golden brighter, scaled by delta."""
    rgb = bytearray()
    for i in range(width * height):
        pos = i * 4
        d = deltas[i]
        # Per-pixel heat intensity: 0 (identical) -> 255 (max colour distance).
        mag = int(round(255.0 * min(d / MAX_COLOR_DISTANCE, 1.0)))
        r, g, b = candidate_rgba[pos], candidate_rgba[pos + 1], candidate_rgba[pos + 2]
        gr, gg, gb = golden_rgba[pos], golden_rgba[pos + 1], golden_rgba[pos + 2]
        br = sum((r, g, b)) / 3.0
        bg = sum((gr, gg, gb)) / 3.0
        if br >= bg:
            rgb.extend((255, mag, mag))
        else:
            rgb.extend((mag, mag, 255))
    write_png(path, width, height, bytes(rgb))


# ---------------------------------------------------------------------------
# PNG reading (dependency-free via a minimal inflate + unfilter over truecolour
# 8-bit PNGs, which is what goldens are). ImageMagick used only as a fallback.
# ---------------------------------------------------------------------------

def read_png(path: Path):
    try:
        return _read_png_stdlib(path)
    except Exception:
        magick = shutil.which("magick") or shutil.which("convert")
        if not magick:
            raise
        print(f"[conversion] stdlib PNG read failed for {path.name}; using ImageMagick")
        out = path.with_suffix(".tmp.rgba")
        subprocess.run([magick, path, "-depth", "8", "rgba:" + str(out)], check=True)
        data = out.read_bytes()
        out.unlink(missing_ok=True)
        # From raw rgba we don't know width/height without the PNG header parse;
        # ask ImageMagick for the geometry to be safe.
        ident = subprocess.run([magick, "identify", "-format", "%w %h", str(path)], capture_output=True, text=True)
        w, h = map(int, ident.stdout.split())
        return w, h, data


def _read_png_stdlib(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos : pos + 4], "big")
        tag = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width = int.from_bytes(payload[0:4], "big")
            height = int.from_bytes(payload[4:8], "big")
            bit_depth = payload[8]
            color_type = payload[9]
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(f"unsupported PNG ({bit_depth=}, {color_type=})")
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p : p + stride])
        p += stride
        if ft == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        out[y * stride : (y + 1) * stride] = line
        prev = line
    if channels == 3:  # expand to RGBA for the comparator
        rgba = bytearray(width * height * 4)
        for i in range(width * height):
            rgba[i * 4 : i * 4 + 3] = out[i * 3 : i * 3 + 3]
            rgba[i * 4 + 3] = 255
        out = rgba
    return width, height, bytes(out)


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------

def esc(text) -> str:
    return (
        str(text)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def write_reports(out_dir: Path, rows: list) -> None:
    """rows: list of dicts (name, render, golden, diff, db, passed, error)."""
    md = ["# Zahlen glTF Render-Fidelity Results", ""]
    md.append(f"Threshold: {FIDELITY_TEST_THRESHOLD} dB (generator convention); lower = closer to the golden.<br>")
    md.append("| Scenario | RMSE ratio (dB) | Result |")
    md.append("| --- | --- | --- |")
    html = [
        "<html><head><meta charset='utf-8'><title>Zahlen glTF Fidelity Results</title></head>",
        "<body style='background:#111;color:#eee;font-family:sans-serif;margin:24px'>",
        f"<h2>Zahlen glTF Render-Fidelity Results</h2>",
        f"<p>Threshold: <b>{FIDELITY_TEST_THRESHOLD} dB</b> (generator convention); lower = closer to the golden. "
        "Note: Zahlen currently bakes its IBL from a procedural sky, not the scenario's HDR, and covers a flat "
        "background — expect offset deltas until those are aligned; the metric and the harness are the integration.</p>",
        "<table border='1' cellpadding='8' style='border-collapse:collapse'>",
        "<tr><th>Scenario</th><th>Zahlen</th><th>Golden</th><th>Diff</th><th>RMSE ratio (dB)</th><th>Result</th></tr>",
    ]
    for r in rows:
        db = r["db"]
        if isinstance(db, str):
            db_cell, result, md_verdict = "—", esc(r["error"]), "—"
        else:
            ok = db <= FIDELITY_TEST_THRESHOLD
            db_cell = f"{db:.2f}"
            result = "<span style='color:#7f7'>PASS</span>" if ok else "<span style='color:#f77'>DIFFERS</span>"
            md_verdict = "PASS" if ok else "DIFFERS"
        md_row = f"| {esc(r['name'])} | {db_cell} | {md_verdict} |"
        md.append(md_row)

        def img(rel):
            return f"<img src='{esc(rel)}' width='280' style='image-rendering:auto'/>" if rel else "—"

        html.append(
            f"<tr><td>{esc(r['name'])}</td>"
            f"<td>{img(r['render_rel'])}</td>"
            f"<td>{img(r['golden_rel'])}</td>"
            f"<td>{img(r['diff_rel'])}</td>"
            f"<td>{db_cell}</td><td>{result}</td></tr>"
        )
    html.append("</table></body></html>")
    (out_dir / "report.md").write_text("\n".join(md) + "\n")
    (out_dir / "report.html").write_text("\n".join(html))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fidelity", type=Path, default=None, help="Path to glTF-Render-Fidelity-Generator (auto-clones if absent)")
    parser.add_argument("--samples", type=Path, default=None, help="Path to glTF-Sample-Assets (auto-clones if absent)")
    parser.add_argument("--engine", type=Path, default=None, help="Path to the FidelityHarness binary")
    parser.add_argument("--output", type=Path, default=None, help="Output directory (default: build/fidelity_output)")
    parser.add_argument("--scenario", nargs="*", default=[], help="Only these scenario name substrings/names")
    parser.add_argument("--renderer", nargs="*", default=[], help="Golden preference order (default: filament blender-cycles ...)")
    parser.add_argument("--ambient-scale", type=float, default=1.0, help="IBL ambient scale forwarded to the harness (default 1.0)")
    parser.add_argument("--limit", type=int, default=0, help="Stop after N scenarios (0 = all)")
    parser.add_argument("--keep-ppm", action="store_true", help="Keep the intermediate PPM files")
    parser.add_argument("--list", action="store_true", help="List resolved scenarios and exit")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    fidelity_repo = find_fidelity_repo(args.fidelity, repo_root)
    samples_dir = find_samples_repo(args.samples, fidelity_repo, repo_root)
    config_path = fidelity_repo / "test" / "config.json"
    goldens_dir = fidelity_repo / "test" / "goldens"

    config = json.loads(config_path.read_text())
    scenarios = config.get("scenarios", [])
    renderers = args.renderer or GOLDEN_RENDERERS

    resolved = []
    for raw in scenarios:
        s = merge_scenario(raw)
        s["model_abs"] = str(resolve_model(s["model"], fidelity_repo, samples_dir))
        resolved.append(s)

    if args.list:
        for s in resolved:
            print(f"{s['name']}  {s['orbit']}  {s['dimensions']}  exists={Path(s['model_abs']).exists()}")
        return 0

    engine = find_engine_binary(repo_root, args.engine)
    if engine is None:
        print("[error] FidelityHarness binary not found; build it or pass --engine.", file=sys.stderr)
        return 2

    out_dir = args.output or (repo_root / "build" / "fidelity_output")
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    count = 0
    for s in resolved:
        if args.scenario and not any(sel.lower() in s["name"].lower() for sel in args.scenario):
            continue
        if args.limit and count >= args.limit:
            break
        count += 1

        name = s["name"]
        dims = s["dimensions"]
        model_abs = Path(s["model_abs"])
        print(f"[*] {name}")

        if not model_abs.exists():
            print(f"    [skip] model not on disk: {model_abs}")
            rows.append({"name": name, "db": "error", "error": "model missing", "render_rel": None, "golden_rel": None, "diff_rel": None})
            continue

        scenario_json = out_dir / f"{name}.json"
        handoff = dict(s)
        handoff["model"] = str(model_abs)  # the harness opens `model` verbatim
        scenario_json.write_text(json.dumps(handoff, indent=2))

        ppm_path = out_dir / f"{name}_zahlen.ppm"
        png_path = out_dir / f"{name}_zahlen.png"

        cmd = [
            str(engine.resolve()),
            "--headless",
            f"--scenario={scenario_json}",
            f"--output={ppm_path}",
            f"--ambient-scale={args.ambient_scale}",
        ]
        result = subprocess.run([str(x) for x in cmd], capture_output=True, text=True)
        if result.returncode != 0:
            joined = (result.stderr + result.stdout).strip().splitlines()
            tail = " | ".join(joined[-6:]) if joined else f"exit {result.returncode}"
            print(f"    [-] harness failed: {tail}")
            rows.append({"name": name, "db": "error", "error": tail, "render_rel": None, "golden_rel": None, "diff_rel": None})
            continue

        # PPM -> PNG (pure python; ImageMagick only if the PPM is unexpected).
        try:
            w, h, rgb = read_ppm(ppm_path)
            write_png(png_path, w, h, rgb)
        except Exception as exc:
            magick = shutil.which("magick") or shutil.which("convert")
            if not magick:
                print(f"    [-] PPM conversion failed and ImageMagick is not available: {exc}")
                rows.append({"name": name, "db": "error", "error": f"ppm conversion: {exc}", "render_rel": None, "golden_rel": None, "diff_rel": None})
                continue
            subprocess.run([magick, str(ppm_path), str(png_path)], check=True)
            _, _, rgb = read_ppm(ppm_path)
        if not args.keep_ppm:
            ppm_path.unlink(missing_ok=True)

        # Choose a golden.
        golden_png = None
        golden_rel = None
        for renderer in renderers:
            cand = goldens_dir / name / f"{renderer}-golden.png"
            if cand.exists():
                golden_png = cand
                golden_rel = f"{name}-golden-{renderer}.png"
                (out_dir / golden_rel).write_bytes(cand.read_bytes())
                break
        if golden_png is None:
            print("    [skip] no reference golden for this scenario")
            rows.append({"name": name, "db": "error", "error": "no golden", "render_rel": None, "golden_rel": None, "diff_rel": None})
            continue

        # Load and (if needed) reconcile sizes. The Khronos goldens are
        # captured at DEVICE_PIXEL_RATIO = 2 (generator src/common.ts), i.e.
        # twice the scenario dimensions, while the harness renders at the
        # scenario's 1x dimensions. Comparing meant downscaling, not cropping
        # the golden's top-left quadrant: area-average whichever image is
        # larger onto the smaller one's size (never upscale).
        cw, ch, cand_rgba = read_png(png_path)
        gw, gh, gold_rgba = read_png(golden_png)
        width, height = min(cw, gw), min(ch, gh)
        if (cw, ch) != (gw, gh):
            print(f"    [warn] size mismatch candidate {cw}x{ch} vs golden {gw}x{gh}; comparing at {width}x{height}")
            cand_rgba = _downscale_area(cand_rgba, cw, ch, width, height)
            gold_rgba = _downscale_area(gold_rgba, gw, gh, width, height)

        rms, deltas = analyze(cand_rgba, gold_rgba, width, height)
        db = to_decibel(rms)
        verdict = "PASS" if db <= FIDELITY_TEST_THRESHOLD else "DIFFERS"
        print(f"    RMSE ratio vs {golden_png.parent.name}/{golden_png.name}: {db:.2f} dB [{verdict}]")

        diff_rel = None
        try:
            diff_rel = f"{name}_diff.png"
            write_diff_png(out_dir / diff_rel, cand_rgba, gold_rgba, deltas, width, height)
        except Exception as exc:
            print(f"    [warn] diff image not written: {exc}")

        rows.append(
            {
                "name": name,
                "db": db,
                "error": None,
                "render_rel": f"{name}_zahlen.png",
                "golden_rel": golden_rel,
                "diff_rel": diff_rel,
            }
        )

    write_reports(out_dir, rows)
    print(f"[+] {count} scenarios processed; report at {out_dir / 'report.html'} ({out_dir / 'report.md'})")
    return 0


def _downscale_area(rgba, w, h, nw, nh):
    """Area-average an RGBA image to (nw, nh), <= the original size."""
    nw, nh = int(nw), int(nh)
    if nw > w or nh > h:
        raise ValueError(f"refusing to upscale {w}x{h} to {nw}x{nh}")
    out = bytearray(nw * nh * 4)
    # Integer box per output pixel, mapped into source space. Boundaries are
    # w*x//nw / h*y//nh so they cover [0, w) / [0, h) exactly.
    x0 = [(w * x) // nw for x in range(nw + 1)]
    y0 = [(h * y) // nh for y in range(nh + 1)]
    for oy in range(nh):
        sy_start, sy_end = y0[oy], y0[oy + 1]
        for ox in range(nw):
            sx_start, sx_end = x0[ox], x0[ox + 1]
            acc_r = acc_g = acc_b = acc_a = 0
            count = 0
            for sy in range(sy_start, sy_end):
                row = sy * w * 4
                p = row + sx_start * 4
                for sx in range(sx_start, sx_end):
                    acc_r += rgba[p]
                    acc_g += rgba[p + 1]
                    acc_b += rgba[p + 2]
                    acc_a += rgba[p + 3]
                    count += 1
                    p += 4
            o = (oy * nw + ox) * 4
            out[o] = acc_r // count
            out[o + 1] = acc_g // count
            out[o + 2] = acc_b // count
            out[o + 3] = acc_a // count
    return bytes(out)


if __name__ == "__main__":
    sys.exit(main())
