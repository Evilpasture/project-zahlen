// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vkd3d/graph/GraphImage.hpp>

#include <vkd3d/graph/PassDecl.hpp>

#include <vkd3d/graph/StateTable.hpp>

// Anchor TU for the neutral graph vocabulary and state analysis. The headers
// are fully constexpr; these checks pin the tag reflection, the usage
// vocabulary, and the state-table hazard rule on a two-pass toy graph so a
// drifted enumerator or a broken simulation fails here instead of inside the
// renderer's template instantiation.

namespace {

using namespace ZHLN::VkD3D;

using ToyColor = GraphImage<"ToyColor", Format::R16G16B16A16Sfloat>;
using ToyDepth = GraphImage<"ToyDepth", Format::D32SfloatS8Uint>;
using ToyMips  = GraphImage<"ToyMips", Format::R8G8B8A8Unorm, ImageAspect::Color, false, false, 2, false, true>;

struct WriteToy: RenderPass<"WriteToy", ColorWrite<ToyColor>, DepthWrite<ToyDepth>> {};
struct ReadToy: RenderPass<"ReadToy", ShaderRead<ToyColor>> {};

static_assert(ToyColor::name.string_view() == "ToyColor");
static_assert(ToyColor::format == Format::R16G16B16A16Sfloat);
static_assert(GetFormatAspect(Format::D32SfloatS8Uint) == (ImageAspect::Depth | ImageAspect::Stencil));
static_assert(GetFormatAspect(Format::R8G8B8A8Unorm) == ImageAspect::Color);
static_assert(ToyDepth::aspect == (ImageAspect::Depth | ImageAspect::Stencil));
static_assert(ToyMips::scale_divisor == 2 && ToyMips::is_mipmapped);

static_assert(ColorAttachmentWriteMask<WriteToy, ToyColor>() == (ColorMask::Red | ColorMask::Green | ColorMask::Blue | ColorMask::Alpha));
static_assert(FrameGraphPass<WriteToy>);
static_assert(TypeList<>::size == 0);

using ToyResources = TypeList<ToyColor, ToyDepth>;
static_assert(TemplatedDetail::GetResourceIndex<ToyResources, ToyColor>() == 0);
static_assert(TemplatedDetail::GetResourceIndex<ToyResources, ToyDepth>() == 1);

constexpr auto kToyTable = TemplatedDetail::ComputeStateTable<ToyResources, WriteToy, ReadToy>();

// The table records each pass's entry state: both targets enter the writer
// undefined (they are written this frame), and the color enters the reader
// still in its declared final layout -- so the reader needs a layout barrier
// but no write hazard.
static_assert(kToyTable[0][0].layout == ImageLayout::Undefined);
static_assert(kToyTable[1][0].layout == ImageLayout::ColorAttachmentOptimal);
static_assert(kToyTable[1][0].lastWritePass == 0);
static_assert(TemplatedDetail::NeedsBarrier<kToyTable[1][0], ShaderRead<ToyColor>, 1>::value);
// An undefined entry state always transitions (discard semantics).
static_assert(TemplatedDetail::NeedsBarrier<kToyTable[0][0], ShaderRead<ToyColor>, 0>::value);

} // namespace
