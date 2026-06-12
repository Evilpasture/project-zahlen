// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "RenderCore.hpp"

// Returns a fully managed, RAII Context ready for headless testing.
ZHLN::Vk::Context MakeHeadlessCtx();