// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/App.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>

int main(int argc, char* argv[]) {
    return ZHLN::App()
        .Title("Zahlen COM Cube")
        .OnStart([](ZHLN::COM::IEngine& e) { e.Assets()->SpawnBox({1, 1, 1}); })
        .Main(argc, argv);
}
