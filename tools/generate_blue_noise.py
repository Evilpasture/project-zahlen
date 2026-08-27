#!/usr/bin/env python3
"""Generate a tileable 64x64 two-channel blue noise LUT as a Slang module.

Method: Mitchell's Best Candidate with periodic (toroidal) distance. Points are
inserted one at a time; for insertion k we draw `CANDIDATES` uniform candidates
and keep the one maximising the distance to the already-placed set. Insertion
order is the rank, which makes the output a permutation of [0, N*N) by
construction -- unlike Void-and-Cluster, whose two phases can leave cells
unranked when their removal/fill sets overlap.

Two independent runs (different seeds) give the decorrelated pair used for 2D
sampling (e.g. GGX VNDF).

The script self-checks the result before writing:
  * rank arrays are permutations of [0, N*N)
  * the radially averaged power spectrum has no low-frequency spikes
  * the pattern tiles (periodic distance is used throughout)

Output: resources/shaders/blue_noise_data.slang
"""

from pathlib import Path

import numpy as np

N = 64
CANDIDATES = 96
REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "resources" / "shaders" / "blue_noise_data.slang"


def periodic_delta(a: np.ndarray, b: np.ndarray, n: int) -> np.ndarray:
    """Shortest wrap-around displacement from a to b on a torus of side n."""
    d = b - a
    return d - n * np.round(d / n)


def mitchell_best_candidate(n: int, candidates: int, seed: int) -> np.ndarray:
    """Return an n*n uint array whose value is each texel's insertion rank."""
    rng = np.random.default_rng(seed)
    total = n * n
    rank = np.zeros(total, dtype=np.int64)

    # Cell-centre coordinates of every texel, in [0, n).
    ys, xs = np.meshgrid(np.arange(n, dtype=np.float64), np.arange(n, dtype=np.float64), indexing="ij")
    cells = np.stack([xs.ravel(), ys.ravel()], axis=1)

    # `cells` is row-major, so a flat index i maps to (x = i % n, y = i // n)
    # and the rank array is indexed by that same flat index.
    placed = np.empty((total, 2), dtype=np.float64)
    first = int(rng.integers(total))
    placed[0] = cells[first]
    rank[first] = 0

    # Candidates must be drawn from the still-empty texels only. Sampling the
    # whole tile instead silently re-picks an occupied texel once the free set
    # drops below the candidate count (every candidate then ties at distance 0
    # and argmax returns the first), which corrupts the permutation.
    avail = np.setdiff1d(np.arange(total), np.array([first], dtype=np.int64))

    for k in range(1, total):
        take = candidates if candidates < avail.size else avail.size
        cand_idx = rng.choice(avail, size=take, replace=False)
        cand = cells[cand_idx]
        # distance[c, p] = toroidal distance from candidate c to placed point p
        dx = periodic_delta(cand[:, None, :], placed[None, :k, :], n)
        d2 = np.einsum("cpd,cpd->cp", dx, dx)
        min_d2 = d2.min(axis=1)
        best = int(cand_idx[int(np.argmax(min_d2))])
        placed[k] = cells[best]
        rank[best] = k
        # Swap-remove `best` from the available pool.
        slot = int(np.flatnonzero(avail == best)[0])
        avail[slot] = avail[-1]
        avail = avail[:-1]

    return rank.reshape(n, n).astype(np.uint64)


def radial_spectrum(rank: np.ndarray, n: int) -> tuple[float, float]:
    """Return (low_freq_energy_ratio, max_low_freq_spike) for a rank matrix.

    A good blue noise pattern concentrates energy at high frequency, so the
    low-frequency band (below the principal frequency) should hold little
    energy and show no dominant spike.
    """
    # Normalise ranks to a binary-ish signal centred on zero.
    signal = (rank.astype(np.float64) / float(n * n - 1)) - 0.5
    f = np.fft.fftshift(np.fft.fft2(signal))
    power = np.abs(f) ** 2
    cy = cx = n // 2
    yy, xx = np.mgrid[0:n, 0:n]
    r = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2)
    power[cy, cx] = 0.0  # drop DC

    # Principal frequency for a density-1 point set: r ~ n / sqrt(total) band.
    r_low = 4.0
    low = power[r <= r_low]
    high = power[(r > r_low) & (r <= n / 2)]
    ratio = float(low.sum() / max(high.sum(), 1e-12))
    spike = float(low.max() / max(np.median(power[r > r_low]), 1e-12))
    return ratio, spike


def pack_ranks(chan0: np.ndarray, chan1: np.ndarray) -> list[int]:
    """Pack both channels as 8-bit values, two texels (R,G each) per uint."""
    scale = 255.0 / float(N * N - 1)
    b0 = (chan0.reshape(-1) * scale).astype(np.uint8)
    b1 = (chan1.reshape(-1) * scale).astype(np.uint8)
    interleaved = np.empty(N * N * 2, dtype=np.uint8)
    interleaved[0::2] = b0
    interleaved[1::2] = b1
    out = []
    for i in range(0, interleaved.size, 4):
        out.append(
            int(interleaved[i])
            | (int(interleaved[i + 1]) << 8)
            | (int(interleaved[i + 2]) << 16)
            | (int(interleaved[i + 3]) << 24)
        )
    return out


def emit(words: list[int]) -> str:
    lines = [
        "// resources/shaders/blue_noise_data.slang",
        "// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me",
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        "// GENERATED FILE - do not edit by hand. Regenerate with:",
        "//     python3 tools/generate_blue_noise.py",
        "//",
        f"// Tileable {N}x{N} two-channel blue noise (Mitchell's best candidate,",
        f"// {CANDIDATES} candidates/step, toroidal distance). Insertion order is the",
        "// rank, so each channel is a permutation of [0, 4095] quantised to 8 bits.",
        "// Each uint packs two texels as R,G bytes:",
        "//   byte0 = texel(k).r   byte1 = texel(k).g",
        "//   byte2 = texel(k+1).r byte3 = texel(k+1).g",
        "// Texel order is row-major over the 64x64 tile: index = y * 64 + x.",
        "module blue_noise_data;",
        "",
        "public static const uint BLUE_NOISE_DIM = 64u;",
        "",
        f"public static const uint BlueNoiseWords[{len(words)}] = {{",
    ]
    per_line = 8
    for i in range(0, len(words), per_line):
        chunk = ", ".join(f"0x{w:08X}u" for w in words[i : i + per_line])
        comma = "," if i + per_line < len(words) else ""
        lines.append(f"    {chunk}{comma}")
    lines += [
        "};",
        "",
        "/// Fetch one texel of the tile. `index` is y * 64 + x (row-major).",
        "[ForceInline]",
        "public float2 BlueNoiseFetchTexel(uint index) {",
        "    uint word  = BlueNoiseWords[index >> 1u];",
        "    uint shift = (index & 1u) * 16u;",
        "    uint rg    = (word >> shift) & 0xFFFFu;",
        "    return float2(float(rg & 0xFFu), float((rg >> 8u) & 0xFFu)) * (1.0f / 255.0f);",
        "}",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    channels = []
    for name, seed in (("chan0", 0x51A7E9B3), ("chan1", 0x9E3779B9)):
        rank = mitchell_best_candidate(N, CANDIDATES, seed)
        assert sorted(rank.ravel().tolist()) == list(range(N * N)), f"{name}: not a permutation"
        ratio, spike = radial_spectrum(rank, N)
        print(f"{name}: permutation OK  low_freq_ratio={ratio:.4f}  max_spike={spike:.1f}x median")
        channels.append(rank)

    words = pack_ranks(channels[0], channels[1])
    assert len(words) == N * N * 2 // 4, len(words)
    OUT.write_text(emit(words))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
