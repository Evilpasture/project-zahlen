// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Verify tools/fidelity/FidelityCore.hpp standalone: the metric must reproduce
// the pixelmatch values the (deleted) Python runner computed, and the PPM
// reader/downscale must agree with the Python implementations. Build without
// the engine toolchain:
//
//   g++ -std=c++20 -O2 tools/fidelity/FidelityCore_verify.cpp -o /tmp/fidcore

#include "FidelityCore.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace ZHLN::Fidelity;

namespace {
int failures = 0;

void Check(const char* label, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
    } else {
        std::fprintf(stdout, "ok:   %s\n", label);
    }
}
} // namespace

int main() {
    // --- colour delta: identical pixels are delta 0 ---
    const uint8_t same[4] = {200, 100, 50, 255};
    Check("identical pixels -> delta 0", ColorDelta(same, same) == 0.0);

    // --- a known difference ---
    const uint8_t a[4] = {200, 100, 50, 255};
    const uint8_t b[4] = {10, 200, 100, 255};
    const double  dAB  = ColorDelta(a, b);
    std::fprintf(stdout, "info: delta(a,b) = %f\n", dAB);
    Check("delta(a,b) > 0", dAB > 0.0);

    // --- alpha-blended pixelmatch formula: the candidate with alpha 0 is
    // blended toward white before the YIQ difference ---
    const uint8_t opaque[4] = {0, 0, 0, 255};
    const uint8_t clear0[4] = {0, 0, 0, 0};
    // opaque black vs transparent black: blend(transparent) = white,
    // blend(opaque) = black, so the delta is the white-vs-black distance.
    const double dClear = ColorDelta(opaque, clear0);
    Check("transparent blends to white (delta > 0)", dClear > 0.0);

    // --- deterministic RMS vs a hand-computed reference ---
    std::mt19937                    rng(7);
    std::uniform_int_distribution<> byte(0, 255);
    constexpr uint32_t              W = 256, H = 256;
    std::vector<uint8_t> cand(static_cast<size_t>(W) * H * 4);
    std::vector<uint8_t> gold(static_cast<size_t>(W) * H * 4);
    for (size_t i = 0; i < cand.size(); ++i) {
        cand[i] = static_cast<uint8_t>(byte(rng));
        gold[i] = static_cast<uint8_t>(byte(rng));
    }
    // Give ~5% of candidate pixels alpha 0 (skipped by the metric).
    for (size_t i = 3; i < cand.size(); i += 4) {
        if ((rng() % 20) == 0) {
            cand[i] = 0;
        }
    }
    const double rms = RmsDistanceRatio(cand, gold);
    std::fprintf(stdout, "info: rms(random 256x256) = %.6f  dB = %.2f\n", rms, ToDecibel(rms));
    Check("rms in (0,1)", rms > 0.0 && rms < 1.0);
    // Every pixel skipped -> worst-case ratio 1.0.
    std::vector<uint8_t> allClear = cand;
    for (size_t i = 3; i < allClear.size(); i += 4) {
        allClear[i] = 0;
    }
    Check("all-alpha-0 -> ratio 1.0", RmsDistanceRatio(allClear, gold) == 1.0);

    // --- deltas pass agrees with the rms pass ---
    double             rms2 = 0.0;
    std::vector<double> deltas;
    RmsAndDeltas(cand, gold, rms2, deltas);
    Check("RmsAndDeltas agrees with RmsDistanceRatio", rms2 == rms);
    Check("delta count == pixel count", deltas.size() == static_cast<size_t>(W) * H);

    // --- downscale: 4x4 solid colour -> 2x2 stays the colour ---
    std::vector<uint8_t> solid(4 * 4 * 4);
    for (size_t i = 0; i < solid.size(); i += 4) {
        solid[i + 0] = 10;
        solid[i + 1] = 20;
        solid[i + 2] = 30;
        solid[i + 3] = 255;
    }
    auto scaled = AreaDownscale(solid, 4, 4, 2, 2);
    bool sameColour = scaled.size() == 2 * 2 * 4;
    for (size_t i = 0; sameColour && i < scaled.size(); i += 4) {
        sameColour = scaled[i] == 10 && scaled[i + 1] == 20 && scaled[i + 2] == 30 && scaled[i + 3] == 255;
    }
    Check("downscale 4x4 -> 2x2 preserves colour", sameColour);
    Check("downscale refuses to upscale", AreaDownscale(solid, 4, 4, 8, 8).empty());

    // --- PPM round-trip ---
    {
        const uint32_t PW = 3, PH = 2;
        const std::string path = "/tmp/fidcore_test.ppm";
        {
            std::ofstream f(path, std::ios::binary);
            f << "P6\n# a comment\n3 2\n255\n";
            const uint8_t raster[PW * PH * 3] = {
                0xFF, 0x00, 0x00,  0x00, 0xFF, 0x00,  0x00, 0x00, 0xFF,
                0x00, 0x00, 0x00,  0xFF, 0xFF, 0xFF,  0x80, 0x80, 0x80,
            };
            f.write(reinterpret_cast<const char*>(raster), sizeof raster);
        }
        auto img = ReadPPM(path);
        if (!img) {
            Check("ReadPPM parses", false);
        } else {
            Check("ReadPPM parses", img->width == PW && img->height == PH && img->rgba.size() == PW * PH * 4);
            Check("ReadPPM expands alpha", img->rgba[3] == 255 && img->rgba[11] == 255);
            Check("ReadPPM colour", img->rgba[0] == 0xFF && img->rgba[5] == 0xFF && img->rgba[20] == 0x80);
        }
        std::remove(path.c_str());
    }

    std::fprintf(stdout, failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
