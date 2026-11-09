// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/COMTypes.hpp
//
// The vocabulary the public API speaks. Everything here is standard layout
// and trivially copyable, so it travels as plain bytes in registers or on the
// stack and never asks the two sides to agree on a container layout, an
// allocator or an exception table.
//
// The engine's own fixed-layout value types (Extent2D, BufferID, Entity, ...)
// are reused rather than mirrored. They already satisfy the ABI rules, and a
// parallel set of structs would be two things to keep in step instead of one.
// What stops at this line is the standard library, not the engine's vocabulary.

#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/Types.hpp>

#include <cstdint>
#include <type_traits>

namespace ZHLN::COM {

// Failures the boundary itself can produce, as opposed to the ones the
// engine reports through its own ErrorCode categories.
enum class ComError : uint8_t {
    AllocationFailed = 1,
    // Nothing is cached under that asset path. Importing happens in a plugin
    // above this library, so the boundary can report the miss but cannot fix
    // it by loading.
    AssetNotCached = 2,
};

// The engine vocabulary this boundary hands across, checked rather than
// assumed. A type that stops satisfying these rules breaks the boundary, and it
// should say so here and not in a consumer's crash report.
static_assert(std::is_standard_layout_v<Extent2D> && std::is_trivially_copyable_v<Extent2D>);
static_assert(std::is_standard_layout_v<ViewportRect> && std::is_trivially_copyable_v<ViewportRect>);
static_assert(std::is_standard_layout_v<Entity> && std::is_trivially_copyable_v<Entity>);
static_assert(std::is_standard_layout_v<BufferID> && std::is_trivially_copyable_v<BufferID>);
static_assert(std::is_standard_layout_v<TextureID> && std::is_trivially_copyable_v<TextureID>);
static_assert(std::is_standard_layout_v<BufferUsage> && std::is_trivially_copyable_v<BufferUsage>);

// Pipeline configuration crosses by value. These are dumb data containers --
// no preset logic, no detection, no policy -- so their layout is stable and
// a host can read, edit and write one back without touching the registry.
static_assert(std::is_standard_layout_v<GraphicsSettings> && std::is_trivially_copyable_v<GraphicsSettings>);
static_assert(std::is_standard_layout_v<GISettings> && std::is_trivially_copyable_v<GISettings>);
static_assert(std::is_standard_layout_v<ShadowSettings> && std::is_trivially_copyable_v<ShadowSettings>);
static_assert(std::is_standard_layout_v<RayTracingConfig> && std::is_trivially_copyable_v<RayTracingConfig>);
static_assert(std::is_standard_layout_v<EnvironmentSettings> && std::is_trivially_copyable_v<EnvironmentSettings>);
static_assert(std::is_standard_layout_v<AAState> && std::is_trivially_copyable_v<AAState>);

} // namespace ZHLN::COM
