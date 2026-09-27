// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN {

// NOLINTBEGIN(performance-enum-size)
enum class AudioHandle : uint64_t { Invalid = 0 };
enum class SynthHandle : uint64_t { Invalid = 0 };

enum class AudioWaveformType : uint8_t { Sine = 0, Square = 1, Triangle = 2, Sawtooth = 3 };
enum class AudioFilterType : uint8_t { LowPass = 0, HighPass = 1, BandPass = 2, Notch = 3 };
enum class AudioNoiseType : uint8_t { White = 0, Pink = 1, Brownian = 2 };
// NOLINTEND(performance-enum-size)

}
