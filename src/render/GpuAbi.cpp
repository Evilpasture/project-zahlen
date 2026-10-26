// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GpuAbi.cpp
//
// The scene push layout, exported. Every number in it -- where each frame
// address sits inside DescriptorHeapPushData, the descriptor-index word after
// them, the furthest byte a push writes -- was read off the gpu_abi module's
// reflection by tools/zshader and written into GeneratedGpuTypes.hpp, so this
// translation unit only gives the generated constant the external name
// src/render/GpuAbi.hpp declares. There is nothing to check here: the header
// it comes from is the ABI, and re-parsing the module's SPIR-V to confirm it
// would be confirming Slang against Slang.

#include "GpuAbi.hpp"
#include "GpuLayout.hpp"

namespace ZHLN::GpuAbi {

const Vk::HeapPushDataLayout kScenePushLayout = Slang::kScenePushLayout;

} // namespace ZHLN::GpuAbi
