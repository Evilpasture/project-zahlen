// utils/zcook/Ninja.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {

// zcook ninja -- scan an asset tree and write the ninja graph that cooks it.
//
// The generator lives here, in the cooker, because the graph it writes is made
// of zcook's own command lines: the rules name the subcommands this binary
// implements ("mesh", "anim", "tex", "glb", "pak"), the inputs are the
// intermediate files it will be asked to read, and the metadata it walks to
// discover meshes and animations is the same manifest -- same structs, same
// parser (BinaryReader/IRManifest) -- that those subcommands consume. A
// generator in another language could drift from the tool without either side
// noticing; this one cannot describe a job zcook does not have.
//
// Usage:
//   zcook ninja --out <assets.ninja> --source <asset-root> --engine-tools <dir>
//               [--self <zcook executable>] [--blender <executable> | --no-blender]
//
//   --out           the ninja file to write (kept where the .ninja_log lives)
//   --source        the asset root to scan: <root>/blender for .blend sources,
//                   <root>/resources/intermediate for what Blender exported,
//                   <root>/resources/assets for loose textures and models
//   --engine-tools  the directory holding the Blender-side exporter
//                   (scripts/export_metadata.py): it runs inside Blender's
//                   bundled Python, launched directly via `zcook blender` with
//                   no host Python dependency
//   --self          how the generated file re-invokes this generator; defaults
//                   to argv[0], which is what the build passes. Keep it the
//                   stable path (the shared symlink) rather than a build-dir
//                   path: the file it writes is shared between build trees.
//   --blender       the Blender executable the extraction rule launches, so the
//                   cook cannot resolve a different one than configure did.
//                   cmake/BlenderProbe.cmake is where the build gets it.
//   --no-blender    write a graph with no blender_extract edge at all: .blend
//                   sources are skipped and everything else still cooks. Blender
//                   is optional -- cmake/BlenderProbe.cmake passes this when the
//                   host has none, or when ZHLN_HAS_BLENDER=OFF.
int GenerateAssetNinja(int argc, char** argv);

// zcook blender -- locate Blender, scrub host Python environment variables so
// virtual environments never leak into Blender's bundled interpreter, and invoke
// Blender directly without requiring a system Python installation.
int RunBlender(int argc, char** argv);

} // namespace ZHLN
