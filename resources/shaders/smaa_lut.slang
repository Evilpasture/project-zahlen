// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU SMAA area + search LUT generation. Ports the CPU trapezoidal / diagonal
// integrators so the textures consumed by SMAA.slang are produced by the same
// language as the filter itself.
import sampling;

public struct SMAALUTPush {
    public uint64_t outAddr;
    public uint width;
    public uint height;
    public uint mode; // 0 = area (160x560), 1 = search (64x16)
};

[vk::push_constant]
SMAALUTPush pc;

static const float kOrthoOffsets[7] = {0.0f, -0.25f, 0.25f, -0.125f, 0.125f, -0.375f, 0.375f};
static const int kEdgesOrtho[16 * 2] = {0, 0, 3, 0, 0, 3, 3, 3, 1, 0, 4, 0, 1, 3, 4, 3, 0, 1, 3, 1, 0, 4, 3, 4, 1, 1, 4, 1, 1, 4, 4, 4};
static const int kEdgesDiag[16 * 2]  = {0, 0, 1, 0, 0, 2, 1, 2, 2, 0, 3, 0, 2, 2, 3, 2, 0, 1, 1, 1, 0, 3, 1, 3, 2, 1, 3, 1, 2, 3, 3, 3};

float2 AreaOrthoInternal(float2 p1, float2 p2, float x) {
    float dx = p2.x - p1.x;
    float dy = p2.y - p1.y;
    float x1 = x;
    float x2 = x + 1.0f;
    float y1 = p1.y + dy * (x1 - p1.x) / dx;
    float y2 = p1.y + dy * (x2 - p1.x) / dx;

    bool inside = (x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x);
    if (!inside)
        return float2(0.0f, 0.0f);

    bool sign1        = y1 < 0.0f;
    bool sign2        = y2 < 0.0f;
    bool is_trapezoid = (sign1 == sign2 || abs(y1) < 1e-4f || abs(y2) < 1e-4f);
    if (is_trapezoid) {
        float a = (y1 + y2) * 0.5f;
        return (a < 0.0f) ? float2(abs(a), 0.0f) : float2(0.0f, abs(a));
    }

    float intersection_x = -p1.y * dx / dy + p1.x;
    float frac           = frac(intersection_x);
    float a1             = (intersection_x > p1.x) ? y1 * frac * 0.5f : 0.0f;
    float a2             = (intersection_x < p2.x) ? y2 * (1.0f - frac) * 0.5f : 0.0f;
    float a              = (abs(a1) > abs(a2)) ? a1 : -a2;
    return (a < 0.0f) ? float2(abs(a1), abs(a2)) : float2(abs(a2), abs(a1));
}

float2 SmoothArea(float d, float2 a1, float2 a2) {
    float2 b1 = sqrt(a1 * 2.0f) * 0.5f;
    float2 b2 = sqrt(a2 * 2.0f) * 0.5f;
    float p   = saturate(d / 32.0f);
    return lerp(b1, a1, p) + lerp(b2, a2, p);
}

float2 AreaOrtho(int pattern, float left, float right, float offset) {
    float d  = left + right + 1.0f;
    float o1 = 0.5f + offset;
    float o2 = 0.5f + offset - 1.0f;

    if (pattern == 0)
        return float2(0.0f, 0.0f);
    if (pattern == 1)
        return (left <= right) ? AreaOrthoInternal(float2(0.0f, o2), float2(d * 0.5f, 0.0f), left) : float2(0.0f, 0.0f);
    if (pattern == 2)
        return (left >= right) ? AreaOrthoInternal(float2(d * 0.5f, 0.0f), float2(d, o2), left) : float2(0.0f, 0.0f);
    if (pattern == 3) {
        float2 a1 = AreaOrthoInternal(float2(0.0f, o2), float2(d * 0.5f, 0.0f), left);
        float2 a2 = AreaOrthoInternal(float2(d * 0.5f, 0.0f), float2(d, o2), left);
        return SmoothArea(d, a1, a2);
    }
    if (pattern == 4)
        return (left <= right) ? AreaOrthoInternal(float2(0.0f, o1), float2(d * 0.5f, 0.0f), left) : float2(0.0f, 0.0f);
    if (pattern == 5)
        return float2(0.0f, 0.0f);
    if (pattern == 6) {
        if (abs(offset) > 0.0f) {
            float2 a1 = AreaOrthoInternal(float2(0.0f, o1), float2(d, o2), left);
            float2 a2 = AreaOrthoInternal(float2(0.0f, o1), float2(d * 0.5f, 0.0f), left) + AreaOrthoInternal(float2(d * 0.5f, 0.0f), float2(d, o2), left);
            return (a1 + a2) * 0.5f;
        }
        return AreaOrthoInternal(float2(0.0f, o1), float2(d, o2), left);
    }
    if (pattern == 7)
        return AreaOrthoInternal(float2(0.0f, o1), float2(d, o2), left);
    if (pattern == 8)
        return (left >= right) ? AreaOrthoInternal(float2(d * 0.5f, 0.0f), float2(d, o1), left) : float2(0.0f, 0.0f);
    if (pattern == 9) {
        if (abs(offset) > 0.0f) {
            float2 a1 = AreaOrthoInternal(float2(0.0f, o2), float2(d, o1), left);
            float2 a2 = AreaOrthoInternal(float2(0.0f, o2), float2(d * 0.5f, 0.0f), left) + AreaOrthoInternal(float2(d * 0.5f, 0.0f), float2(d, o1), left);
            return (a1 + a2) * 0.5f;
        }
        return AreaOrthoInternal(float2(0.0f, o2), float2(d, o1), left);
    }
    if (pattern == 10)
        return float2(0.0f, 0.0f);
    if (pattern == 11)
        return AreaOrthoInternal(float2(0.0f, o2), float2(d, o1), left);
    if (pattern == 12) {
        float2 a1 = AreaOrthoInternal(float2(0.0f, o1), float2(d * 0.5f, 0.0f), left);
        float2 a2 = AreaOrthoInternal(float2(d * 0.5f, 0.0f), float2(d, o1), left);
        return SmoothArea(d, a1, a2);
    }
    if (pattern == 13)
        return AreaOrthoInternal(float2(0.0f, o2), float2(d, o1), left);
    if (pattern == 14)
        return AreaOrthoInternal(float2(0.0f, o1), float2(d, o2), left);
    return float2(0.0f, 0.0f);
}

float AreaDiagInternal1(float2 p1, float2 p2, float2 p) {
    if (p1.x == p2.x && p1.y == p2.y)
        return 1.0f;
    float xm         = (p1.x + p2.x) * 0.5f;
    float ym         = (p1.y + p2.y) * 0.5f;
    float a          = p2.y - p1.y;
    float b          = p1.x - p2.x;
    float const_part = -a * xm - b * ym;
    float count      = 0.0f;
    const int samples     = 30;
    const float inv       = 1.0f / float(samples - 1);
    for (int x = 0; x < samples; ++x) {
        float px   = p.x + float(x) * inv;
        float a_px = a * px;
        for (int y = 0; y < samples; ++y) {
            float py = p.y + float(y) * inv;
            if ((a_px + b * py + const_part) > 0.0f)
                count += 1.0f;
        }
    }
    return count / float(samples * samples);
}

float2 AreaDiagHelper(int pattern, float2 p1, float2 p2, float left, float2 offset) {
    int e1   = kEdgesDiag[pattern * 2 + 0];
    int e2   = kEdgesDiag[pattern * 2 + 1];
    p1       = (e1 > 0) ? p1 + offset : p1;
    p2       = (e2 > 0) ? p2 + offset : p2;
    float a1 = AreaDiagInternal1(p1, p2, float2(1.0f, 0.0f) + float2(left, left));
    float a2 = AreaDiagInternal1(p1, p2, float2(1.0f, 1.0f) + float2(left, left));
    return float2(1.0f - a1, a2);
}

float2 AreaDiag(int pattern, float left, float right, float2 offset) {
    float d = left + right + 1.0f;
    if (pattern == 0)
        return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, 1.0f + d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 1)
        return (AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(d, d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 2)
        return (AreaDiagHelper(pattern, float2(0.0f, 0.0f), float2(1.0f + d, d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 3)
        return AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset);
    if (pattern == 4)
        return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(d, d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 5)
        return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(d, d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 6)
        return AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, d), left, offset);
    if (pattern == 7)
        return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 8)
        return (AreaDiagHelper(pattern, float2(0.0f, 0.0f), float2(1.0f + d, 1.0f + d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, 1.0f + d), left, offset)) *
               0.5f;
    if (pattern == 9)
        return AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, 1.0f + d), left, offset);
    if (pattern == 10)
        return (AreaDiagHelper(pattern, float2(0.0f, 0.0f), float2(1.0f + d, 1.0f + d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 11)
        return (AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, 1.0f + d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    if (pattern == 12)
        return AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, 1.0f + d), left, offset);
    if (pattern == 13)
        return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, 1.0f + d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, 1.0f + d), left, offset)) *
               0.5f;
    if (pattern == 14)
        return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, 1.0f + d), left, offset) +
                AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, d), left, offset)) *
               0.5f;
    return (AreaDiagHelper(pattern, float2(1.0f, 1.0f), float2(1.0f + d, 1.0f + d), left, offset) +
            AreaDiagHelper(pattern, float2(1.0f, 0.0f), float2(1.0f + d, d), left, offset)) *
           0.5f;
}

uint BakeAreaPixel(uint px, uint py) {
    // Left half: orthogonal mappings. Right half: diagonal mappings.
    if (px < 80) {
        int yOff = int(py) / (5 * 16);
        if (yOff < 0 || yOff > 6)
            return 0xFF000000u;
        float offset = kOrthoOffsets[yOff];
        uint posY    = uint(5 * 16 * yOff);
        for (int pattern = 0; pattern < 16; ++pattern) {
            uint startX = 16 * uint(kEdgesOrtho[pattern * 2 + 0]);
            uint startY = posY + 16 * uint(kEdgesOrtho[pattern * 2 + 1]);
            if (px >= startX && px < startX + 16 && py >= startY && py < startY + 16) {
                int left  = int(px - startX);
                int right = int(py - startY);
                float2 res = AreaOrtho(pattern, float(left * left), float(right * right), offset);
                return PackRG8(res.x, res.y);
            }
        }
        return 0xFF000000u;
    }

    float2 diagOffsets[5] = {float2(0.00f, 0.00f), float2(0.25f, -0.25f), float2(-0.25f, 0.25f), float2(0.125f, -0.125f), float2(-0.125f, 0.125f)};
    for (int y = 0; y < 5; ++y) {
        uint posY = uint(4 * 20 * y);
        for (int pattern = 0; pattern < 16; ++pattern) {
            uint startX = 80 + 20 * uint(kEdgesDiag[pattern * 2 + 0]);
            uint startY = posY + 20 * uint(kEdgesDiag[pattern * 2 + 1]);
            if (px >= startX && px < startX + 20 && py >= startY && py < startY + 20) {
                int left  = int(px - startX);
                int right = int(py - startY);
                float2 res = AreaDiag(pattern, float(left), float(right), diagOffsets[y]);
                return PackRG8(res.x, res.y);
            }
        }
    }
    return 0xFF000000u;
}

float BilinearEdge(float e0, float e1, float e2, float e3) {
    float a = lerp(e0, e1, 0.75f);
    float b = lerp(e2, e3, 0.75f);
    return lerp(a, b, 0.875f);
}

void FindEdges(float t, out int e[4]) {
    e[0] = 0;
    e[1] = 0;
    e[2] = 0;
    e[3] = 0;
    int idx = 0;
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c)
                for (int d = 0; d < 2; ++d) {
                    float val = BilinearEdge(float(a), float(b), float(c), float(d));
                    if (abs(val - t) < 1e-4f) {
                        e[0] = a;
                        e[1] = b;
                        e[2] = c;
                        e[3] = d;
                        return;
                    }
                    idx++;
                }
}

int DeltaLeft(int left[4], int top[4]) {
    int d = 0;
    if (top[3] == 1)
        d += 1;
    if (d == 1 && top[2] == 1 && left[1] != 1 && left[3] != 1)
        d += 1;
    return d;
}

int DeltaRight(int left[4], int top[4]) {
    int d = 0;
    if (top[3] == 1 && left[1] != 1 && left[3] != 1)
        d += 1;
    if (d == 1 && top[2] == 1 && left[0] != 1 && left[2] != 1)
        d += 1;
    return d;
}

uint BakeSearchPixel(uint fx, uint fy) {
    // Search LUT: 66x33 workspace, crop (0,17,64,33), flip Y.
    uint cy = 15 - fy;
    uint y  = 17 + cy;
    uint x  = fx;
    if (x >= 64 || fy >= 16)
        return 0xFF000000u;

    // Reconstruct the workspace value at (x, y) of the 66-wide temp buffer.
    // x in [0,32] is left deltas, [33,65] is right deltas.
    int srcX = int(x);
    bool rightHalf = srcX >= 33;
    int comboX = rightHalf ? (srcX - 33) : srcX;
    int comboY = int(y);

    int edgesX[4];
    int edgesY[4];
    FindEdges(0.03125f * float(comboX), edgesX);
    FindEdges(0.03125f * float(comboY), edgesY);
    int val = rightHalf ? DeltaRight(edgesX, edgesY) : DeltaLeft(edgesX, edgesY);
    uint u  = uint(127 * val);
    return 0xFF000000u | (u << 16) | (u << 8) | u;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    uint* pixels = (uint*) pc.outAddr;
    uint packed  = (pc.mode == 0) ? BakeAreaPixel(tid.x, tid.y) : BakeSearchPixel(tid.x, tid.y);
    pixels[tid.y * pc.width + tid.x] = packed;
}
