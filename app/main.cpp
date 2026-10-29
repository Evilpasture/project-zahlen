// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Ordinary game: composition, launch, events, and frame pacing belong to App.
#include <Zahlen/App.hpp>
#include <Zahlen/PrefabFactory.hpp>

int main(int argc, char* argv[]) {
    return ZHLN::App()
        .OnStart([](ZHLN::Engine& engine) {
            ZHLN::PrefabFactory::SpawnParams spawn;
            spawn.position = JPH::RVec3(0.0, 2.0, 0.0);
            ZHLN::PrefabFactory::CreateBox(engine, JPH::Vec3::sReplicate(1.0f), spawn);
        })
        .Run(argc, argv, [](ZHLN::Engine& engine, float dt) {
            // Add gameplay here; App handles input, Tick, rendering and teardown.
            (void) engine;
            (void) dt;
        });
}
