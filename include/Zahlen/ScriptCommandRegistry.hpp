// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/ScriptCommandRegistry.hpp
//
// The seam that lets a module hand the script host a command without the script
// host knowing the module exists.
//
// Before this header, extensions/Scripting/Lua/Scripting.cpp included
// <RagdollAuthoring/RagdollAuthoring.hpp> and called
// ZHLN::RagdollAuthoring::BuildHumanoidBipedRagdoll from inside the SetupRagdoll
// command body. The dependency ran the wrong way twice over: the generic
// scripting layer could not compile without a gameplay module, and a gameplay
// module could not run under a host that did not link the scripting layer. Both
// directions were a consequence of putting the *verb* in the scripting layer
// when it belongs to the domain that implements it.
//
// The shape here is the one the rest of the tree already uses for a capability
// core must be able to name without depending on its implementation -- the
// IScriptRuntime the ScriptRunner forwards to, the BakedFontLoader plugins/Fonts
// installs, the bone-pose post-processor an animation extension supplies. Core
// declares the shape of a contribution, a module provides the value, and the
// host is what connects them. Here the shape is a name and a function pointer:
// the scripting layer resolves a name it was handed, and never a symbol.
//
// Commands are addressed by name rather than by the integer the Lua ABI caches,
// because the integer belongs to the script host's interning and is not
// something core can mint. A hosted runtime plugs in through SetInterner below,
// which is how a module command ends up with an id the script side can cache
// like any other.

#include <Zahlen/Common.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN {

class Engine;

// One script-facing verb a module contributes. `args` points at the argument
// blob the script host marshalled -- a POD mirroring the Lua-side ffi.cdef
// declaration -- and its layout is the contributing module's business, which is
// why the struct is declared next to the command that reads it and not here.
//
// The return value is the script-visible integer the Lua side sees; 0 is the
// conventional "did nothing".
struct ScriptCommand {
    std::string_view name;
    uint64_t (*invoke)(Engine& engine, const void* args) = nullptr;
};

// Host-supplied name interning. A script host that caches command ids (the
// LuaJIT ABI interns each name once and dispatches through a jump table) installs
// one of these, and a module command added afterwards is resolvable by the
// script side under the same id every other command uses. Returns the id the
// host minted, or `kUnknownCommandID` when the host cannot intern.
using ScriptCommandInterner = uint32_t (*)(std::string_view name);

inline constexpr uint32_t kUnknownCommandID = 0xFFFFFFFFu;

class ZHLN_API ScriptCommandRegistry {
  public:
    // Binding a name twice is a composition error, not a merge: two modules
    // claiming "SpawnThing" is a bug in what the application installed, and the
    // first binding winning silently is how it would go unnoticed. Reported
    // like every other install-time problem -- false, and the caller's problem.
    auto Add(ScriptCommand command) -> bool;

    [[nodiscard]] auto Find(std::string_view name) const noexcept -> const ScriptCommand*;

    // A name no installed module provides is a no-op that yields nothing, not an
    // error: a document or script may legitimately name a verb this host did not
    // install. That is the same policy GUI::ActionRegistry settled on for an
    // action id no host bound.
    [[nodiscard]] auto Invoke(Engine& engine, std::string_view name, const void* args) const -> std::optional<uint64_t>;

    // Null when no script host installed one -- a C++-only host still holds
    // module commands, it just has no ids to give them. Every command already
    // added is interned when an interner arrives, so install order between a
    // host's runtime and the application's modules does not matter.
    void SetInterner(ScriptCommandInterner interner);

    [[nodiscard]] auto InternedID(std::string_view name) const noexcept -> std::optional<uint32_t>;

    // Enumeration for a host that wants to report what it contributed. Indexed
    // rather than a span: the entries carry each command's interned id next to
    // it, so a view over the commands alone would have to be stride-corrected
    // into a second container that then has to be kept in step with this one.
    [[nodiscard]] auto Count() const noexcept -> size_t;
    [[nodiscard]] auto At(size_t index) const noexcept -> const ScriptCommand*;

    void Clear() noexcept;

  private:
    struct Entry {
        ScriptCommand  command;
        std::optional<uint32_t> internedID;
    };

    ScriptCommandInterner _interner = nullptr;
    std::vector<Entry>    _commands;
};

// How a module contributes its verbs: one function the host calls with the
// registry the engine holds.
using ScriptCommandRegistrar = void (*)(ScriptCommandRegistry& commands);

} // namespace ZHLN
