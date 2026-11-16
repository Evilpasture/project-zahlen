// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace ZHLN { class Engine; namespace COM {
// Internal narrow operations, never a World/registry escape hatch.
struct JointAllocator { static uint32_t AllocateJoints(Engine&, uint32_t) noexcept; };
}}
