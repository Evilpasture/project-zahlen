// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConsoleDebugger.hpp"
#include <format>
#include "SymbolResolver.hpp"
#include <Console/Console.hpp>
#include <Scripting/ScriptBinder.hpp>
#include <Scripting/ScriptBinderRegistry.hpp>
#include <Scripting/ScriptECSBridge.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>

#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace ZHLN {

namespace {

    auto Tokenize(std::string_view str) -> std::vector<std::string> {
        std::vector<std::string> tokens;
        std::string              token;
        bool                     inQuotes = false;

        for (char c: str) {
            if (c == '"') {
                inQuotes = !inQuotes;
            } else if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes) {
                if (!token.empty()) {
                    tokens.push_back(std::move(token));
                    token.clear();
                }
            } else {
                token += c;
            }
        }
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
        return tokens;
    }

    auto ParseEntity(std::string_view sv) -> std::optional<Entity> {
        uint64_t raw   = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), raw);
        if (ec == std::errc() && ptr == sv.data() + sv.size()) {
            return Entity::Unpack(raw);
        }
        return std::nullopt;
    }

    auto ParseAddress(std::string_view sv) -> std::optional<uintptr_t> {
        if (sv.starts_with("0x") || sv.starts_with("0X")) {
            sv.remove_prefix(2);
        }
        uintptr_t addr = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), addr, 16);
        if (ec == std::errc() && ptr == sv.data() + sv.size()) {
            return addr;
        }
        return std::nullopt;
    }



} // namespace

void ConsoleDebugger::Execute(COM::IEngine& engine, GameConsole& console, std::string_view commandLine) {
    Execute(*engine.Entities(), console, commandLine);
}

void ConsoleDebugger::Execute(COM::IEntities& entities, GameConsole& console, std::string_view commandLine) {
    RegisterCoreScriptTypes();
    ScriptComponentAccess access(entities);
    access.RegisterManifest<Components>(true);
    Execute(std::move(access), console, commandLine);
}

void ConsoleDebugger::Execute(ScriptComponentAccess inspection, GameConsole& console, std::string_view commandLine) {
    auto tokens = Tokenize(commandLine);
    if (tokens.empty()) {
        return;
    }

    std::string_view cmd = tokens[0];
    ScriptECSBridge  bridge(inspection);

    // ========================================================================
    // 1. HELP
    // ========================================================================
    if (cmd == "help" || cmd == "?") {
        console.Log("=== Zahlen Debugger Commands ===", {.r = 0.3f, .g = 0.85f, .b = 1.0f, .a = 1.0f});
        console.Log("  types                         - List all registered C++ reflection types");
        console.Log("  type <TypeName>               - Dump fields and memory layout for a C++ type");
        console.Log("  entity <id>                   - Inspect all active components on an entity");
        console.Log("  get <id> <Comp>.<field>       - Read a component field value");
        console.Log("  set <id> <Comp>.<field> <v>   - Modify a single component field value");
        console.Log("  patch <id> <Comp> { k = v }   - Lua table patching lives in the Lua plugin, not Console");
        console.Log("  sym <address>                 - Resolve raw hex pointer to demangled C++ symbol");
        console.Log("  find <Name>                   - Find entity with NameComponent");
        return;
    }

    // ========================================================================
    // 2. TYPES
    // ========================================================================
    if (cmd == "types") {
        const auto& classes = ScriptBinder::Get().classes;
        console.Log(std::format("Registered Reflected Types ({}) :", classes.size()), {.r = 0.3f, .g = 0.85f, .b = 1.0f, .a = 1.0f});
        for (const auto& [name, info]: classes) {
            console.Log(
                std::format("  {} (size: {}B, align: {}B, props: {})", name, info.size, info.alignment, info.properties.size())
            );
        }
        return;
    }

    // ========================================================================
    // 3. TYPE <TypeName> (Reflected Memory Layout)
    // ========================================================================
    if (cmd == "type") {
        if (tokens.size() < 2) {
            console.Log("Usage: type <TypeName>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }
        const auto& classes = ScriptBinder::Get().classes;
        auto        it      = classes.find(tokens[1]);
        if (it == classes.end()) {
            console.Log(std::format("Type '{}' not found in reflection registry.", tokens[1]), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        const auto& info = it->second;
        console.Log(std::format("Type '{}' [Size: {}B, Align: {}B]:", info.name, info.size, info.alignment), {.r = 0.3f, .g = 0.85f, .b = 1.0f, .a = 1.0f});

        console.Log("  Properties:");
        for (const auto& [propName, prop]: info.properties) {
            console.Log(std::format("    .{} {}", propName, prop.get_element_at ? "[Array/Range]" : ""));
        }
        return;
    }

    // ========================================================================
    // 4. ENTITY <id> (Dump All Components)
    // ========================================================================
    if (cmd == "entity" || cmd == "inspect") {
        if (tokens.size() < 2) {
            console.Log("Usage: entity <id>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }
        auto ent = ParseEntity(tokens[1]);
        if (!ent || !inspection.IsAlive(*ent)) {
            console.Log(std::format("Invalid or dead entity: {}", tokens[1]), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string dump;
        inspection.Dump(*ent, dump);
        console.Log(std::format("Entity {}:", ent->Pack()), {.r = 0.3f, .g = 0.85f, .b = 1.0f, .a = 1.0f});
        std::istringstream stream(dump);
        std::string        line;
        while (std::getline(stream, line)) {
            if (!line.empty()) {
                console.Log("  " + line);
            }
        }
        return;
    }

    // ========================================================================
    // 5. GET <id> <Comp>.<field>
    // ========================================================================
    if (cmd == "get") {
        if (tokens.size() < 3) {
            console.Log("Usage: get <id> <Component>.<field>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }
        auto ent = ParseEntity(tokens[1]);
        if (!ent || !inspection.IsAlive(*ent)) {
            console.Log("Entity not found.", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string_view target = tokens[2];
        auto             dotPos = target.find('.');
        if (dotPos == std::string_view::npos) {
            console.Log("Target must be in format <Component>.<field>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string_view compName = target.substr(0, dotPos);
        std::string_view propName = target.substr(dotPos + 1);

        auto val = bridge.GetProperty(*ent, compName, propName);
        if (!val) {
            console.Log(std::format("Failed to get property: {}", val.error()), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        if (const auto* d = std::get_if<double>(&*val)) {
            console.Log(std::format("{} = {}", target, *d));
        } else if (const auto* b = std::get_if<bool>(&*val)) {
            console.Log(std::format("{} = {}", target, *b ? "true" : "false"));
        } else if (const auto* s = std::get_if<std::string>(&*val)) {
            console.Log(std::format("{} = \"{}\"", target, *s));
        } else if (const auto* arr = std::get_if<ScriptArray>(&*val)) {
            console.Log(std::format("{} = [Array of {} items]", target, arr->elements.size()));
        } else if (const auto* obj = std::get_if<BoxedObject>(&*val)) {
            console.Log(std::format("{} = Object<{}> at {}", target, obj->typeName, obj->rawPtr));
        }
        return;
    }

    // ========================================================================
    // 6. SET <id> <Comp>.<field> <value>
    // ========================================================================
    if (cmd == "set") {
        if (tokens.size() < 4) {
            console.Log("Usage: set <id> <Component>.<field> <value>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }
        auto ent = ParseEntity(tokens[1]);
        if (!ent || !inspection.IsAlive(*ent)) {
            console.Log("Entity not found.", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string_view target = tokens[2];
        auto             dotPos = target.find('.');
        if (dotPos == std::string_view::npos) {
            console.Log("Target must be in format <Component>.<field>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string_view compName = target.substr(0, dotPos);
        std::string_view propName = target.substr(dotPos + 1);
        std::string_view rawVal   = tokens[3];

        ScriptVal valToSet;
        if (rawVal == "true") {
            valToSet = true;
        } else if (rawVal == "false") {
            valToSet = false;
        } else {
            double       d  = 0.0;
            auto [p, ec]    = std::from_chars(rawVal.data(), rawVal.data() + rawVal.size(), d);
            if (ec == std::errc() && p == rawVal.data() + rawVal.size()) {
                valToSet = d;
            } else {
                valToSet = std::string(rawVal);
            }
        }

        auto res = bridge.SetProperty(*ent, compName, propName, valToSet);
        if (!res) {
            console.Log(std::format("Failed to set property: {}", res.error()), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
        } else {
            console.Log(std::format("Updated {} to {}", target, rawVal), {.r = 0.3f, .g = 1.0f, .b = 0.3f, .a = 1.0f});
        }
        return;
    }

    // ========================================================================
    // 7. PATCH <id> <Component> { field = expr, ... }
    // ========================================================================
    if (cmd == "patch") {
        console.Log("Lua patching is not available in this build.", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
        return;
    }

    // ========================================================================
    // 8. SYM <address> (Binary Symbol Demangler)
    // ========================================================================
    if (cmd == "sym" || cmd == "addr") {
        if (tokens.size() < 2) {
            console.Log("Usage: sym <hex_address>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        auto addr = ParseAddress(tokens[1]);
        if (!addr) {
            console.Log("Invalid address format. Expected hex (e.g. 0x7ffd1020).", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        auto sym = SymbolResolver::Resolve(*addr);
        if (sym.valid) {
            console.Log(
                std::format("0x{:x} -> {} + 0x{:x} ({})", sym.address, sym.name, sym.offset, sym.moduleName), {.r = 0.45f, .g = 0.95f, .b = 0.55f, .a = 1.0f}
            );
        } else {
            console.Log(std::format("0x{:x} -> [No symbol information available]", sym.address), {.r = 0.8f, .g = 0.8f, .b = 0.8f, .a = 1.0f});
        }
        return;
    }

    // ========================================================================
    // 9. FIND <name>
    // ========================================================================
    if (cmd == "find") {
        if (tokens.size() < 2) {
            console.Log("Usage: find <EntityName>", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        bool found = false;
        for (Entity e: inspection.EntitiesWith<Components::NameComponent>()) {
            if (const auto nameComp = inspection.Read<Components::NameComponent>(e)) {
                if (std::string_view(nameComp->name) == tokens[1]) {
                    console.Log(std::format("Found Entity: {} (ID: {})", nameComp->name.c_str(), e.Pack()), {.r = 0.3f, .g = 1.0f, .b = 0.3f, .a = 1.0f});
                    found = true;
                }
            }
        }
        if (!found) {
            console.Log(std::format("No entity named '{}' found.", tokens[1]), {.r = 1.0f, .g = 0.8f, .b = 0.2f, .a = 1.0f});
        }
        return;
    }

    console.Log(std::format("Unknown command: '{}'. Type 'help' for available commands.", cmd), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
}

} // namespace ZHLN
