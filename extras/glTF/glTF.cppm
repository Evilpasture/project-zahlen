// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/glTF/glTF.cppm
export module ZHLN.glTF;

extern "C++" {
namespace ZHLN {
class Engine;
}
}

export namespace ZHLN::glTF {

// Loading a model needs no registration step. ZHLN::GLTF::LoadGLBPrefab()
// (declared in <glTF/GLTFImporter.hpp>) builds the ZHLN::ModelPrefab the ECS
// already describes and caches it; CreativeWorksFactory::LoadModelPrefab(path)
// then finds it in that cache. Core consumes the struct, the importer produces
// it, and nothing installs a callback in between.

void Initialize(ZHLN::Engine& engine);

} // namespace ZHLN::glTF
