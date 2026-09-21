// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/PresentationTarget.cpp
//
// The anchor translation unit for the presentation interface. Its destructor is
// the class's key function, so defining it here is what makes the vtable exist
// exactly once -- in this subsystem, which owns every implementation -- instead
// of weakly in every translation unit that includes the header.

#include <Zahlen/PresentationTarget.hpp>

namespace ZHLN {

IPresentationTarget::~IPresentationTarget() = default;

} // namespace ZHLN
