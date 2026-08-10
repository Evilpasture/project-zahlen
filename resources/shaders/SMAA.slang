// resources/shaders/SMAA.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Slang module shim for the external SMAA library (https://github.com/iryoku/smaa)
// The original SMAA.hlsl is HLSL-only and is downloaded at configure time via CMake.
// This module wraps it for Slang's import system: `import SMAA;` will pull in the HLSL library.
// For DXC compatibility, smaa_wrap.hlsl still uses `#include "SMAA.hlsl"`.
// For Slang, smaa_wrap.slang uses `import SMAA;` which resolves to this file.

module SMAA;

// Pull the HLSL library into the Slang module.
// __include is Slang's module-aware include (different from preprocessor #include).
// It makes SMAA's declarations available as `SMAA::` or via import.
// Fallback to #include for DXC compatibility if __include not available.
#if defined(__SLANG__)
__include "SMAA.hlsl"
#else
#include "SMAA.hlsl"
#endif
