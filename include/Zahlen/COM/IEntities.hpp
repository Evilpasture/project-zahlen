// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IEntities.hpp
//
// The ECS seam. A virtual function cannot be a template, so the vtable carries
// only type-erased operations keyed on a component's qualified name, and the
// typed API is rebuilt from inline templates that compile in the consumer's
// own translation unit. Nothing about the caller's component type needs to be
// known inside the library.
//
// The storage behind these is the registry's dynamic-component path, which
// holds raw bytes and runs no destructor. That is why the templates below
// require a trivially copyable component: it is the actual contract of the
// storage, not a boundary formality.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>

#include <cstdint>
#include <new>
#include <type_traits>

namespace ZHLN::COM {

template <typename T>
concept ComComponent = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

struct IEntities : IObject {
    [[nodiscard]] virtual Entity CreateEntity() noexcept = 0;
    virtual void                 Despawn(Entity entity) noexcept = 0;
    [[nodiscard]] virtual bool   IsAlive(Entity entity) const noexcept = 0;

    // `typeName` identifies the component family. The library hashes and
    // registers it on first use, so a consumer-defined component needs no
    // registration call and no shared header. Returns nullptr if the entity is
    // dead, or if the name is already taken by a C++ component family -- the
    // registry refuses to reinterpret typed storage as raw bytes.
    [[nodiscard]] virtual void* AttachRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment) noexcept = 0;
    virtual bool                DetachRaw(Entity entity, StringView typeName) noexcept = 0;
    [[nodiscard]] virtual void* GetRaw(Entity entity, StringView typeName) const noexcept = 0;

    // The engine's own components live in typed storage that its systems
    // iterate; the dynamic path above deliberately cannot reach them. These
    // three do, for the families the engine has chosen to publish.
    //
    // `layoutHash` is the caller's own fingerprint of the component. A name
    // the engine publishes is only served when size, alignment and hash all
    // agree with the definition the library was built against -- otherwise
    // the call is refused rather than served through a mismatched view.
    [[nodiscard]] virtual bool  IsEngineComponent(StringView typeName) const noexcept = 0;
    [[nodiscard]] virtual void* AttachEngineRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment,
                                                uint64_t layoutHash) noexcept = 0;
    virtual bool                DetachEngineRaw(Entity entity, StringView typeName) noexcept = 0;
    [[nodiscard]] virtual void* GetEngineRaw(Entity entity, StringView typeName) const noexcept = 0;

    // Compiled in the consumer's TU; not part of the vtable. The audit ignores
    // templates, and these never cross the boundary as symbols.
    template <ComComponent T>
    [[nodiscard]] auto Attach(Entity entity, const T& component) noexcept -> T* {
        if (IsEngineComponent(ComponentName<T>())) {
            // Already a constructed T in typed storage; assign, do not
            // placement-new over storage the registry is tracking. No
            // fallback on refusal -- a mismatch must surface, not degrade
            // into a parallel family nothing reads.
            void* storage = AttachEngineRaw(entity, ComponentName<T>(), sizeof(T), alignof(T), Reflect::LayoutHash<T>());
            if (storage == nullptr) return nullptr;
            *static_cast<T*>(storage) = component;
            return static_cast<T*>(storage);
        }
        void* storage = AttachRaw(entity, ComponentName<T>(), sizeof(T), alignof(T));
        if (storage == nullptr) return nullptr;
        return new (storage) T(component);
    }

    template <ComComponent T>
    [[nodiscard]] auto Get(Entity entity) const noexcept -> T* {
        if (IsEngineComponent(ComponentName<T>())) {
            return static_cast<T*>(GetEngineRaw(entity, ComponentName<T>()));
        }
        return static_cast<T*>(GetRaw(entity, ComponentName<T>()));
    }

    template <ComComponent T>
    auto Detach(Entity entity) noexcept -> bool {
        if (IsEngineComponent(ComponentName<T>())) {
            return DetachEngineRaw(entity, ComponentName<T>());
        }
        return DetachRaw(entity, ComponentName<T>());
    }

    // The qualified name, so two components called Transform in different
    // namespaces are different families.
    template <typename T>
    [[nodiscard]] static consteval auto ComponentName() noexcept -> StringView {
        return Reflect::QualifiedTypeName<T>();
    }

protected:
    ~IEntities() = default;
};

} // namespace ZHLN::COM
