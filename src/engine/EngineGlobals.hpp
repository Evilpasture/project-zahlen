// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {

/// Process-global GLFW/Jolt registration and the optional RenderDoc bind.
/// First in registers, last out tears down; not Engine instance state.

void InitRenderDocAPI();

void AcquireJoltRegistration();
void ReleaseJoltRegistration();

[[nodiscard]] auto AcquireGlfw() -> bool;
void               ReleaseGlfw();

} // namespace ZHLN
