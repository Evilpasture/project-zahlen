// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Config.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Engine/Capabilities/HostWindow.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/GUI/UIData.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>

namespace ZHLN::Test {
namespace {
int graphRegistrationCalls = 0;
void GraphLifecycleProbeSystem() {}
void AddGraphLifecycleProbe(ECS::SimGraph& updateGraph, ECS::RenderGraph&) {
    ++graphRegistrationCalls;
    updateGraph.AddSystem<&GraphLifecycleProbeSystem>();
}
} // namespace

// Like TestCOMBoundary, this exercises the headless engine through public
// headers in the existing worker. No GPU frame is submitted.
void test_module_install_compiles_graphs_before_return_and_getters_are_observers() {
    TaskSystem::Scope tasks(2);
    EngineConfig config {};
    config.appName = "EagerGraphLifecycleTest";
    config.width = 32;
    config.height = 32;
    config.validationMode = ValidationMode::Off;
    config.headless = true;
    config.enableMeshShading = false;
    config.physics = {.maxBodies = 100, .maxBodyPairs = 200, .maxContactConstraints = 200,
                      .tempAllocatorSize = 8 * 1024 * 1024};
    auto created = Engine::Create(config);
    if (!ExpectTrue(created.has_value())) {
        return;
    }
    auto& engine = **created;
    auto& update = engine.GetUpdateGraph();
    auto& render = engine.GetRenderGraph();
    const auto initialCount = update.GetSystemCount();
    ExpectTrue(initialCount > 0);
    // Headless primary identity remains addressable without an OS Window.
    const auto ids = engine.WindowIDs();
    ExpectEq(ids.size(), size_t {1});
    ExpectTrue(ids.front() == WindowID::Primary);
    ExpectFalse(engine.Host().Present());
    ExpectTrue(engine.Host().Running());
    ExpectTrue(engine.AddWindow("unavailable", 32, 32, false) == WindowID::Invalid);
    const auto absent = WindowID {9999};
    ExpectFalse(engine.Host(absent).Running());
    ExpectEq(engine.Host(absent).Extent().width, uint32_t {0});
    const auto unavailable = engine.AcquireTarget(absent);
    ExpectTrue(unavailable.has_value() && !unavailable->has_value());
    ExpectFalse(engine.GetAcquiredTarget(absent).has_value());
    engine.RemoveWindow(absent);
    engine.RemoveWindow(WindowID::Primary); // removal never destroys the primary host
    ExpectTrue(engine.Host().Running());
    ExpectEq(engine.WindowIDs().size(), size_t {1});

    auto graphics = engine.Graphics();
    {
        const UIBatch batch {.vertexCount = 3};
        const VertexPosition positions[3] {};
        const VertexSurface surfaces[3] {};
        graphics.SubmitUI(WindowID::Primary, UIDrawData {.batches = {&batch, 1}, .positions = positions, .surfaces = surfaces});
    }
    ExpectTrue(graphics.HasSubmittedUI(WindowID::Primary));
    graphics.ClearSubmittedUI();
    ExpectFalse(graphics.HasSubmittedUI(WindowID::Primary));

    // Scene-local debug policy resets along with culling and graph locals.
    engine.State().SetPhysicsDrawMode(2);
    engine.State().SetFreezeFrustum(true);
    engine.ClearScene();
    ExpectEq(engine.State().PhysicsDrawMode(), 0);
    ExpectFalse(engine.State().CullingStats().FreezeFrustum);

    graphRegistrationCalls = 0;
    const Module probe {
        .name            = "EagerGraphRegistrationProbe",
        .addGraphSystems = &AddGraphLifecycleProbe,
    };
    ExpectTrue(engine.InstallModule(probe).has_value());
    // Inspect the previously held reference *before* invoking any getter.
    ExpectEq(graphRegistrationCalls, 1);
    ExpectEq(update.GetSystemCount(), initialCount + 1);
    for (int i = 0; i < 3; ++i) {
        ExpectTrue(&engine.GetUpdateGraph() == &update);
        ExpectTrue(&engine.GetRenderGraph() == &render);
    }
    ExpectEq(graphRegistrationCalls, 1);
    ExpectTrue(engine.InstallModule(probe).has_value());
    ExpectEq(graphRegistrationCalls, 1); // duplicate installation is inert
    ExpectEq(update.GetSystemCount(), initialCount + 1);
}
} // namespace ZHLN::Test
