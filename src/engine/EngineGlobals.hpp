// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {


void InitRenderDocAPI();

void AcquireJoltRegistration();
void ReleaseJoltRegistration();

[[nodiscard]] auto AcquireGlfw() -> bool;
void               ReleaseGlfw();

}
