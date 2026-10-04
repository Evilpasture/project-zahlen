// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

struct lua_State;

namespace ZHLN::ProjectLight {

class DataModel;

// Installs the project-light compatibility surface into an existing LuaJIT
// state. The DataModel must outlive that state; Instances remain the canonical
// world graph and Lua userdata are identity-preserving handles into it.
void RegisterDataModelLuaBindings(lua_State* state, DataModel& dataModel);

} // namespace ZHLN::ProjectLight
