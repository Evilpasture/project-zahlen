// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SourceVFS.hpp"
#include "VMTParser.hpp"
#include <Zahlen/Render/Types.hpp>
namespace ZHLN::BSP {
// ---------------------------------------------------------------------------
// The .vmt lighting rules
// ---------------------------------------------------------------------------
//
// A `.vmt` has one appearance, and both material passes have to agree on it:
// the brush and StudioModel passes assemble different materials, and a map routinely puts the same material on both (Source's
// `light_wall` lamp panel is a prop, the glow sheet under the same fixture is
// a brush). Source states that appearance in two places, and a pass that reads
// only one of them renders the material dark:
//
//   1. `$selfillum` / `$selfillummask` / `$selfillummaskscale` /
//      `$emissiveblendstrength` -- the authored self-illumination.
//   2. The shader class, plus the measured luminance of `$basetexture` for the
//      materials that author neither (a bright lamp panel with no keyvalues
//      still has to glow).
//
// `strength` is 0..1 linear and already scaled by the shader class, and `tint`
// is the chromaticity the emission is emitted in -- a red neon sign emits red,
// not a grey scaled by red.
struct VmtEmission {
    float     strength = 0.0f;
    FVec3 tint {1.0f, 1.0f, 1.0f};
};

// Reads the rules above out of one parsed material. The base texture luminance
// needs the VFS (it decodes the .vtf), which is the only reason this is not a
// pure function of the keyvalues.
[[nodiscard]] auto ResolveVmtEmission(const SourceVFS& vfs, const VMTMaterial& vmt) -> VmtEmission;

// Applies the rules above to a material description: first the shader classes
// that are unlit by construction, then the emission term. `desc.baseColor` and
// the texture maps are expected to be filled in already.
//
// Exposed (rather than file-local to the world pass) so the StudioModel pass
// -- and a test -- reach the same answer: the lamp props in a shipped map are
// `VertexLitGeneric` + `$selfillum 1`, and they stayed dark only because the
// prop pass never read those keyvalues.
auto ApplyVmtLighting(const SourceVFS& vfs, const VMTMaterial& vmt, MaterialDesc& desc) -> void;

// Negative when unavailable; shared by material import and emissive ranking.
[[nodiscard]] auto MeasureVmtTextureLuma(const SourceVFS& vfs, const VMTMaterial& vmt) -> float;
} // namespace ZHLN::BSP
