// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestConsoleDebugger.cpp
//
// ConsoleDebugger is an extras/Console command dispatcher. It talks to
// ScriptBinder / ScriptECSBridge, so the suite lives here rather than in
// tests/core and is built only when ZHLN_BUILD_EXTRAS is on.

#include "TestsFramework.hpp"
#include <Console/Console.hpp>
#include <Console/ConsoleDebugger.hpp>
#include <Scripting/ScriptBinder.hpp>
#include <Scripting/ScriptBinderRegistry.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace {

auto JoinedLogs(const ZHLN::GameConsole& console) -> std::string {
    std::string out;
    for (size_t i = 0; i < console.GetEntryCount(); ++i) {
        std::string_view text;
        float            r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
        console.GetEntry(i, text, r, g, b, a);
        if (!out.empty()) {
            out += '\n';
        }
        out += text;
    }
    return out;
}

[[nodiscard]] auto Contains(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

struct ConsoleDebuggerTestSuite {
    ConsoleDebuggerTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
        ZHLN::RegisterCoreScriptTypes();
    }

    ~ConsoleDebuggerTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> help_lists_commands() {
            ZHLN::ECS::Registry  reg;
            ZHLN::GameConsole    console;
            ZHLN::ConsoleDebugger::Execute(reg, console, "help");
            const std::string logs = JoinedLogs(console);
            ZHLN::Test::ExpectTrue(Contains(logs, "Zahlen Debugger Commands"));
            ZHLN::Test::ExpectTrue(Contains(logs, "types"));
            ZHLN::Test::ExpectTrue(Contains(logs, "entity"));
            ZHLN::Test::ExpectTrue(Contains(logs, "find"));

            ZHLN::GameConsole alias;
            ZHLN::ConsoleDebugger::Execute(reg, alias, "?");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(alias), "Zahlen Debugger Commands"));
            return {};
        }

        std::expected<void, ZHLN::Error> empty_line_is_a_no_op() {
            ZHLN::ECS::Registry  reg;
            ZHLN::GameConsole    console;
            ZHLN::ConsoleDebugger::Execute(reg, console, "   ");
            ZHLN::Test::ExpectEq(console.GetEntryCount(), static_cast<size_t>(0));
            return {};
        }

        std::expected<void, ZHLN::Error> unknown_command_reports_help_hint() {
            ZHLN::ECS::Registry  reg;
            ZHLN::GameConsole    console;
            ZHLN::ConsoleDebugger::Execute(reg, console, "explode");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(console), "Unknown command: 'explode'"));
            return {};
        }

        std::expected<void, ZHLN::Error> types_lists_registered_components() {
            ZHLN::ECS::Registry  reg;
            ZHLN::GameConsole    console;
            ZHLN::ConsoleDebugger::Execute(reg, console, "types");
            const std::string logs = JoinedLogs(console);
            ZHLN::Test::ExpectTrue(Contains(logs, "Registered Reflected Types"));
            ZHLN::Test::ExpectTrue(Contains(logs, "PBRComponent"));
            ZHLN::Test::ExpectTrue(Contains(logs, "NameComponent"));
            return {};
        }

        std::expected<void, ZHLN::Error> type_dumps_fields_or_reports_missing() {
            ZHLN::ECS::Registry  reg;
            ZHLN::GameConsole    console;
            ZHLN::ConsoleDebugger::Execute(reg, console, "type");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(console), "Usage: type <TypeName>"));

            ZHLN::GameConsole missing;
            ZHLN::ConsoleDebugger::Execute(reg, missing, "type NotAComponent");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(missing), "not found"));

            ZHLN::GameConsole pbr;
            ZHLN::ConsoleDebugger::Execute(reg, pbr, "type PBRComponent");
            const std::string logs = JoinedLogs(pbr);
            ZHLN::Test::ExpectTrue(Contains(logs, "Type 'PBRComponent'"));
            ZHLN::Test::ExpectTrue(Contains(logs, ".roughness"));
            ZHLN::Test::ExpectTrue(Contains(logs, ".metallic"));
            return {};
        }

        std::expected<void, ZHLN::Error> find_entity_and_inspect() {
            ZHLN::ECS::Registry reg;
            const ZHLN::Entity  hero = reg.Create();
            reg.Add(hero, ZHLN::Components::NameComponent {.name = ZHLN::String64 {"Hero"}});
            reg.Add(hero, ZHLN::Components::PBRComponent {.roughness = 0.4f, .metallic = 0.2f});

            ZHLN::GameConsole findConsole;
            ZHLN::ConsoleDebugger::Execute(reg, findConsole, "find Hero");
            const std::string found = JoinedLogs(findConsole);
            ZHLN::Test::ExpectTrue(Contains(found, "Found Entity: Hero"));
            ZHLN::Test::ExpectTrue(Contains(found, std::format("ID: {}", hero.Pack())));

            ZHLN::GameConsole quoted;
            ZHLN::ConsoleDebugger::Execute(reg, quoted, R"(find "Hero")");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(quoted), "Found Entity: Hero"));

            ZHLN::GameConsole missing;
            ZHLN::ConsoleDebugger::Execute(reg, missing, "find Nobody");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(missing), "No entity named 'Nobody'"));

            ZHLN::GameConsole inspect;
            ZHLN::ConsoleDebugger::Execute(reg, inspect, std::format("entity {}", hero.Pack()));
            const std::string dump = JoinedLogs(inspect);
            ZHLN::Test::ExpectTrue(Contains(dump, std::format("Entity {}:", hero.Pack())));

            ZHLN::GameConsole dead;
            const uint64_t    packed = hero.Pack();
            reg.Destroy(hero);
            ZHLN::ConsoleDebugger::Execute(reg, dead, std::format("entity {}", packed));
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(dead), "Invalid or dead entity"));
            return {};
        }

        std::expected<void, ZHLN::Error> get_and_set_component_fields() {
            ZHLN::ECS::Registry reg;
            // get/set resolve the component by name through
            // Registry::GetFamilyIDFromName, which is filled by RegisterComponent
            // (Engine::InitializeDefaultScene does RegisterAllComponentsIn).
            // A bare Add() stores the component but does not map the name, so
            // ScriptECSBridge would report ComponentNotFound.
            reg.RegisterAllComponentsIn<ZHLN::Components>();
            const ZHLN::Entity e = reg.Create();
            reg.Add(e, ZHLN::Components::PBRComponent {.roughness = 0.5f, .metallic = 0.0f});
            const std::string id = std::format("{}", e.Pack());

            ZHLN::GameConsole getConsole;
            ZHLN::ConsoleDebugger::Execute(reg, getConsole, std::format("get {} PBRComponent.roughness", id));
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(getConsole), "PBRComponent.roughness = 0.5"));

            ZHLN::GameConsole setConsole;
            ZHLN::ConsoleDebugger::Execute(reg, setConsole, std::format("set {} PBRComponent.roughness 0.25", id));
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(setConsole), "Updated PBRComponent.roughness to 0.25"));
            const auto* pbr = reg.Get<ZHLN::Components::PBRComponent>(e);
            ZHLN::Test::ExpectTrue(pbr != nullptr);
            if (pbr != nullptr) {
                ZHLN::Test::ExpectEq(pbr->roughness, 0.25f);
            }

            ZHLN::GameConsole usage;
            ZHLN::ConsoleDebugger::Execute(reg, usage, "get");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(usage), "Usage: get"));

            ZHLN::GameConsole badField;
            ZHLN::ConsoleDebugger::Execute(reg, badField, std::format("get {} PBRComponent", id));
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(badField), "<Component>.<field>"));
            return {};
        }

        std::expected<void, ZHLN::Error> sym_rejects_bad_addresses() {
            ZHLN::ECS::Registry  reg;
            ZHLN::GameConsole    usage;
            ZHLN::ConsoleDebugger::Execute(reg, usage, "sym");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(usage), "Usage: sym"));

            ZHLN::GameConsole bad;
            ZHLN::ConsoleDebugger::Execute(reg, bad, "sym not-hex");
            ZHLN::Test::ExpectTrue(Contains(JoinedLogs(bad), "Invalid address format"));
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ConsoleDebuggerTestSuite>();
}
