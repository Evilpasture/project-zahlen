// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
namespace ZHLN::COM { struct IEngine; }
// Standalone consumer's lifecycle/capture helper. All dependencies are installed.
[[nodiscard]] bool RunInstalledCubeSmoke(void (*createScene)(ZHLN::COM::IEngine&), const char* ppmPath);
