// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/Host.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <cstddef>

int main(int argc, char* argv[]) {
    using namespace ZHLN::COM;
    return RunApp(argc, argv, App().Title("Zahlen COM Cube").OnStart([](IEngine& e) { e.Assets()->SpawnBox({1, 1, 1}); }));
}
