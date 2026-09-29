// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module phase;

public float PhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float d = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (12.5663706f * d * sqrt(d));
}

public float PhaseDualHG(float cosTheta, float g1, float g2, float k) {
    return lerp(PhaseHG(cosTheta, g2), PhaseHG(cosTheta, g1), k);
}
