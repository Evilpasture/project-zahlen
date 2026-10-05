// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/Reflection/Services.hpp>
#include <Zahlen/Core/SoA.hpp>
#include <Zahlen/Frame.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/SystemAccess.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <Zahlen/ecs/SystemSignature.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

// A graph that provides no services: the graph a unit test builds when it wants
// to exercise ordering, queries and frame parameters and nothing else. Systems
// that ask for a service cannot be added to it -- that is a compile error, not a
// null pointer at run time.
struct NoServices {};

// What the executor hands a system. Two references and a frame: the registry the
// graph runs against, the services the graph was built with, and this
// execution's clock and scratch. No engine pointers, no null service slots.
//
// `local` is this node's own state (see Local<T>): the graph fills it in for each
// invocation, and it is the only way a system reaches state that belongs to it
// rather than to the world.
//
// The `typedef` names the services type so a resolver can ask, of the carrier it
// was handed, whether those services provide what it was asked to resolve.
template <typename ServicesT>
struct Carrier {
    using Services = ServicesT;

    ECS::Registry&                  registry;
    ServicesT&                      services;
    const ZHLN::Frame&              frame;
    TemplatedDetail::LocalStateView local {};
};

// What the core hands a node's function: the carrier the graph was executed with,
// and the storage this node owns. One of these is built per invocation on the
// invoking thread's stack -- the carrier itself is shared by every node, so
// per-node state cannot live on it without racing.
//
// The erased function pointer still takes a single `void*`, and a hand-built
// SystemInfo whose function ignores its argument is unaffected.
struct SystemCall {
    void*                                 carrier   = nullptr;
    void*                                 local     = nullptr;
    const TemplatedDetail::LocalSlotDesc* slots     = nullptr;
    size_t                                slotCount = 0;
};

// Parameters are deliberately matched by *exact* type. In particular a plain
// float cannot accidentally select dt instead of alpha, and a Res/Query
// reference cannot bind to a short-lived resolved value.
//
// The primary template resolves a *service by its type*: a system asking for
// `Camera` or `VisibleEntities` gets the bundle member of that type. It is
// constrained on the bundle actually providing one, so a graph that offers no
// such service cannot resolve the parameter -- and the resolver not being
// callable is what lets AddSystem<SystemFn>() say so while compiling.
template <typename Param>
struct ParameterResolver {
    template <typename CarrierType>
        requires(Reflect::BundleProvides<Param, typename CarrierType::Services>())
    static auto Resolve(CarrierType& carrier) noexcept -> Param {
        Param* service = Reflect::FindService<Param>(carrier.services);
        // Unreachable: the constraint above is answered by the same member list
        // this walk reads, so a bundle that provides Param always finds it.
        ZHLN::Assert(service != nullptr, "System service is missing from the bundle it was resolved against");
        return *service;
    }
};

template <typename... Comps>
struct ParameterResolver<Query<Comps...>> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> Query<Comps...> {
        return Query<Comps...>(carrier.registry);
    }
};

template <>
struct ParameterResolver<Registry&> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> Registry& {
        return carrier.registry;
    }
};

template <>
struct ParameterResolver<const Registry&> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> const Registry& {
        return carrier.registry;
    }
};

template <>
struct ParameterResolver<FrameDt> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> FrameDt {
        return {carrier.frame.dt};
    }
};

template <>
struct ParameterResolver<FrameAlpha> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> FrameAlpha {
        return {carrier.frame.alpha};
    }
};

template <>
struct ParameterResolver<FrameIndex> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> FrameIndex {
        return {carrier.frame.frame};
    }
};

template <typename T>
struct ParameterResolver<Res<T>> {
    template <typename CarrierType>
        requires(Reflect::BundleProvides<T, typename CarrierType::Services>())
    static auto Resolve(CarrierType& carrier) noexcept -> Res<T> {
        return {Reflect::FindService<T>(carrier.services)};
    }
};

template <typename T>
struct ParameterResolver<ResMut<T>> {
    template <typename CarrierType>
        requires(Reflect::BundleProvides<T, typename CarrierType::Services>())
    static auto Resolve(CarrierType& carrier) noexcept -> ResMut<T> {
        return {Reflect::FindService<T>(carrier.services)};
    }
};

// A node's own state. The block was allocated for *this* node from the layout its
// signature produced, so the lookup is a walk over a handful of slots for the
// family id of LocalToken<T> -- no TypeId resolution, no world, and nothing
// outside this system can reach the storage.
//
// The asserts are the mis-registration boundary: a hand-built SystemInfo that
// names a function taking Local<T> without describing the layout, or describing a
// different one, stops here with the reason instead of reading garbage.
template <typename T>
struct ParameterResolver<Local<T>> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> Local<T> {
        const TemplatedDetail::LocalStateView& state = carrier.local;
        ZHLN::Assert(state.block != nullptr, "Local<T> in a system whose node was given no state: the graph allocates it from the signature");
        for (size_t i = 0; i < state.count; ++i) {
            if (state.slots[i].familyId == ComponentFamily::GetTypeID<TemplatedDetail::LocalToken<T>>()) {
                return {static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(state.block) + state.slots[i].offset))};
            }
        }
        ZHLN::Assert(false, "This node's state carries no slot of the type this system asked for: the layout and the signature disagree");
        return {};
    }
};

// A service the *graph* may or may not provide, without a null check at run
// time: whether the bundle has it is answered while compiling, and only the
// absence case produces a nullopt.
template <TemplatedDetail::OptionalResourceParam Param>
struct ParameterResolver<Param> {
    using Value = typename Param::value_type;

    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) noexcept -> Param {
        if constexpr (Reflect::BundleProvides<std::remove_const_t<Value>, typename CarrierType::Services>()) {
            return *Reflect::FindService<std::remove_const_t<Value>>(carrier.services);
        } else {
            return std::nullopt;
        }
    }
};

// The per-worker scratch, from this frame's pool.
template <typename T, size_t N>
struct ParameterResolver<SoAScratch<T, N>> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) -> SoAScratch<T, N> {
        ZHLN::Assert(carrier.frame.scratch != nullptr, "This graph's frames carry no scratch pool");
        auto& arena = carrier.frame.scratch->GetWorkerArena(TaskSystem::GetWorkerIndex());
        return SoAScratch<T, N>(arena.Allocate(SoABlock<T>::ComputeByteSize(N), SoABlock<T>::StreamAlignment));
    }
};

// The arena itself, for a system whose scratch shape is not fixed by its
// signature: it allocates what it needs and the graph still resets it at the
// frame boundary. Prefer SoAScratch<T, N> where N is knowable -- the allocation
// there is sized and aligned by the layout rather than by the caller's estimate.
template <>
struct ParameterResolver<LinearArena&> {
    template <typename CarrierType>
    static auto Resolve(CarrierType& carrier) -> LinearArena& {
        ZHLN::Assert(carrier.frame.scratch != nullptr, "This graph's frames carry no scratch pool");
        return carrier.frame.scratch->GetWorkerArena(TaskSystem::GetWorkerIndex());
    }
};

namespace TemplatedDetail {

// One Local<T> slot as the compiler sees it. The family id is deliberately the
// one member that is not a constant expression: ComponentFamily::GetTypeID caches
// a registry-side id, so it is reached through a function pointer that the layout
// builder calls at run time.
struct LocalSlotMeta {
    uint32_t offset = 0;
    uint32_t size   = 0;
    uint32_t align  = 1;

    uint32_t (*familyId)()        = nullptr;
    void (*construct)(void* slot) = nullptr;
    void (*destroy)(void* slot)   = nullptr;
};

template <typename Inspector, std::size_t I>
inline constexpr bool IsLocalParameter = IsLocalParam<std::remove_cvref_t<typename Inspector::template ParameterType<I>>>;

template <typename Inspector, std::size_t I>
using LocalParameterType = typename std::remove_cvref_t<typename Inspector::template ParameterType<I>>::Target;

// How many Local<T> the signature names. The slot table is sized by this.
template <auto SystemFn>
consteval auto LocalSlotCount() -> std::size_t {
    using Inspector       = Reflect::CallableInspector<SystemFn>;
    std::size_t slotCount = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((IsLocalParameter<Inspector, Is> ? ++slotCount : slotCount), ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    return slotCount;
}

template <auto SystemFn>
struct LocalLayoutDesc {
    std::array<LocalSlotMeta, LocalSlotCount<SystemFn>()> slots {};
    std::size_t                                           size  = 0;
    std::size_t                                           align = 1;
};

// The signature's state, laid out once: aligned offsets in declaration order,
// total size, and the per-slot construct/destroy pair. The allocator and the
// resolver both come from here, so a slot cannot be described with one type and
// constructed with another.
template <auto SystemFn>
consteval auto DescribeLocalSlots() -> LocalLayoutDesc<SystemFn> {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    LocalLayoutDesc<SystemFn> desc {};
    std::size_t               index  = 0;
    std::size_t               offset = 0;

    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (
            [&] {
                if constexpr (IsLocalParameter<Inspector, Is>) {
                    using T             = LocalParameterType<Inspector, Is>;
                    offset              = Math::AlignUp(offset, alignof(T));
                    desc.slots[index++] = LocalSlotMeta {
                        .offset    = static_cast<uint32_t>(offset),
                        .size      = static_cast<uint32_t>(sizeof(T)),
                        .align     = static_cast<uint32_t>(alignof(T)),
                        .familyId  = +[]() -> uint32_t { return ComponentFamily::GetTypeID<LocalToken<T>>(); },
                        .construct = +[](void* slot) { ::new (slot) T {}; },
                        .destroy   = +[](void* slot) { std::destroy_at(static_cast<T*>(slot)); },
                    };
                    offset += sizeof(T);
                    desc.align = alignof(T) > desc.align ? alignof(T) : desc.align;
                }
            }(),
            ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    desc.size = offset;
    return desc;
}

// The same layout in its runtime form: what the graph allocates from, and what
// the resolver scans. Built once per node, when the system is added.
template <auto SystemFn>
inline auto MakeLocalLayout() -> LocalLayout {
    constexpr auto desc = DescribeLocalSlots<SystemFn>();

    LocalLayout layout;
    layout.size  = desc.size;
    layout.align = desc.align;
    layout.slots.reserve(desc.slots.size());
    for (const LocalSlotMeta& meta: desc.slots) {
        layout.slots.push_back(LocalSlotDesc {.familyId = meta.familyId(), .offset = meta.offset, .construct = meta.construct, .destroy = meta.destroy});
    }
    return layout;
}

// ------ signature checks over the written parameter types -------------------
// Two Local<T> of one type would resolve to one slot, and a Local<T> beside a
// Res<T> names one state on both sides of the boundary. Both are mistakes in the
// signature, and both are rejected where the system is added.

template <typename Clean>
struct ServiceTargetOf {
    static constexpr bool defined = false;
};

template <typename T>
struct ServiceTargetOf<Res<T>> {
    static constexpr bool defined = true;
    using Type                    = T;
};

template <typename T>
struct ServiceTargetOf<ResMut<T>> {
    static constexpr bool defined = true;
    using Type                    = T;
};

// ZHLN::Optional is an alias template whose expansion puts T in a non-deduced
// context, so a partial specialization on ZHLN::Optional<T&> is never matched.
// Use a concept-constrained specialization (see OptionalResourceParam) instead.
template <OptionalResourceParam Param>
struct ServiceTargetOf<Param> {
    static constexpr bool defined = true;
    using Type                    = typename Param::value_type;
};

template <typename Inspector, std::size_t I, std::size_t J>
consteval auto LocalPairDuplicates() -> bool {
    if constexpr (IsLocalParameter<Inspector, I> && IsLocalParameter<Inspector, J>) {
        return std::is_same_v<LocalParameterType<Inspector, I>, LocalParameterType<Inspector, J>>;
    } else {
        return false;
    }
}

template <typename Inspector, std::size_t I, std::size_t J>
consteval auto LocalPairNamesService() -> bool {
    using LocalParam = std::remove_cvref_t<typename Inspector::template ParameterType<I>>;
    using OtherParam = std::remove_cvref_t<typename Inspector::template ParameterType<J>>;

    if constexpr (IsLocalParam<LocalParam> && ServiceTargetOf<OtherParam>::defined) {
        return std::is_same_v<typename LocalParam::Target, typename ServiceTargetOf<OtherParam>::Type>;
    } else {
        return false;
    }
}

template <auto SystemFn, std::size_t I>
consteval auto LocalRowIsUnique() -> bool {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    bool unique     = true;
    [&]<std::size_t... Js>(std::index_sequence<Js...>) {
        ((unique = unique && !(Js > I && LocalPairDuplicates<Inspector, I, Js>())), ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    return unique;
}

template <auto SystemFn>
consteval auto LocalParametersAreUnique() -> bool {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    bool unique     = true;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((unique = unique && LocalRowIsUnique<SystemFn, Is>()), ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    return unique;
}

template <auto SystemFn, std::size_t I>
consteval auto LocalRowShadowsNoService() -> bool {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    bool clean      = true;
    [&]<std::size_t... Js>(std::index_sequence<Js...>) {
        ((clean = clean && !LocalPairNamesService<Inspector, I, Js>()), ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    return clean;
}

template <auto SystemFn>
consteval auto LocalParametersShadowNoService() -> bool {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    bool clean      = true;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((clean = clean && LocalRowShadowsNoService<SystemFn, Is>()), ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    return clean;
}

} // namespace TemplatedDetail

// A system parameter is admissible when the graph's carrier can resolve it. The
// gate AddSystem<SystemFn>() applies is this predicate, so "which parameters may
// this graph provide" has exactly one answer, in the resolver table rather than
// in a hand-maintained list beside it.
template <typename CarrierT, typename Param>
concept ResolvableFrom = requires(CarrierT& carrier) {
    // The parameter is resolved as written. A reference parameter is part of the
    // signature's meaning (Registry& is the world; Res<T> is the indirection),
    // so stripping the reference here would silently change what a system asked
    // for -- and would miss the specialization that answers it.
    ParameterResolver<Param>::Resolve(carrier);
};

// The gate itself. The walk has to be a *constant expression* -- that is what
// lets the rejection happen at the AddSystem call -- so it goes through the
// inspector's parameter *aliases* rather than instantiating a callback over
// them (ForEachParameter deliberately runs after the types are known, which is
// not a constant expression).
template <typename CarrierT, auto Fn, std::size_t... Is>
consteval auto EveryParameterResolvable(std::index_sequence<Is...> /*unused*/) -> bool {
    return (ResolvableFrom<CarrierT, typename Reflect::CallableInspector<Fn>::template ParameterType<Is>> && ...);
}

// A system whose signature this graph cannot satisfy is rejected here, as an
// immediate call at the AddSystem site: the diagnostic carries the system's
// identity through the instantiation trace, and there is no runtime
// equivalent -- by the time Execute runs, every parameter is known to exist.
template <auto SystemFn, typename CarrierT>
consteval void AdmitParameters() {
    static_assert(
        TemplatedDetail::LocalParametersAreUnique<SystemFn>(),
        "This system names two Local<T> parameters of the same type: both would resolve to the one slot the signature allocated. "
        "Give them distinct types -- a small struct wrapping the state is enough."
    );
    static_assert(
        TemplatedDetail::LocalParametersShadowNoService<SystemFn>(),
        "This system names the same T as both Local<T> and a service (Res<T>/ResMut<T>/Optional<T&>): one state cannot be node-private "
        "and shared at the same time. Keep the Local<T> for scratch the system owns, or the service for state the domain shares."
    );
    static_assert(
        EveryParameterResolvable<CarrierT, SystemFn>(std::make_index_sequence<Reflect::CallableInspector<SystemFn>::ParameterCount()> {}),
        "A system parameter cannot be resolved by this graph. Either add the service it asks for to the graph's service bundle, "
        "or ask for something every graph resolves: Query<...>, Registry&, FrameDt/FrameAlpha/FrameIndex, Local<T> for the node's own "
        "state, Res<T>/ResMut<T>/Optional<T&> for a service the bundle provides, SoAScratch<T, N> or LinearArena&."
    );
}

using SystemFunc = void (*)(void*);

struct SystemInfo {
    SystemFunc                   update_func = nullptr;
    const char*                  name        = "UnnamedSystem";
    std::vector<ComponentAccess> access_pattern;

    // The state this system's signature keeps between frames, described by the
    // signature itself: one slot per Local<T>, with the offsets, sizes and
    // construct/destroy pair the node needs to own a block. Empty when the
    // system names no Local<T>. A hand-built SystemInfo may leave it empty; a
    // function that resolves a Local<T> without one stops at the resolver.
    TemplatedDetail::LocalLayout local;

    bool enabled = true;
};

// The state one node owns: the bytes its signature asked for, plus the operations
// that construct and destroy what lives in them. Owning this in the node means
// clearing a graph, destroying it, or moving it does the right thing without a
// hand-written loop at each site.
class LocalBlock {
  public:
    LocalBlock() noexcept = default;
    ~LocalBlock() noexcept {
        Reset();
    }

    LocalBlock(const LocalBlock&)                    = delete;
    auto operator=(const LocalBlock&) -> LocalBlock& = delete;

    LocalBlock(LocalBlock&& other) noexcept {
        Take(other);
    }
    auto operator=(LocalBlock&& other) noexcept -> LocalBlock& {
        if (this != &other) {
            Reset();
            Take(other);
        }
        return *this;
    }

    // Allocate the layout and bring every slot to life, in declaration order.
    void Adopt(TemplatedDetail::LocalLayout layout) {
        Reset();
        _layout = std::move(layout);
        if (_layout.size == 0) {
            return;
        }
        _block = ::operator new(_layout.size, std::align_val_t(_layout.align));
        for (const TemplatedDetail::LocalSlotDesc& slot: _layout.slots) {
            slot.construct(static_cast<std::byte*>(_block) + slot.offset);
        }
    }

    // Take the state down, in reverse order, and hand the bytes back.
    void Reset() noexcept {
        if (_block != nullptr) {
            for (auto& slot: std::ranges::reverse_view(_layout.slots)) {
                slot.destroy(static_cast<std::byte*>(_block) + slot.offset);
            }
            ::operator delete(_block, std::align_val_t(_layout.align));
            _block = nullptr;
        }
        _layout = {};
    }

    [[nodiscard]] auto Pointer() const noexcept -> void* {
        return _block;
    }
    [[nodiscard]] auto Slots() const noexcept -> const TemplatedDetail::LocalSlotDesc* {
        return _layout.slots.data();
    }
    [[nodiscard]] auto SlotCount() const noexcept -> size_t {
        return _layout.slots.size();
    }

  private:
    void Take(LocalBlock& other) noexcept {
        _block  = std::exchange(other._block, nullptr);
        _layout = std::move(other._layout);
    }

    void*                        _block = nullptr;
    TemplatedDetail::LocalLayout _layout {};
};

// The mechanism of a graph: nodes, conflict analysis, parallel dispatch. It
// knows nothing about services or frames -- what a system is invoked with is
// whatever carrier the derived, typed graph hands it, erased to void* here.
class ZHLN_API SystemGraphCore {
  public:
    SystemGraphCore()  = default;
    ~SystemGraphCore() = default;

    SystemGraphCore(const SystemGraphCore&)                        = delete;
    auto operator=(const SystemGraphCore&) -> SystemGraphCore&     = delete;
    SystemGraphCore(SystemGraphCore&&) noexcept                    = default;
    auto operator=(SystemGraphCore&&) noexcept -> SystemGraphCore& = default;

    // The legacy API remains for extension authors who cannot yet express
    // their component accesses in their signature. The function pointer takes
    // the carrier the graph was executed with.
    void AddSystem(SystemInfo info);
    auto AddSystemBefore(SystemInfo info, std::string_view beforeSystem) -> bool;

    void DeclareExternalWrites(const char* label, std::vector<ComponentAccess> accesses);

    void Compile();
    void Execute(void* carrier);

    void               SetSystemEnabled(std::string_view name, bool enabled) noexcept;
    [[nodiscard]] auto IsSystemEnabled(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto GetSystemCount() const noexcept -> size_t;
    [[nodiscard]] auto IsEmpty() const noexcept -> bool;
    void               Clear() noexcept;

    [[nodiscard]] static auto HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) noexcept -> bool;

  private:
    // An aggregate on purpose: every construction site spells out the fields it
    // sets. The state it owns makes it move-only, and the implicit moves do the
    // right thing because LocalBlock's do.
    struct Node {
        SystemInfo            info;
        std::vector<uint32_t> dependents;
        uint32_t              initialDependencyCount = 0;

        // This node's own state (Local<T>), allocated from the layout in `info`
        // when the node is added and destroyed with the node.
        LocalBlock local;
    };

    struct ExecutionContext;
    struct NodePayload;

    static void TaskThunk(void* arg);
    void        DispatchNode(ExecutionContext& ctx, uint32_t nodeIdx);

    // Allocate this node's Local<T> storage from the layout its signature
    // produced. Every path that adds a node goes through here.
    static void AdoptLocal(Node& node);

    std::vector<Node>     _nodes;
    std::vector<uint32_t> _entryNodes;
};

// A graph that provides `Services`. The services are the graph's, not the
// frame's: they are bound once, they are references (nothing to null-check), and
// a system that asks for something they do not provide does not compile.
template <typename Services>
class SystemGraph: public SystemGraphCore {
  public:
    using CarrierType = Carrier<Services>;

    // A bundle with no members needs no engine services: such a graph (the tests'
    // NoServices graphs, for instance) is built from the registry alone.
    explicit SystemGraph(ECS::Registry& registry) noexcept
        requires(std::is_empty_v<Services>)
        : _registry(registry), _services(EmptyServices()) {
    }

    SystemGraph(ECS::Registry& registry, Services& services) noexcept: _registry(registry), _services(services) {
        static_assert(
            Reflect::TemplatedDetail::BundleTypesAreUnique<Services>(),
            "Two members of this graph's service bundle have the same type, so by-type resolution would silently pick one of "
            "them: give the services distinct types."
        );
    }

    SystemGraph(const SystemGraph&)                        = delete;
    auto operator=(const SystemGraph&) -> SystemGraph&     = delete;
    SystemGraph(SystemGraph&&) noexcept                    = default;
    auto operator=(SystemGraph&&) noexcept -> SystemGraph& = default;

    // The admission gate: a system whose signature this graph cannot satisfy is
    // rejected here, at the call site, with one message. There is no runtime
    // equivalent -- by the time Execute runs, every parameter is known to exist.
    // The mechanism's own entry points stay reachable: a caller that hand-builds a
    // SystemInfo (tests, tooling) registers it exactly as before.
    using SystemGraphCore::AddSystem;
    using SystemGraphCore::AddSystemBefore;

    template <auto SystemFn>
    void AddSystem() {
        AdmitParameters<SystemFn, CarrierType>();
        SystemGraphCore::AddSystem(MakeSystemInfo<SystemFn>());
    }

    template <auto SystemFn>
    auto AddSystemBefore(std::string_view beforeSystem) -> bool {
        AdmitParameters<SystemFn, CarrierType>();
        return SystemGraphCore::AddSystemBefore(MakeSystemInfo<SystemFn>(), beforeSystem);
    }

    void Execute(const ZHLN::Frame& frame) {
        CarrierType carrier {.registry = _registry, .services = _services, .frame = frame};
        SystemGraphCore::Execute(&carrier);
    }

    [[nodiscard]] auto GetRegistry() noexcept -> ECS::Registry& {
        return _registry;
    }

  private:
    static auto EmptyServices() noexcept -> Services& {
        static Services instance {};
        return instance;
    }

    template <auto SystemFn>
    static auto MakeSystemInfo() -> SystemInfo {
        using Signature = SystemSignature<SystemFn>;
        SystemInfo info;
        info.name        = Signature::NameCString();
        info.update_func = &CarrierThunk<SystemFn>;
        Signature::PopulateAccessPattern(info.access_pattern);
        // The signature's own description of the state it keeps between frames;
        // the node allocates the block from it. A system with no Local<T> gets an
        // empty layout and no allocation.
        info.local = TemplatedDetail::MakeLocalLayout<SystemFn>();
        return info;
    }

    // The erased boundary: the core calls this with the carrier it was executed
    // with and the storage it allocated for this node, and the concrete carrier
    // type comes back at the first instruction.
    //
    // The carrier is copied rather than mutated: it is shared by every node, some
    // of which run at the same time, so the node's state is attached to a local
    // copy for the duration of this one invocation.
    template <auto SystemFn>
    static void CarrierThunk(void* arg) {
        const auto* call    = static_cast<const SystemCall*>(arg);
        CarrierType carrier = *static_cast<const CarrierType*>(call->carrier);
        carrier.local       = TemplatedDetail::LocalStateView {.block = call->local, .slots = call->slots, .count = call->slotCount};
        SystemSignature<SystemFn>::template Invoke<ParameterResolver, CarrierType>(carrier);
    }

    ECS::Registry& _registry;
    Services&      _services;
};

} // namespace ZHLN::ECS
