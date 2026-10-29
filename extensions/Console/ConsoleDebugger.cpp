// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConsoleDebugger.hpp"
#include "SymbolResolver.hpp"

#include <Console/Console.hpp>
#include <Scripting/ScriptBinder.hpp>
#include <Scripting/ScriptECSBridge.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>

#if defined(ZHLN_CONSOLE_HAS_LUA)
#include <lua/ReflectLua.hpp>
#include <lua/SceneLua.hpp>
#endif

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

    auto TrimAscii(std::string_view sv) noexcept -> std::string_view {
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
            sv.remove_suffix(1);
        }
        return sv;
    }

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

    // Extracts the raw tail after the Nth whitespace-delimited token so Lua table
    // expressions preserve spaces, quotes, and arithmetic verbatim.
    auto SliceAfterTokens(std::string_view line, size_t tokenCount) noexcept -> std::string_view {
        size_t pos      = 0;
        bool   inQuotes = false;
        for (size_t t = 0; t < tokenCount; ++t) {
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
                ++pos;
            }
            while (pos < line.size()) {
                const char c = line[pos];
                if (c == '"') {
                    inQuotes = !inQuotes;
                } else if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes) {
                    break;
                }
                ++pos;
            }
        }
        return TrimAscii(line.substr(pos));
    }

    auto ParseEntity(std::string_view sv) -> std::optional<Entity> {
        uint64_t raw   = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), raw);
        if (ec == std::errc()) {
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
        if (ec == std::errc()) {
            return addr;
        }
        return std::nullopt;
    }

#if defined(ZHLN_CONSOLE_HAS_LUA)
    using ComponentPatchFn = std::expected<void, ErrorCode> (*)(void* rawComp, std::string_view luaChunk);

    template <typename CompT>
    auto PatchTypedComponent(void* rawComp, std::string_view luaChunk) -> std::expected<void, ErrorCode> {
        return ReflectLua::ApplyPatchString(*static_cast<CompT*>(rawComp), luaChunk, "console_patch");
    }

    auto ComponentPatchTable() -> const std::unordered_map<std::string_view, ComponentPatchFn>& {
        static const auto kTable = []() {
            std::unordered_map<std::string_view, ComponentPatchFn> map;
            Reflect::ForEachNestedType<Components>([&]<typename Comp>() {
                map.emplace(Reflect::TypeName<Comp>(), &PatchTypedComponent<Comp>);
            });
            return map;
        }();
        return kTable;
    }
#endif

} // namespace

void ConsoleDebugger::Execute(Engine& engine, GameConsole& console, std::string_view commandLine) {
    Execute(engine.GetRegistry(), console, commandLine);
}

void ConsoleDebugger::Execute(ECS::Registry& reg, GameConsole& console, std::string_view commandLine) {
    auto tokens = Tokenize(commandLine);
    if (tokens.empty()) {
        return;
    }

    std::string_view cmd = tokens[0];
    ScriptECSBridge  bridge(reg);

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
        console.Log("  patch <id> <Comp> { k = v }   - Transactionally patch a component via Lua table");
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
        if (!ent || !reg.IsAlive(*ent)) {
            console.Log(std::format("Invalid or dead entity: {}", tokens[1]), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string dump;
        reg.DebugDumpEntity(*ent, dump);
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
        if (!ent || !reg.IsAlive(*ent)) {
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
        if (!ent || !reg.IsAlive(*ent)) {
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
            if (ec == std::errc()) {
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
#if defined(ZHLN_CONSOLE_HAS_LUA)
        const std::string_view luaExpr = SliceAfterTokens(commandLine, 3);
        if (tokens.size() < 4 || luaExpr.empty()) {
            console.Log("Usage: patch <id> <Component> { field = expr, ... }", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }
        auto ent = ParseEntity(tokens[1]);
        if (!ent || !reg.IsAlive(*ent)) {
            console.Log("Entity not found.", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        const std::string_view compName = tokens[2];
        const uint32_t         familyID = ECS::Registry::GetFamilyIDFromName(compName);
        if (familyID == 0xFFFFFFFF) {
            console.Log(std::format("Component '{}' is not registered.", compName), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }
        void* compPtr = reg.GetRawByFamily(*ent, familyID);
        if (compPtr == nullptr) {
            console.Log(std::format("Entity {} does not own '{}'.", ent->Pack(), compName), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        const auto& patchers = ComponentPatchTable();
        const auto  patchIt  = patchers.find(compName);
        if (patchIt == patchers.end()) {
            console.Log(std::format("Component '{}' does not support Lua patching.", compName), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
            return;
        }

        std::string chunk;
        if (luaExpr.starts_with('{')) {
            chunk = std::format("return {}", luaExpr);
        } else {
            chunk.assign(luaExpr);
        }

        if (auto res = patchIt->second(compPtr, chunk); !res) {
            console.Log(std::format("Failed to patch {}: {}", compName, res.error()), {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
        } else {
            console.Log(std::format("Patched {} on entity {}", compName, ent->Pack()), {.r = 0.3f, .g = 1.0f, .b = 0.3f, .a = 1.0f});
        }
#else
        console.Log("Lua patching is disabled in this build.", {.r = 1.0f, .g = 0.4f, .b = 0.4f, .a = 1.0f});
#endif
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
        for (Entity e: reg.GetEntitiesWith<Components::NameComponent>()) {
            if (const auto nameComp = reg.Get<Components::NameComponent>(e)) {
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
