// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/TargetCameraFixture.hpp
//
// The third-person target camera rig is an extras module (extras/Camera). The
// headless fixture builds a bare core engine, so a suite that needs the rig
// installs it through this helper instead of reaching for the module itself.
//
// The install is safe on a pooled engine: CameraRig::Install is idempotent at
// the seam (its frame step deduplicates on every schedule rebuild). The scene
// rebuild below is what actually applies the extension -- the engine's frame
// scheduler was built before the extension existed, and only a scene rebuild
// re-runs the extension list over the fresh schedule.

#pragma once

#include "helpers/HeadlessEngineFixture.hpp"
#include <Camera/TargetCamera.hpp>

namespace ZHLN::Test::Headless {

// Installs the extras target camera rig on `engine` and rebuilds the scene so
// the rig's frame step lands before the core "CameraSystems" step. Call right
// after AcquireEngine, before building the scene: the rebuild clears entities
// the way ResetScene does.
inline void InstallTargetCameraRig(ZHLN::Engine& engine) {
    ZHLN::CameraRig::Install(engine);

    engine.GetRegistry().Clear();
    engine.InitializeDefaultScene();

    // The camera is engine state, not an entity, so Clear does not touch it;
    // reset it the way ResetScene does so the rig starts from the authored
    // framing regardless of what an earlier suite left behind.
    engine.GetCamera() = ZHLN::Camera {};
}

} // namespace ZHLN::Test::Headless
