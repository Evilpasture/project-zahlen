// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <d3d12/memory/BufferAddress.hpp>

#include <cstdint>
#include <type_traits>

// Keep the address contract compiled as part of the native D3D12 target even
// while the backend's device/resource implementation is being brought up.
static_assert(sizeof(ZHLN::D3D::BufferAddress<>) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<ZHLN::D3D::BufferAddress<uint32_t>>);
