// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// tools/fidelity/FidelityCore.hpp
//
// Dependency-free core of the fidelity runner: the pixelmatch YIQ metric, the
// P6 PPM reader for engine captures, and the area-average downscale used to
// reconcile candidate/golden sizes. Nothing in this header names the engine,
// extras, stb or simdjson, so it compiles and unit-tests standalone (g++ only),
// which the heavier halves of tools/fidelity/ cannot do away from the repo's
// full build tree.
//
// The metric is a faithful port of the Khronos generator's
// src/third_party/pixelmatch/color-delta.ts and ImageComparator.analyze():
//   * a pixel is skipped when the CANDIDATE's alpha is 0 (omitBackground);
//   * colours are pre-multiplied by alpha then blended toward white;
//   * the YIQ difference is weighted (0.5053, 0.299, 0.1957) and summed over
//     the blended colour distance;
//   * rmsDistanceRatio = sqrt(sum(delta^2) / modelPixels) / 35215;
//   * decibels = 10 * log10(ratio).

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Fidelity {

inline constexpr double kMaxColorDistance = 35215.0;

// An RGBA image in tightly packed 4-component bytes (R, G, B, A).
struct Image {
    uint32_t              width  = 0;
    uint32_t              height = 0;
    std::vector<uint8_t> rgba;
};

// ---------------------------------------------------------------------------
// PPM (P6, binary) — the output of RenderContext::CaptureScreenshotPPM.
// ---------------------------------------------------------------------------

// Reads a P6 PPM and expands it to RGBA (alpha 255). Tolerates '#' comments in
// the header, which some writers emit between the magic and the dimensions.
inline std::optional<Image> ReadPPM(std::string_view path) {
    std::ifstream f {std::string(path), std::ios::binary};
    if (!f) {
        return std::nullopt;
    }
    std::string data {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto nextToken = [&](size_t& pos) -> std::string {
        const size_t n = data.size();
        // Skip whitespace and any '#' comment lines.
        for (;;) {
            while (pos < n && isSpace(static_cast<unsigned char>(data[pos]))) {
                ++pos;
            }
            if (pos < n && data[pos] == '#') {
                while (pos < n && data[pos] != '\n') {
                    ++pos;
                }
                continue;
            }
            break;
        }
        const size_t start = pos;
        while (pos < n && !isSpace(static_cast<unsigned char>(data[pos]))) {
            ++pos;
        }
        return data.substr(start, pos - start);
    };

    size_t pos         = 0;
    const std::string magic    = nextToken(pos);
    if (magic != "P6") {
        return std::nullopt;
    }
    const std::string widthTok  = nextToken(pos);
    const std::string heightTok = nextToken(pos);
    const std::string maxTok    = nextToken(pos);

    uint32_t       parsedW = 0, parsedH = 0;
    const auto [wp, we1] = std::from_chars(widthTok.data(), widthTok.data() + widthTok.size(), parsedW);
    const auto [hp, we2] = std::from_chars(heightTok.data(), heightTok.data() + heightTok.size(), parsedH);
    (void)wp;
    (void)hp;
    if (we1 != std::errc {} || we2 != std::errc {} || maxTok != "255") {
        return std::nullopt;
    }
    const uint32_t width  = parsedW;
    const uint32_t height = parsedH;
    // One whitespace byte separates the header from the raster.
    while (pos < data.size() && isSpace(static_cast<unsigned char>(data[pos]))) {
        ++pos;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (data.size() < pos + pixelCount * 3) {
        return std::nullopt;
    }

    Image out {
        .width  = width,
        .height = height,
        .rgba   = {},
    };
    out.rgba.resize(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i) {
        out.rgba[i * 4 + 0] = static_cast<uint8_t>(data[pos + i * 3 + 0]);
        out.rgba[i * 4 + 1] = static_cast<uint8_t>(data[pos + i * 3 + 1]);
        out.rgba[i * 4 + 2] = static_cast<uint8_t>(data[pos + i * 3 + 2]);
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

// Netpbm PAM (P7). The fidelity harness writes this when the capture path
// ends in .pam so omit-background alpha reaches the metric. P6 has no alpha
// channel; ReadPPM stays the P6 reader the verify test exercises.
inline std::optional<Image> ReadPAM(std::string_view path) {
    std::ifstream f {std::string(path), std::ios::binary};
    if (!f) {
        return std::nullopt;
    }
    std::string data {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (data.size() < 3 || data[0] != 'P' || data[1] != '7') {
        return std::nullopt;
    }

    size_t pos = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t maxval = 0;
    bool ended = false;
    bool first = true;
    while (!ended) {
        if (pos >= data.size()) {
            return std::nullopt;
        }
        const size_t start = pos;
        while (pos < data.size() && data[pos] != '\n') {
            ++pos;
        }
        std::string line = data.substr(start, pos - start);
        if (pos < data.size() && data[pos] == '\n') {
            ++pos;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (first) {
            if (line != "P7") {
                return std::nullopt;
            }
            first = false;
            continue;
        }
        if (line == "ENDHDR") {
            ended = true;
            break;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos) {
            return std::nullopt;
        }
        const std::string key = line.substr(0, sp);
        const std::string val = line.substr(sp + 1);
        uint32_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed);
        (void)ptr;
        if (ec != std::errc {}) {
            if (key == "TUPLTYPE") {
                continue;
            }
            return std::nullopt;
        }
        if (key == "WIDTH") {
            width = parsed;
        } else if (key == "HEIGHT") {
            height = parsed;
        } else if (key == "DEPTH") {
            depth = parsed;
        } else if (key == "MAXVAL") {
            maxval = parsed;
        }
    }
    if (!ended || width == 0 || height == 0 || depth != 4 || maxval != 255) {
        return std::nullopt;
    }
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (data.size() < pos + pixelCount * 4) {
        return std::nullopt;
    }
    Image out {.width = width, .height = height, .rgba = {}};
    out.rgba.resize(pixelCount * 4);
    std::memcpy(out.rgba.data(), data.data() + pos, pixelCount * 4);
    return out;
}

// P6 or P7. Compare uses this so a .ppm capture (alpha forced opaque) and a
// .pam capture (real alpha) take the same path.
inline std::optional<Image> ReadCapture(std::string_view path) {
    std::ifstream f {std::string(path), std::ios::binary};
    if (!f) {
        return std::nullopt;
    }
    char magic[2] = {};
    f.read(magic, 2);
    if (!f) {
        return std::nullopt;
    }
    if (magic[0] == 'P' && magic[1] == '7') {
        return ReadPAM(path);
    }
    return ReadPPM(path);
}

// ---------------------------------------------------------------------------
// Area-average downscale (integer boxes, never upscales, never crops).
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> AreaDownscale(std::span<const uint8_t> rgba, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh) {
    if (nw > w || nh > h) {
        return {};
    }
    std::vector<uint32_t> x0(nw + 1);
    std::vector<uint32_t> y0(nh + 1);
    for (uint32_t x = 0; x <= nw; ++x) {
        x0[x] = (w * x) / nw;
    }
    for (uint32_t y = 0; y <= nh; ++y) {
        y0[y] = (h * y) / nh;
    }

    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    for (uint32_t oy = 0; oy < nh; ++oy) {
        const uint32_t syStart = y0[oy];
        const uint32_t syEnd   = y0[oy + 1];
        for (uint32_t ox = 0; ox < nw; ++ox) {
            const uint32_t sxStart = x0[ox];
            const uint32_t sxEnd   = x0[ox + 1];
            uint64_t       accR = 0, accG = 0, accB = 0, accA = 0;
            uint64_t       count = 0;
            for (uint32_t sy = syStart; sy < syEnd; ++sy) {
                const uint8_t* row = rgba.data() + static_cast<size_t>(sy) * w * 4 + sxStart * 4;
                for (uint32_t sx = sxStart; sx < sxEnd; ++sx) {
                    accR += row[0];
                    accG += row[1];
                    accB += row[2];
                    accA += row[3];
                    count += 1;
                    row += 4;
                }
            }
            uint8_t* p = out.data() + (static_cast<size_t>(oy) * nw + ox) * 4;
            p[0] = static_cast<uint8_t>(accR / count);
            p[1] = static_cast<uint8_t>(accG / count);
            p[2] = static_cast<uint8_t>(accB / count);
            p[3] = static_cast<uint8_t>(accA / count);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The pixelmatch YIQ metric.
// ---------------------------------------------------------------------------

// Colour delta for one pixel pair (both RGBA, 4 bytes), generator-exact.
[[nodiscard]] inline double ColorDelta(const uint8_t* cand, const uint8_t* gold) {
    const double a1 = cand[3] / 255.0;
    const double a2 = gold[3] / 255.0;

    // Pre-multiply by alpha, then blend toward white (255).
    const double r1 = 255.0 + (cand[0] - 255.0) * a1;
    const double g1 = 255.0 + (cand[1] - 255.0) * a1;
    const double b1 = 255.0 + (cand[2] - 255.0) * a1;
    const double r2 = 255.0 + (gold[0] - 255.0) * a2;
    const double g2 = 255.0 + (gold[1] - 255.0) * a2;
    const double b2 = 255.0 + (gold[2] - 255.0) * a2;

    const double y = (r1 * 0.29889531 + g1 * 0.58662247 + b1 * 0.11448223) - (r2 * 0.29889531 + g2 * 0.58662247 + b2 * 0.11448223);
    const double i = (r1 * 0.59597799 - g1 * 0.27417610 - b1 * 0.32180189) - (r2 * 0.59597799 - g2 * 0.27417610 - b2 * 0.32180189);
    const double q = (r1 * 0.21147017 - g1 * 0.52261711 + b1 * 0.31114694) - (r2 * 0.21147017 - g2 * 0.52261711 + b2 * 0.31114694);

    return 0.5053 * y * y + 0.299 * i * i + 0.1957 * q * q;
}

// rmsDistanceRatio between equal-length RGBA buffers. Candidate alpha gates the
// pixel (0 => skipped, matching ImageComparator.analyze()); the golden's alpha
// still participates through ColorDelta's blend. Returns 1.0 (worst) when no
// pixel is comparable.
[[nodiscard]] inline double RmsDistanceRatio(std::span<const uint8_t> cand, std::span<const uint8_t> gold) {
    const size_t pixels = cand.size() / 4;
    double       sum    = 0.0;
    uint64_t     count  = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* c = cand.data() + i * 4;
        if (c[3] == 0) {
            continue;
        }
        const double d = ColorDelta(c, gold.data() + i * 4);
        sum += d * d;
        ++count;
    }
    if (count == 0) {
        return 1.0;
    }
    return std::sqrt(sum / static_cast<double>(count)) / kMaxColorDistance;
}

// Same pass as RmsDistanceRatio but also fills `deltas` (one per pixel, 0 where
// the candidate alpha is 0) for the closest-golden diff image.
inline void RmsAndDeltas(
    std::span<const uint8_t> cand, std::span<const uint8_t> gold, double& outRms, std::vector<double>& outDeltas
) {
    const size_t pixels = cand.size() / 4;
    outDeltas.assign(pixels, 0.0);
    double   sum   = 0.0;
    uint64_t count = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* c = cand.data() + i * 4;
        if (c[3] == 0) {
            continue;
        }
        const double d = ColorDelta(c, gold.data() + i * 4);
        outDeltas[i]   = d;
        sum += d * d;
        ++count;
    }
    outRms = (count == 0) ? 1.0 : std::sqrt(sum / static_cast<double>(count)) / kMaxColorDistance;
}

[[nodiscard]] inline double ToDecibel(double rmsDistanceRatio) {
    if (rmsDistanceRatio <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10(rmsDistanceRatio);
}

} // namespace ZHLN::Fidelity
