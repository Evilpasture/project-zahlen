// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <Console/ConsoleDebugger.hpp>
#include <Scene/Scene.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Scripting/ScriptECSBridge.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/HostUI.hpp>
#include <format>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto Logs(const GameConsole& console) {
    std::string result;
    for (size_t i = 0; i < console.GetEntryCount(); ++i) {
        const auto entry = console.GetEntry(i);
        if (!entry) continue;
        const std::string_view text = entry->text;
        [[maybe_unused]] const float r = entry->color.r, g = entry->color.g, b = entry->color.b, a = entry->color.a;
        result += text;
        result += '\n';
    }
    return result;
}
struct ConsoleHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> real_scene_console_reads_edits_rejects_raw_native_escape_and_renders() {
            auto made = COM::CreateEngineRef(
                COM::InChain(
                    COM::WindowConfig {.width = 640, .height = 360, .vsync = false, .headless = true},
                    COM::RenderConfig {.validationMode = ValidationMode::Off}
                )
            );
            if (!made)
                return std::unexpected(made.error());
            auto         engine = std::move(*made);
            Scene::Document scene;
            scene.camera.position       = {0, 0, 8};
            scene.environment.enableRTR = false;
            Scene::Entity box;
            box.name = "ConsoleTarget";
            box.body = Scene::BodyKind::Dynamic;
            scene.entities.push_back(box);
            if (auto built = Scene::Instantiate(*engine.Get(), scene, {.buildBLAS = false}); !built)
                return std::unexpected(built.error());
            auto   names  = engine->Entities()->EntitiesWith<Components::NameComponent>();
            Entity target = Entity::Null();
            for (auto e: names)
                if (std::string_view(engine->Entities()->Get<Components::NameComponent>(e)->name) == "ConsoleTarget")
                    target = e;
            if (!ExpectTrue(target != Entity::Null()))
                return {};
            GameConsole console;
            ConsoleDebugger::Execute(*engine.Get(), console, "find ConsoleTarget");
            ExpectTrue(Logs(console).find("Found Entity: ConsoleTarget") != std::string::npos);
            engine->Entities()->Attach(target, Components::PBRComponent {.roughness = .5f});
            ConsoleDebugger::Execute(*engine.Get(), console, std::format("set {} PBRComponent.roughness 0.25", target.Pack()));
            ExpectEq(engine->Entities()->Get<Components::PBRComponent>(target)->roughness, .25f);
            ScriptECSBridge bridge(*engine->Entities());
            auto            read = bridge.GetProperty(target, "PBRComponent", "roughness");
            ExpectTrue(read.has_value());
            const auto canonical = COM::IEntities::ComponentName<Components::PBRComponent>();
            ExpectTrue(engine->Entities()->GetRaw(target, canonical) == nullptr);
            ExpectTrue(engine->Entities()->GetRaw(target, "PBRComponent") == nullptr);
            ExpectTrue(engine->Entities()->Get<Components::PBRComponent>(target) != nullptr);
            const auto body = engine->Entities()->Get<Components::PhysicsComponent>(target)->physicsID;
            ExpectTrue(!bridge.SetProperty(target, "PhysicsComponent", "isStatic", ScriptVal {true}));
            ExpectTrue(engine->Simulation()->BodyPosition(body).HasValue());
            HostUI host(*engine.Get());
            host.BeginFrame();
            auto& context = host.Context();
            auto  ui      = context.BeginFrame(host.ConsumeInput(1.f / 60));
            ui.Text(Logs(console));
            ExpectTrue(host.Submit(std::move(ui).End()).HasValue());
            ExpectTrue(engine->Tick(1.f / 60).HasValue());
            Screenshot::CaptureBuffer capture;
            auto                      frame = capture.Read(*engine->Graphics(), engine->WindowExtent(engine->PrimaryWindow()));
            ExpectTrue(frame.has_value());
            if (frame)
                ExpectTrue((*frame)->metrics.litPixels > 0);
            engine->Entities()->Despawn(target);
            ExpectTrue(engine->Tick(1.f / 60).HasValue());
            ExpectTrue(!bridge.GetProperty(target, "PBRComponent", "roughness"));
            return {};
        }
    };
};
} // namespace
