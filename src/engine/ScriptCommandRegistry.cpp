// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/ScriptCommandRegistry.hpp>

namespace ZHLN {

auto ScriptCommandRegistry::Add(ScriptCommand command) -> bool {
    if (command.name.empty() || command.invoke == nullptr) {
        return false;
    }
    if (Find(command.name) != nullptr) {
        return false;
    }

    Entry entry {.command = command, .internedID = std::nullopt};
    if (_interner != nullptr) {
        entry.internedID = _interner(command.name);
    }
    _commands.push_back(entry);
    return true;
}

auto ScriptCommandRegistry::Find(std::string_view name) const noexcept -> const ScriptCommand* {
    for (const Entry& entry: _commands) {
        if (entry.command.name == name) {
            return &entry.command;
        }
    }
    return nullptr;
}

auto ScriptCommandRegistry::Invoke(Engine& engine, std::string_view name, const void* args) const -> std::optional<uint64_t> {
    const ScriptCommand* command = Find(name);
    if (command == nullptr || command->invoke == nullptr) {
        return std::nullopt;
    }
    return command->invoke(engine, args);
}

void ScriptCommandRegistry::SetInterner(ScriptCommandInterner interner) {
    _interner = interner;
    if (_interner == nullptr) {
        return;
    }
    // Re-interning what was already added is what makes the host's runtime and
    // the application's modules order-independent: a host that installs its
    // runtime after the modules that contribute verbs still reaches them.
    for (Entry& entry: _commands) {
        entry.internedID = _interner(entry.command.name);
    }
}

auto ScriptCommandRegistry::InternedID(std::string_view name) const noexcept -> std::optional<uint32_t> {
    for (const Entry& entry: _commands) {
        if (entry.command.name == name) {
            return entry.internedID;
        }
    }
    return std::nullopt;
}

auto ScriptCommandRegistry::Count() const noexcept -> size_t {
    return _commands.size();
}

auto ScriptCommandRegistry::At(size_t index) const noexcept -> const ScriptCommand* {
    return index < _commands.size() ? &_commands[index].command : nullptr;
}

void ScriptCommandRegistry::Clear() noexcept {
    _commands.clear();
}

} // namespace ZHLN
