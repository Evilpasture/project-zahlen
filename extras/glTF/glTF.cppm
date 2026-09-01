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

/// Installs the glTF/GLB importer as the engine's ZHLN::PrefabLoader backend.
///
/// Reading a model file is an extra, so core cannot call the importer directly:
/// CreativeWorksFactory goes through ZHLN::PrefabLoader, and until something
/// registers a backend its prefab entry points return null. Any application
/// that loads a .glb -- directly, through Scene::ShapeKind::Prefab, or through
/// CreativeWorksFactory::InstantiatePrefab -- calls this once at startup, next
/// to Engine creation. Initialize() calls it too, so the inspector needs no
/// separate step. Idempotent.
void RegisterPrefabLoader() noexcept;

void Initialize(ZHLN::Engine& engine);

} // namespace ZHLN::glTF
