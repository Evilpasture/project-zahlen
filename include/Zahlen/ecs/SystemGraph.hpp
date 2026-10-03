// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Reflection/Services.hpp>
#include <Zahlen/Core/SoA.hpp>
#include <Zahlen/Frame.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/SystemAccess.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <Zahlen/ecs/SystemSignature.hpp>
#include <cstdint>
#include <string_view>
#include <type_traits>
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
// The `typedef` names the services type so a resolver can ask, of the carrier it
// was handed, whether those services provide what it was asked to resolve.
template <typename ServicesT>
struct Carrier {
    using Services = ServicesT;

    ECS::Registry& registry;
    ServicesT&     services;
    const ZHLN::Frame& frame;
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
consteval auto EveryParameterResolvable(std::index_sequence<Is...>) -> bool {
    return (ResolvableFrom<CarrierT, typename Reflect::CallableInspector<Fn>::template ParameterType<Is>> && ...);
}

// A system whose signature this graph cannot satisfy is rejected here, as an
// immediate call at the AddSystem site: the diagnostic carries the system's
// identity through the instantiation trace, and there is no runtime
// equivalent -- by the time Execute runs, every parameter is known to exist.
template <auto SystemFn, typename CarrierT>
consteval void AdmitParameters() {
    static_assert(
        EveryParameterResolvable<CarrierT, SystemFn>(std::make_index_sequence<Reflect::CallableInspector<SystemFn>::ParameterCount()> {}),
        "A system parameter cannot be resolved by this graph. Either add the service it asks for to the graph's service bundle, "
        "or ask for something every graph resolves: Query<...>, Registry&, FrameDt/FrameAlpha/FrameIndex, Res<T>/ResMut<T>/"
        "Optional<T&> for a service the bundle provides, SoAScratch<T, N> or LinearArena&."
    );
}

using SystemFunc = void (*)(void*);

struct SystemInfo {
    SystemFunc                   update_func = nullptr;
    const char*                  name        = "UnnamedSystem";
    std::vector<ComponentAccess> access_pattern;
    bool                         enabled = true;
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
    struct Node {
        SystemInfo            info;
        std::vector<uint32_t> dependents;
        uint32_t              initialDependencyCount = 0;
    };

    struct ExecutionContext;
    struct NodePayload;

    static void TaskThunk(void* arg);
    void        DispatchNode(ExecutionContext& ctx, uint32_t nodeIdx);

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
        static_assert(Reflect::TemplatedDetail::BundleTypesAreUnique<Services>(),
                      "Two members of this graph's service bundle have the same type, so by-type resolution would silently pick one of "
                      "them: give the services distinct types.");
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
        return info;
    }

    // The erased boundary: the core calls this with the carrier it was executed
    // with, and the concrete carrier type comes back at the first instruction.
    template <auto SystemFn>
    static void CarrierThunk(void* carrier) {
        SystemSignature<SystemFn>::template Invoke<ParameterResolver, CarrierType>(*static_cast<CarrierType*>(carrier));
    }

    ECS::Registry& _registry;
    Services&      _services;
};

} // namespace ZHLN::ECS
