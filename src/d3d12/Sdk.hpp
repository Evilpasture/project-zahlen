#pragma once

// The one include point for the Microsoft DirectX headers inside the
// d3d12 subsystem. Every backend header includes this instead of naming
// <directx/d3d12.h> itself, so the winadapter-first rule for non-Windows
// hosts lives in exactly one place. Private to src/d3d12: nothing here
// may be reachable from an include/ seam header.

#if __has_include(<wsl/winadapter.h>)
#include <wsl/winadapter.h>
#endif
#include <directx/d3d12.h>
#include <directx/dxgiformat.h>
