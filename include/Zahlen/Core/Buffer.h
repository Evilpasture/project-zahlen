/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ZHLN_BufferFlags : uint8_t {
    ZHLN_BUFFER_CONTIGUOUS = 1 << 0,
    ZHLN_BUFFER_ALIGNED_16 = 1 << 1,
    ZHLN_BUFFER_ALIGNED_32 = 1 << 2,
    ZHLN_BUFFER_WRITABLE   = 1 << 3,
} ZHLN_BufferFlags;

typedef enum ZHLN_OwnerType : uint8_t { ZHLN_OWNER_NONE = 0, ZHLN_OWNER_PHYSICS_WORLD = 1, ZHLN_OWNER_ECS_REGISTRY = 2 } ZHLN_OwnerType;

typedef struct ZHLN_BufferView {
    void*    buf;
    void*    obj;
    size_t   len;
    uint32_t itemsize;

    char format[8];
    int  readonly;

    uint32_t ndim;
    size_t   shape[4];
    size_t   strides[4];

    uint32_t flags;
    uint32_t owner_type;
} ZHLN_BufferView;

#ifdef __cplusplus
}

#include <type_traits>
namespace ZHLN {
using BufferView = ::ZHLN_BufferView;
static_assert(isDebug || (std::is_trivially_default_constructible_v<BufferView> && std::is_trivially_copyable_v<BufferView>) );
}
#endif
