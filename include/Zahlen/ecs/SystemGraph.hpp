// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/ecs/SystemAccess.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <Zahlen/ecs/SystemSignature.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN::ECS {

// Parameters are deliberately matched by *exact* type. In particular a plain
// float cannot accidentally select dt instead of alpha, and a Res/Query
// reference cannot bind to a short-lived resolved value.
template <typename Param>
struct ParameterResolver {
    static_assert(!std::is_same_v<Param, Param>, "Unrecognized system parameter type (use Query, Res, OptionRes or a tagged frame value)");
};

template <typename... Comps>
struct ParameterResolver<Query<Comps...>> {
    static auto Resolve(SystemContext& ctx) noexcept -> Query<Comps...> { return Query<Comps...>(ctx.registry); }
};

template <>
struct ParameterResolver<Registry&> {
    static auto Resolve(SystemContext& ctx) noexcept -> Registry& { return ctx.registry; }
};

template <>
struct ParameterResolver<const Registry&> {
    static auto Resolve(SystemContext& ctx) noexcept -> const Registry& { return ctx.registry; }
};

template <>
struct ParameterResolver<FrameDt> {
    static auto Resolve(SystemContext& ctx) noexcept -> FrameDt { return {ctx.dt}; }
};

template <>
struct ParameterResolver<FrameAlpha> {
    static auto Resolve(SystemContext& ctx) noexcept -> FrameAlpha { return {ctx.alpha}; }
};

template <>
struct ParameterResolver<FrameIndex> {
    static auto Resolve(SystemContext& ctx) noexcept -> FrameIndex { return {ctx.frame}; }
};

namespace TemplatedDetail {

template <typename T>
struct ResourceSlot {
    static auto Get(SystemContext& ctx) noexcept -> T* {
        if constexpr (std::is_same_v<T, RenderContext>) {
            return ctx.render;
        } else if constexpr (std::is_same_v<T, PhysicsContext>) {
            return ctx.physics;
        } else if constexpr (std::is_same_v<T, AudioContext>) {
            return ctx.audio;
        } else if constexpr (std::is_same_v<T, Camera>) {
            return ctx.camera;
        } else if constexpr (std::is_same_v<T, CullingSystem>) {
            return ctx.culling;
        } else if constexpr (std::is_same_v<T, ArticulationSystem>) {
            return ctx.articulation;
        } else {
            static_assert(!std::is_same_v<T, T>, "Resource is not a service provided by SystemContext");
        }
    }
};

} // namespace TemplatedDetail

template <typename T>
struct ParameterResolver<Res<T>> {
    static auto Resolve(SystemContext& ctx) -> Res<T> {
        const T* ptr = TemplatedDetail::ResourceSlot<T>::Get(ctx);
        ZHLN::Assert(ptr != nullptr, "System requires a service which is absent from SystemContext");
        return {ptr};
    }
};

template <typename T>
struct ParameterResolver<ResMut<T>> {
    static auto Resolve(SystemContext& ctx) -> ResMut<T> {
        T* ptr = TemplatedDetail::ResourceSlot<T>::Get(ctx);
        ZHLN::Assert(ptr != nullptr, "System requires a service which is absent from SystemContext");
        return {ptr};
    }
};

template <typename T>
struct ParameterResolver<OptionRes<T>> {
    static auto Resolve(SystemContext& ctx) noexcept -> OptionRes<T> { return {TemplatedDetail::ResourceSlot<T>::Get(ctx)}; }
};

template <>
struct ParameterResolver<BonePosePostProcessor> {
    static auto Resolve(SystemContext& ctx) noexcept -> BonePosePostProcessor { return ctx.bonePosePostProcessor; }
};

template <>
struct ParameterResolver<VisibleEntities> {
    static auto Resolve(SystemContext& ctx) -> VisibleEntities {
        ZHLN::Assert(ctx.visibleEntities != nullptr, "System requires the visible-entities output list");
        return {*ctx.visibleEntities};
    }
};

template <>
struct ParameterResolver<VisibleShadowEntities> {
    static auto Resolve(SystemContext& ctx) -> VisibleShadowEntities {
        ZHLN::Assert(ctx.visibleShadowEntities != nullptr, "System requires the shadow-visible output list");
        return {*ctx.visibleShadowEntities};
    }
};

using SystemFunc = void (*)(ZHLN::SystemContext&);

struct SystemInfo {
    SystemFunc                   update_func = nullptr;
    const char*                  name        = "UnnamedSystem";
    std::vector<ComponentAccess> access_pattern;
    bool                         enabled = true;
};

class ZHLN_API SystemGraph {
  public:
    SystemGraph()  = default;
    ~SystemGraph() = default;

    SystemGraph(const SystemGraph&)                        = delete;
    auto operator=(const SystemGraph&) -> SystemGraph&     = delete;
    SystemGraph(SystemGraph&&) noexcept                    = default;
    auto operator=(SystemGraph&&) noexcept -> SystemGraph& = default;

    // The legacy API remains for extension authors who cannot yet express
    // their component accesses in their signature.
    void AddSystem(SystemInfo info);
    auto AddSystemBefore(SystemInfo info, std::string_view beforeSystem) -> bool;

    template <auto SystemFn>
    void AddSystem() { AddSystem(MakeSystemInfo<SystemFn>()); }

    template <auto SystemFn>
    auto AddSystemBefore(std::string_view beforeSystem) -> bool { return AddSystemBefore(MakeSystemInfo<SystemFn>(), beforeSystem); }

    void DeclareExternalWrites(const char* label, std::vector<ComponentAccess> accesses);

    void Compile();
    void Execute(ZHLN::SystemContext& ctx);

    void               SetSystemEnabled(std::string_view name, bool enabled) noexcept;
    [[nodiscard]] auto IsSystemEnabled(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto GetSystemCount() const noexcept -> size_t;
    [[nodiscard]] auto IsEmpty() const noexcept -> bool;
    void               Clear() noexcept;

    [[nodiscard]] static auto HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) noexcept -> bool;

  private:
    template <auto SystemFn>
    static auto MakeSystemInfo() -> SystemInfo {
        using Signature = SystemSignature<SystemFn>;
        SystemInfo info;
        info.name        = Signature::NameCString();
        info.update_func = &Signature::template Invoke<ParameterResolver, ZHLN::SystemContext>;
        Signature::PopulateAccessPattern(info.access_pattern);
        return info;
    }

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

} // namespace ZHLN::ECS
