// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptECSBridge.hpp"
#include <Scripting/ScriptBinderRegistry.hpp>

namespace ZHLN {
ScriptECSBridge::ScriptECSBridge(COM::IEntities& entities): m_inspection(entities) {
    RegisterCoreScriptTypes();
    m_inspection.RegisterManifest<Components>(true);
}
auto ScriptECSBridge::ResolveValue(ScriptVal& value) const -> std::expected<void, ErrorCode> {
    if (auto* box = std::get_if<BoxedObject>(&value)) {
        auto ptr = ResolveBoxedPointer(*box);
        if (!ptr)
            return std::unexpected(ptr.error());
        box->rawPtr = *ptr;
    } else if (auto* array = std::get_if<ScriptArray>(&value)) {
        for (auto& element: array->elements)
            if (auto resolved = ResolveValue(element); !resolved)
                return resolved;
    }
    return {};
}

namespace {
void Stamp(ScriptVal& value, Entity entity, std::string_view component, std::vector<BoxedPathStep> path, const ScriptComponentAccess& access) {
    if (auto* box = std::get_if<BoxedObject>(&value)) {
        box->ownerEntity    = entity;
        box->compName       = component;
        box->propName       = path.empty() ? std::string {} : path.back().property;
        box->elementIndex   = path.empty() ? SIZE_MAX : path.back().index;
        box->path           = std::move(path);
        box->rawPtr         = nullptr;
        box->componentOwner = &access.Entities();
        box->retainedOwner  = access.Scope();
    } else if (auto* array = std::get_if<ScriptArray>(&value)) {
        for (size_t i = 0; i < array->elements.size(); ++i) {
            auto child = path;
            if (!child.empty()) {
                if (child.back().index == SIZE_MAX)
                    child.back().index = i;
                else
                    child.back().nestedIndices.push_back(i);
            }
            Stamp(array->elements[i], entity, component, std::move(child), access);
        }
    }
}
} // namespace
auto ScriptECSBridge::ResolveBoxedPointer(const BoxedObject& obj) const -> std::expected<void*, ErrorCode> {
    if (obj.ownerEntity == Entity::Null()) {
        if (!obj.rawPtr)
            return std::unexpected(ScriptError::ComponentNotFound);
        return obj.rawPtr;
    }
    if (obj.componentOwner != &m_inspection.Entities())
        return std::unexpected(ScriptError::EntityNotFound);
    if (!m_inspection.IsAlive(obj.ownerEntity))
        return std::unexpected(ScriptError::EntityNotFound);
    if (!m_inspection.IsRegistered(obj.compName))
        return std::unexpected(ScriptError::ComponentNotFound);
    void* current = m_inspection.Component(obj.ownerEntity, obj.compName);
    if (!current)
        return std::unexpected(ScriptError::ComponentNotFound);
    std::string_view type = obj.compName;
    if (obj.path.empty())
        return std::unexpected(ScriptError::PropertyNotFound);
    for (const auto& step: obj.path) {
        auto cls = ScriptBinder::Get().classes.find(type);
        if (cls == ScriptBinder::Get().classes.end())
            return std::unexpected(ScriptError::TypeNotFound);
        auto prop = cls->second.properties.find(step.property);
        if (prop == cls->second.properties.end())
            return std::unexpected(ScriptError::PropertyNotFound);
        std::expected<ScriptVal, ErrorCode> value;
        if (step.index == SIZE_MAX)
            value = prop->second.get(current);
        else if (prop->second.get_element_at)
            value = prop->second.get_element_at(current, step.index);
        else
            return std::unexpected(ScriptError::UnsupportedConversion);
        if (!value)
            return std::unexpected(value.error());
        for (auto index: step.nestedIndices) {
            auto* arr = std::get_if<ScriptArray>(&*value);
            if (!arr)
                return std::unexpected(ScriptError::TypeMismatch);
            if (index >= arr->elements.size())
                return std::unexpected(ScriptError::IndexOutOfBounds);
            ScriptVal next = arr->elements[index];
            value          = std::move(next);
        }
        auto* box = std::get_if<BoxedObject>(&*value);
        if (!box || !box->rawPtr)
            return std::unexpected(ScriptError::TypeMismatch);
        current = box->rawPtr;
        type    = box->typeName;
    }
    return current;
}

auto ScriptECSBridge::GetProperty(Entity entity, std::string_view compName, std::string_view propName) -> std::expected<ScriptVal, ErrorCode> {
    if (!m_inspection.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    if (!m_inspection.IsRegistered(compName)) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    const void* compPtr = m_inspection.Component(entity, compName);
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
        Stamp(val, entity, compName, {{std::string(propName), SIZE_MAX}}, m_inspection);
        return val;
    }

    return std::unexpected(ScriptError::PropertyNotFound);
}

auto ScriptECSBridge::GetPropertyOf(const ScriptVal& parentVal, std::string_view propName) const -> std::expected<ScriptVal, ErrorCode> {
    void*            parentPtr = nullptr;
    std::string_view typeName;
    Entity           ownerEnt = Entity::Null();
    std::string_view rootComp;

    if (const auto* boxed = std::get_if<BoxedObject>(&parentVal)) {
        auto resolved = ResolveBoxedPointer(*boxed);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        parentPtr = resolved.value();
        typeName  = boxed->typeName;
        ownerEnt  = boxed->ownerEntity;
        rootComp  = boxed->compName;
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
    if (ownerEnt != Entity::Null()) {
        auto path = std::get<BoxedObject>(parentVal).path;
        path.push_back({std::string(propName), SIZE_MAX});
        Stamp(outVal, ownerEnt, rootComp, std::move(path), m_inspection);
    }
    return outVal;
}

auto ScriptECSBridge::SetProperty(Entity entity, std::string_view compName, std::string_view propName, const ScriptVal& val)
    -> std::expected<void, ErrorCode> {
    if (!m_inspection.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    if (!m_inspection.IsRegistered(compName)) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    void* compPtr = m_inspection.Component(entity, compName);
    if (compPtr == nullptr) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    if (!m_inspection.CanWrite(compName))
        return std::unexpected(ScriptError::UnsupportedConversion);

    ScriptVal valueToAssign = val;
    if (auto resolved = ResolveValue(valueToAssign); !resolved)
        return std::unexpected(resolved.error());

    auto propIt = it->second.properties.find(propName);
    if (propIt != it->second.properties.end()) {
        return propIt->second.set(compPtr, valueToAssign);
    }

    return std::unexpected(ScriptError::PropertyNotFound);
}

auto ScriptECSBridge::SetPropertyOf(ScriptVal& parentVal, std::string_view propName, const ScriptVal& val) const -> std::expected<void, ErrorCode> {
    void*            parentPtr = nullptr;
    std::string_view typeName;

    if (auto* boxed = std::get_if<BoxedObject>(&parentVal)) {
        auto resolved = ResolveBoxedPointer(*boxed);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        if (boxed->ownerEntity != Entity::Null() && !m_inspection.CanWrite(boxed->compName))
            return std::unexpected(ScriptError::UnsupportedConversion);
        parentPtr = resolved.value();
        typeName  = boxed->typeName;
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
    if (auto resolved = ResolveValue(valueToAssign); !resolved)
        return std::unexpected(resolved.error());

    return propIt->second.set(parentPtr, valueToAssign);
}

auto ScriptECSBridge::GetArrayElement(const ScriptVal& arrayVal, size_t index) -> std::expected<ScriptVal, ErrorCode> {
    if (const auto* arr = std::get_if<ScriptArray>(&arrayVal)) {
        if (index >= arr->elements.size()) {
            return std::unexpected(ScriptError::IndexOutOfBounds);
        }
        return arr->elements[index];
    }
    return std::unexpected(ScriptError::TypeMismatch);
}

auto ScriptECSBridge::SetArrayElement(ScriptVal& arrayVal, size_t index, const ScriptVal& val) -> std::expected<void, ErrorCode> {
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
    -> std::expected<ScriptVal, ErrorCode> {
    if (!m_inspection.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    if (!m_inspection.IsRegistered(compName)) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    const void* compPtr = m_inspection.Component(entity, compName);
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
    Stamp(val, entity, compName, {{std::string(propName), index}}, m_inspection);

    return val;
}

auto ScriptECSBridge::SetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index, const ScriptVal& val)
    -> std::expected<void, ErrorCode> {
    if (!m_inspection.IsAlive(entity)) {
        return std::unexpected(ScriptError::EntityNotFound);
    }

    const auto& registry = ScriptBinder::Get().classes;
    auto        it       = registry.find(compName);
    if (it == registry.end()) {
        return std::unexpected(ScriptError::TypeNotFound);
    }

    if (!m_inspection.IsRegistered(compName)) {
        return std::unexpected(ScriptError::ComponentNotFound);
    }

    void* compPtr = m_inspection.Component(entity, compName);
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

    if (!m_inspection.CanWrite(compName))
        return std::unexpected(ScriptError::UnsupportedConversion);

    ScriptVal valueToAssign = val;
    if (auto resolved = ResolveValue(valueToAssign); !resolved)
        return std::unexpected(resolved.error());

    return propIt->second.set_element_at(compPtr, index, valueToAssign);
}

} // namespace ZHLN
