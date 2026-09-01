// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptECSBridge.hpp"

namespace ZHLN {

auto ScriptECSBridge::ResolveBoxedPointer(const BoxedObject& obj) const -> std::expected<void*, Error> {
    // Path A: Stable ECS Handle Re-resolution
    if (obj.ownerEntity != Entity::Null()) {
        if (!m_registry.IsAlive(obj.ownerEntity)) {
            return std::unexpected(ScriptError::EntityNotFound);
        }

        uint32_t familyID = ECS::Registry::GetFamilyIDFromName(obj.compName);
        if (familyID == 0xFFFFFFFF) {
            return std::unexpected(ScriptError::ComponentNotFound);
        }

        void* compPtr = m_registry.GetRawByFamily(obj.ownerEntity, familyID);
        if (compPtr == nullptr) {
            return std::unexpected(ScriptError::ComponentNotFound);
        }

        const auto& registry = ScriptBinder::Get().classes;
        auto        classIt  = registry.find(obj.compName);
        if (classIt == registry.end()) {
            return std::unexpected(ScriptError::TypeNotFound);
        }

        auto propIt = classIt->second.properties.find(obj.propName);
        if (propIt == classIt->second.properties.end()) {
            return std::unexpected(ScriptError::PropertyNotFound);
        }

        if (obj.elementIndex != SIZE_MAX) {
            if (!propIt->second.get_element_at) {
                return std::unexpected(ScriptError::UnsupportedConversion);
            }
            auto res = propIt->second.get_element_at(compPtr, obj.elementIndex);
            if (!res) {
                return std::unexpected(res.error());
            }

            if (const auto* freshlyBoxed = std::get_if<BoxedObject>(&res.value())) {
                return freshlyBoxed->rawPtr;
            }
            return std::unexpected(ScriptError::TypeMismatch);
        }

        auto res = propIt->second.get(compPtr);
        if (!res) {
            return std::unexpected(res.error());
        }

        if (const auto* freshlyBoxed = std::get_if<BoxedObject>(&res.value())) {
            return freshlyBoxed->rawPtr;
        }

        return std::unexpected(ScriptError::TypeMismatch);
    }

    if (obj.rawPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }
    return obj.rawPtr;
}

auto ScriptECSBridge::GetProperty(Entity entity, std::string_view compName, std::string_view propName) -> std::expected<ScriptVal, Error> {
    if (!m_registry.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    uint32_t familyID = ECS::Registry::GetFamilyIDFromName(compName);
    if (familyID == 0xFFFFFFFF) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    const void* compPtr = m_registry.GetRawByFamily(entity, familyID);
    if (compPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    auto propIt = it->second.properties.find(propName);
    if (propIt != it->second.properties.end()) {
        auto res = propIt->second.get(compPtr);
        if (!res) {
            return std::unexpected(res.error());
        }

        ScriptVal val = res.value();
        if (auto* boxed = std::get_if<BoxedObject>(&val)) {
            boxed->ownerEntity = entity;
            boxed->compName    = compName;
            boxed->propName    = propName;
        }
        return val;
    }

    return std::unexpected(ScriptError::PropertyNotFound);
}

auto ScriptECSBridge::GetPropertyOf(const ScriptVal& parentVal, std::string_view propName) const -> std::expected<ScriptVal, Error> {
    void*                 parentPtr = nullptr;
    std::string_view      typeName;
    Entity                ownerEnt = Entity::Null();
    std::string_view      rootComp;
    std::shared_ptr<void> parentOwnedKeepAlive = nullptr;

    if (const auto* boxed = std::get_if<BoxedObject>(&parentVal)) {
        auto resolved = ResolveBoxedPointer(*boxed);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        parentPtr = resolved.value();
        typeName  = boxed->typeName;
        ownerEnt  = boxed->ownerEntity;
        rootComp  = boxed->compName;
    } else if (const auto* owned = std::get_if<OwnedObject>(&parentVal)) {
        parentPtr            = owned->ptr.get();
        typeName             = owned->typeName;
        parentOwnedKeepAlive = owned->ptr;
    } else {
        return std::unexpected(ScriptError::TypeMismatch);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        classIt  = registry.find(typeName);
    if (classIt == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    auto propIt = classIt->second.properties.find(propName);
    if (propIt == classIt->second.properties.end()) {
        return std::unexpected(ScriptError::PropertyNotFound);
    }

    auto res = propIt->second.get(parentPtr);
    if (!res) {
        return res;
    }

    ScriptVal outVal = res.value();
    if (auto* childBoxed = std::get_if<BoxedObject>(&outVal)) {
        if (parentOwnedKeepAlive) {
            std::shared_ptr<void> aliasedPtr(parentOwnedKeepAlive, childBoxed->rawPtr);
            return OwnedObject {.typeName = childBoxed->typeName, .ptr = aliasedPtr};
        }

        childBoxed->ownerEntity = ownerEnt;
        childBoxed->compName    = rootComp;
        childBoxed->propName    = propName;
    }
    return outVal;
}

auto ScriptECSBridge::SetProperty(Entity entity, std::string_view compName, std::string_view propName, const ScriptVal& val) -> std::expected<void, Error> {
    if (!m_registry.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    uint32_t familyID = ECS::Registry::GetFamilyIDFromName(compName);
    if (familyID == 0xFFFFFFFF) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    void* compPtr = m_registry.GetRawByFamily(entity, familyID);
    if (compPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    ScriptVal valueToAssign = val;
    if (auto* boxed = std::get_if<BoxedObject>(&valueToAssign)) {
        auto resolvedPtr = ResolveBoxedPointer(*boxed);
        if (!resolvedPtr) {
            return std::unexpected(resolvedPtr.error());
        }
        boxed->rawPtr = resolvedPtr.value();
    }

    auto propIt = it->second.properties.find(propName);
    if (propIt != it->second.properties.end()) {
        return propIt->second.set(compPtr, valueToAssign);
    }

    return std::unexpected(ScriptError::PropertyNotFound);
}

auto ScriptECSBridge::SetPropertyOf(ScriptVal& parentVal, std::string_view propName, const ScriptVal& val) const -> std::expected<void, Error> {
    void*            parentPtr = nullptr;
    std::string_view typeName;

    if (auto* boxed = std::get_if<BoxedObject>(&parentVal)) {
        auto resolved = ResolveBoxedPointer(*boxed);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        parentPtr = resolved.value();
        typeName  = boxed->typeName;
    } else if (auto* owned = std::get_if<OwnedObject>(&parentVal)) {
        parentPtr = owned->ptr.get();
        typeName  = owned->typeName;
    } else {
        return std::unexpected(ScriptError::TypeMismatch);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        classIt  = registry.find(typeName);
    if (classIt == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    auto propIt = classIt->second.properties.find(propName);
    if (propIt == classIt->second.properties.end()) {
        return std::unexpected(ScriptError::PropertyNotFound);
    }

    ScriptVal valueToAssign = val;
    if (auto* valueBoxed = std::get_if<BoxedObject>(&valueToAssign)) {
        auto resolvedPtr = ResolveBoxedPointer(*valueBoxed);
        if (!resolvedPtr) {
            return std::unexpected(resolvedPtr.error());
        }
        valueBoxed->rawPtr = resolvedPtr.value();
    }

    return propIt->second.set(parentPtr, valueToAssign);
}

auto ScriptECSBridge::CallMethod(Entity entity, std::string_view compName, std::string_view methodName, std::span<const ScriptVal> args)
    -> std::expected<ScriptVal, Error> {
    if (!m_registry.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    uint32_t familyID = ECS::Registry::GetFamilyIDFromName(compName);
    if (familyID == 0xFFFFFFFF) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    void* compPtr = m_registry.GetRawByFamily(entity, familyID);
    if (compPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    std::vector<ScriptVal> resolvedArgs(args.begin(), args.end());
    for (auto& arg: resolvedArgs) {
        if (auto* boxed = std::get_if<BoxedObject>(&arg)) {
            auto resolvedPtr = ResolveBoxedPointer(*boxed);
            if (!resolvedPtr) {
                return std::unexpected(resolvedPtr.error());
            }
            boxed->rawPtr = resolvedPtr.value();
        }
    }

    return it->second.InvokeMethod(compPtr, methodName, resolvedArgs);
}

auto ScriptECSBridge::GetArrayElement(const ScriptVal& arrayVal, size_t index) -> std::expected<ScriptVal, Error> {
    if (const auto* arr = std::get_if<ScriptArray>(&arrayVal)) {
        if (index >= arr->elements.size()) {
            return std::unexpected(ScriptError::IndexOutOfBounds);
        }
        return arr->elements[index];
    }
    return std::unexpected(ScriptError::TypeMismatch);
}

auto ScriptECSBridge::SetArrayElement(ScriptVal& arrayVal, size_t index, const ScriptVal& val) -> std::expected<void, Error> {
    if (auto* arr = std::get_if<ScriptArray>(&arrayVal)) {
        if (index >= arr->elements.size()) {
            return std::unexpected(ScriptError::IndexOutOfBounds);
        }
        arr->elements[index] = val;
        return {};
    }
    return std::unexpected(ScriptError::TypeMismatch);
}

auto ScriptECSBridge::GetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index)
    -> std::expected<ScriptVal, Error> {
    if (!m_registry.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    uint32_t familyID = ECS::Registry::GetFamilyIDFromName(compName);
    if (familyID == 0xFFFFFFFF) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    const void* compPtr = m_registry.GetRawByFamily(entity, familyID);
    if (compPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    auto propIt = it->second.properties.find(propName);
    if (propIt == it->second.properties.end()) {
        return std::unexpected(ScriptError::PropertyNotFound);
    }

    if (!propIt->second.get_element_at) {
        return std::unexpected(ScriptError::UnsupportedConversion);
    }

    auto res = propIt->second.get_element_at(compPtr, index);
    if (!res) {
        return std::unexpected(res.error());
    }

    ScriptVal val = res.value();
    if (auto* boxed = std::get_if<BoxedObject>(&val)) {
        boxed->ownerEntity  = entity;
        boxed->compName     = compName;
        boxed->propName     = propName;
        boxed->elementIndex = index;
    }

    return val;
}

auto ScriptECSBridge::SetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index, const ScriptVal& val)
    -> std::expected<void, Error> {
    if (!m_registry.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    uint32_t familyID = ECS::Registry::GetFamilyIDFromName(compName);
    if (familyID == 0xFFFFFFFF) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    void* compPtr = m_registry.GetRawByFamily(entity, familyID);
    if (compPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    auto propIt = it->second.properties.find(propName);
    if (propIt == it->second.properties.end()) {
        return std::unexpected(ScriptError::PropertyNotFound);
    }

    if (!propIt->second.set_element_at) {
        return std::unexpected(ScriptError::UnsupportedConversion);
    }

    ScriptVal valueToAssign = val;
    if (auto* valueBoxed = std::get_if<BoxedObject>(&valueToAssign)) {
        auto resolvedPtr = ResolveBoxedPointer(*valueBoxed);
        if (!resolvedPtr) {
            return std::unexpected(resolvedPtr.error());
        }
        valueBoxed->rawPtr = resolvedPtr.value();
    }

    return propIt->second.set_element_at(compPtr, index, valueToAssign);
}

} // namespace ZHLN
