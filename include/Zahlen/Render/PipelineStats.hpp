// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN {

struct GpuPipelineCounters {
    uint64_t iaPrimitives         = 0;
    uint64_t vsInvocations        = 0;
    uint64_t clipperInvocations   = 0;
    uint64_t clipperPrimitivesOut = 0;
    uint64_t gsInvocations        = 0;
    uint64_t gsPrimitives         = 0;
    uint64_t fsInvocations        = 0;
    uint64_t csInvocations        = 0;
    uint64_t taskInvocations      = 0;
    uint64_t meshInvocations      = 0;
};

}
