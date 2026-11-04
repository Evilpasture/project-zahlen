// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuAddress.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

using ZHLN::D3D::BufferAddress;
using ZHLN::VkD3D::DeviceAddress;

static_assert(sizeof(DeviceAddress<>) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<DeviceAddress<uint32_t>>);

// The joint D3D12 wire encoding reserves raw zero as null without reserving
// logical descriptor slot zero.
static_assert(DeviceAddress<uint32_t>(BufferAddress<uint32_t>(0, 0)).Raw() == 0x0000000100000000ULL);
static_assert(!DeviceAddress<uint32_t>(BufferAddress<uint32_t>{}));
static_assert(static_cast<BufferAddress<uint32_t>>(DeviceAddress<uint32_t>(BufferAddress<uint32_t>(0, 0))) ==
              BufferAddress<uint32_t>(0, 0));

// Pointer stepping changes only the byte offset. Underflow / overflow yields
// null rather than silently borrowing into or carrying into the slot dword.
static_assert((DeviceAddress<uint32_t>(BufferAddress<uint32_t>(5, 4)) + -1).Raw() == 0x0000000600000000ULL);
static_assert(!(DeviceAddress<uint32_t>(BufferAddress<uint32_t>(5, 0)) + -1));
static_assert(!(DeviceAddress<uint32_t>(BufferAddress<uint32_t>(5, std::numeric_limits<uint32_t>::max())) + 1));
