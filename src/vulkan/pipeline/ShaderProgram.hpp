// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PushDataLayout.hpp"

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

namespace Declared {

template <ZHLN::StringLiteral Name, VkDescriptorType Type, uint32_t Set, uint32_t Binding>
struct ResourceSlot {
    static constexpr std::string_view name    = Name;
    static constexpr VkDescriptorType type    = Type;
    static constexpr uint32_t         set     = Set;
    static constexpr uint32_t         binding = Binding;
};

template <ZHLN::StringLiteral Name, uint32_t Set, uint32_t Binding>
struct SamplerSlot {
    static constexpr std::string_view name    = Name;
    static constexpr uint32_t         set     = Set;
    static constexpr uint32_t         binding = Binding;
};

}

struct ResourceBindings {
    template <typename Program>
    using Of = typename Program::Resources;
};

struct SamplerBindings {
    template <typename Program>
    using Of = typename Program::Samplers;
};

struct PushMember {
    const char* name   = nullptr;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

template <typename Module>
concept DeclaresPushBlock = requires {
    Module::PushSize;
    Module::Push;
};

template <typename CppPush, typename Module>
[[nodiscard]] consteval auto PushConstantLayoutMatches() noexcept -> bool {
    if constexpr (!DeclaresPushBlock<Module>) {
        return false;
    } else {
        constexpr uint32_t kMembers = static_cast<uint32_t>(sizeof(Module::Push) / sizeof(Module::Push[0]));
#if ZHLN_REFLECTION_AVAILABLE
        bool         ok     = true;
        uint32_t     extent = 0;
        Reflect::ForEachFieldInfo<CppPush>([&]<typename FieldType>(std::string_view name, std::size_t offset) {
            const uint32_t begin = static_cast<uint32_t>(offset);
            const uint32_t end   = begin + static_cast<uint32_t>(sizeof(FieldType));
            bool           found = false;
            bool           hole  = true;
            for (uint32_t i = 0; i < kMembers; ++i) {
                const PushMember& member = Module::Push[i];
                if (name == member.name && begin == member.offset && sizeof(FieldType) == member.size) {
                    found = true;
                    extent  = extent > member.offset + member.size ? extent : member.offset + member.size;
                }
                hole = hole && !(begin < member.offset + member.size && member.offset < end);
            }
            ok = ok && (found || hole);
        });
        ok = ok && sizeof(CppPush) == ::ZHLN::Vk::AlignUp(extent, static_cast<uint32_t>(alignof(CppPush)));
        return ok;
#else
        return sizeof(CppPush) == ::ZHLN::Vk::AlignUp(Module::PushSize, static_cast<uint32_t>(alignof(CppPush)));
#endif
    }
}

struct SpirvBinding;
class SpirvBindings;

template <typename... Slots>
struct BindingList {
    static constexpr size_t count = sizeof...(Slots);

    [[nodiscard]] static constexpr auto Declares([[maybe_unused]] std::string_view name) noexcept -> bool {
        return ((Slots::name == name) || ...);
    }

    [[nodiscard]] static constexpr auto Spells(const SpirvBindings& declarations, const SpirvBinding& candidate, uint32_t set) noexcept -> bool;
};

template <typename List>
struct SlotsOf;
template <typename... Slots>
struct SlotsOf<BindingList<Slots...>> {
    using tuple = std::tuple<Slots...>;
};
template <typename List>
using SlotsOfT = typename SlotsOf<List>::tuple;

template <typename Half, typename Program>
using DeclaredList = typename Half::template Of<Program>;

template <typename T>
concept ShaderProgram = requires {
    typename T::Resources;
    typename T::Samplers;
    { T::EntryPoint } -> std::convertible_to<std::string_view>;
    { T::Stage } -> std::convertible_to<VkShaderStageFlagBits>;
    { T::Path } -> std::convertible_to<const char*>;
    { T::Bytes() } -> std::same_as<std::span<const uint8_t>>;
};

template <typename CppPush, ShaderProgram Module>
[[nodiscard]] consteval auto PushConstantLayoutMatchesOne() noexcept -> bool {
    if constexpr (!DeclaresPushBlock<Module>) {
        return true;
    } else {
        return PushConstantLayoutMatches<CppPush, Module>();
    }
}

template <typename CppPush, ShaderProgram... Modules>
[[nodiscard]] consteval auto PushConstantLayoutMatchesAll() noexcept -> bool {
    static_assert(
        sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushConstantLayoutMatchesAll<PushT, Shaders::Modules::X>()"
    );
    static_assert((DeclaresPushBlock<Modules> || ...), "none of the named shader modules declares a push-constant block: the bytes would be written for no one");
    return (PushConstantLayoutMatchesOne<CppPush, Modules>() && ...);
}


namespace TemplatedDetail {

template <typename Set, typename Half, ZHLN::StringLiteral Name>
struct UndeclaredBinding;

template <typename Set, typename Half, typename Program, uint32_t Binding>
struct UnspelledBinding;

template <typename Set, typename Half, typename Program, uint32_t Binding, bool Spelled>
struct DeclarationSpelledBy;
template <typename Set, typename Half, typename Program, uint32_t Binding>
struct DeclarationSpelledBy<Set, Half, Program, Binding, true> {};

template <typename Slot>
[[nodiscard]] consteval auto IsUnreadSlot() noexcept -> bool {
    if constexpr (requires { Slot::unread; }) {
        return Slot::unread;
    } else {
        return false;
    }
}

template <typename Half, typename Program, typename Slot>
[[nodiscard]] consteval auto SlotIsDeclared() noexcept -> bool {
    if constexpr (IsUnreadSlot<Slot>()) {
        return true;
    } else {
        return DeclaredList<Half, Program>::Declares(Slot::name);
    }
}

template <typename Half, typename Program, typename... Slots>
[[nodiscard]] consteval auto DeclaredSlotMask() noexcept -> uint64_t {
    uint64_t mask  = 0;
    uint32_t index = 0;
    ((mask |= SlotIsDeclared<Half, Program, Slots>() ? (uint64_t {1} << index) : uint64_t {0}, ++index), ...);
    return mask;
}

template <typename Set, typename Half, uint64_t Mask, typename... Slots, size_t... Index>
consteval void RequireDeclaredBits(std::index_sequence<Index...>) {
    (static_cast<void>(sizeof(std::conditional_t<
                           ((Mask >> Index) & 1u) != 0,
                           std::true_type,
                           UndeclaredBinding<Set, Half, Slots...[Index]::literal>
                       >)), ...);
}

template <typename DeclaredSlot, typename... Slots>
[[nodiscard]] consteval auto SpellsDeclaredSlot() noexcept -> bool {
    return ((Slots::name == DeclaredSlot::name) || ...) || (IsUnreadSlot<Slots>() || ...);
}

template <typename Set, typename Half, typename Program, typename... Slots, size_t... Index>
consteval void RequireSpelledBindings(std::index_sequence<Index...>) {
    using List = DeclaredList<Half, Program>;
    [&]<typename... Declared>(std::type_identity<std::tuple<Declared...>>) {
        (static_cast<void>(sizeof(DeclarationSpelledBy<
                               Set,
                               Half,
                               Program,
                               Declared...[Index]::binding,
                               SpellsDeclaredSlot<Declared...[Index], Slots...>()
                           >)), ...);
    }(std::type_identity<SlotsOfT<List>> {});
}

template <typename Set, typename Half, ShaderProgram Program, typename... Slots>
[[nodiscard]] consteval auto SpellsEveryDeclaration() -> bool {
    constexpr size_t declared = DeclaredList<Half, Program>::count;
    RequireSpelledBindings<Set, Half, Program, Slots...>(std::make_index_sequence<declared> {});
    return true;
}


template <typename Check, typename DeclaredSlot, typename WriteSlot>
[[nodiscard]] consteval auto DeclaredSlotHoldsCheck() noexcept -> bool {
    if constexpr (DeclaredSlot::name == WriteSlot::name) {
        return Check::template Holds<DeclaredSlot, WriteSlot>();
    } else {
        return true;
    }
}

template <typename Check, typename List, typename WriteSlot, size_t... Index>
[[nodiscard]] consteval auto CheckDeclaredSlotsAt(std::index_sequence<Index...>) noexcept -> bool {
    return [&]<typename... Declared>(std::type_identity<std::tuple<Declared...>>) {
        return (DeclaredSlotHoldsCheck<Check, Declared...[Index], WriteSlot>() && ...);
    }(std::type_identity<SlotsOfT<List>> {});
}

template <typename Check, typename Half, typename Program, typename WriteSlot>
[[nodiscard]] consteval auto ModuleSatisfiesCheck() noexcept -> bool {
    if constexpr (IsUnreadSlot<WriteSlot>()) {
        return true;
    } else {
        using List = DeclaredList<Half, Program>;
        return CheckDeclaredSlotsAt<Check, List, WriteSlot>(std::make_index_sequence<List::count> {});
    }
}

template <typename Check, typename Half, typename Program, typename... Slots>
[[nodiscard]] consteval auto ModuleSatisfiesChecks() noexcept -> bool {
    return (ModuleSatisfiesCheck<Check, Half, Program, Slots>() && ...);
}

}

template <ShaderProgram... Programs>
struct ShaderSet {
    static constexpr uint32_t programCount = sizeof...(Programs);

    template <typename Half>
    [[nodiscard]] static consteval auto Declares([[maybe_unused]] std::string_view name) -> bool {
        return (DeclaredList<Half, Programs>::Declares(name) || ...);
    }

    template <typename Half, typename... Slots>
    [[nodiscard]] static consteval auto SpellsDeclaredNames() -> bool {
        constexpr uint64_t mask = (TemplatedDetail::DeclaredSlotMask<Half, Programs, Slots...>() | ...);
        TemplatedDetail::RequireDeclaredBits<ShaderSet, Half, mask, Slots...>(std::index_sequence_for<Slots...> {});
        return true;
    }

    template <typename Half, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsAreSpelled() -> bool {
        return (TemplatedDetail::SpellsEveryDeclaration<ShaderSet, Half, Programs, Slots...>() && ...);
    }

    template <typename Half, typename Check, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsHold() -> bool {
        return (TemplatedDetail::ModuleSatisfiesChecks<Check, Half, Programs, Slots...>() && ...);
    }

    template <typename CppPush>
    [[nodiscard]] static consteval auto PushLayoutMatches() -> bool {
        return (PushConstantLayoutMatchesOne<CppPush, Programs>() && ...);
    }

    template <typename Pass, typename... Args>
    static void DispatchHeapIndexed(Pass& pass, Args&&... args) noexcept {
        pass.template DispatchHeapIndexedThreads<Programs...>(std::forward<Args>(args)...);
    }
};

template <typename T>
concept ShaderProgramSet = requires {
    { T::programCount } -> std::convertible_to<uint32_t>;
    { T::template Declares<ResourceBindings>(std::string_view {}) } -> std::same_as<bool>;
};

template <typename Set, typename Half, typename... Slots>
[[nodiscard]] consteval auto NamesAreDeclared() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template SpellsDeclaredNames<Half, Slots...>();
}

template <typename Set, typename Half, typename... Slots>
[[nodiscard]] consteval auto NamesCoverDeclarations() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsAreSpelled<Half, Slots...>();
}

template <typename... Slots>
[[nodiscard]] consteval auto NamesAreDistinct() noexcept -> bool {
    constexpr std::array<std::string_view, sizeof...(Slots)> spelled {Slots::name...};
    for (size_t i = 0; i < spelled.size(); ++i) {
        for (size_t j = i + 1; j < spelled.size(); ++j) {
            if (spelled[i] == spelled[j]) {
                return false;
            }
        }
    }
    return true;
}

template <typename Set, typename Half, typename Check, typename... Slots>
[[nodiscard]] consteval auto DeclarationsSatisfy() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsHold<Half, Check, Slots...>();
}


template <ShaderProgram Program>
[[nodiscard]] auto CreateShaderDesc() noexcept -> ZHLN_ShaderDesc {
    const std::span<const uint8_t> bytes = Program::Bytes();
    const std::string_view entryPoint = Program::EntryPoint;
    return ZHLN_ShaderDesc {
        .code        = std::bit_cast<const uint32_t*>(bytes.data()),
        .size        = bytes.size_bytes(),
        .entry_point = entryPoint.data(),
    };
}

template <ShaderProgram Program>
[[nodiscard]] consteval auto StageOf() noexcept -> VkShaderStageFlagBits {
    return Program::Stage;
}

}
