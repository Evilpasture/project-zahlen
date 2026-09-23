// tools/zcook/Ninja.hpp
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
//               [--self <zcook executable>]
//
//   --out           the ninja file to write (kept where the .ninja_log lives)
//   --source        the asset root to scan: <root>/blender for .blend sources,
//                   <root>/resources/intermediate for what Blender exported,
//                   <root>/resources/assets for loose textures and models
//   --engine-tools  the engine's tools/ directory: the Blender-side exporter
//                   (export_metadata.py) and its launcher (run_blender.py) are
//                   Python by necessity -- they run inside Blender -- and the
//                   generated rules must name them at the path the build knows
//   --self          how the generated file re-invokes this generator; defaults
//                   to argv[0], which is what the build passes. Keep it the
//                   stable path (the shared symlink) rather than a build-dir
//                   path: the file it writes is shared between build trees.
int GenerateAssetNinja(int argc, char** argv);

} // namespace ZHLN
