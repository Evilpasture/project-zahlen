// resources/shaders/SMAA.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Slang module shim for the external SMAA library (https://github.com/iryoku/smaa)
// The original SMAA.hlsl is HLSL-only and is downloaded at configure time via CMake.
// This module wraps it for Slang's import system: `import SMAA;` will pull in the HLSL library.

module SMAA;

#define SMAA_HLSL_4 1
#define SMAA_PRESET_ULTRA 1
#define SamplerState static SamplerState
#include "SMAA.hlsl"
#undef SamplerState
