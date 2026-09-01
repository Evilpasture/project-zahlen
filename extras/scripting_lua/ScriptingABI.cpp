/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/scripting_lua/ScriptingABI.cpp
//
// The shim between LuaJIT's ffi and ScriptBinder.
//
// Everything here exists because the two sides cannot share types. ScriptVal
// owns heap memory and is not trivially copyable; ZHLN_ScriptVal is POD so
// ffi.cdef can describe it. The conversions are the whole job.
//
// Lifetime: values handed back to Lua point into thread-local storage that is
// reused by the next call on the same thread. A script must copy out what it
// wants before making another call, which is the usual contract for a C shim
// and is why nothing here allocates on the caller's behalf.

#include "ScriptingABI.h"
#include "ScriptBinder.hpp"
#include "ScriptBinderRegistry.hpp"

#include <Zahlen/Core/Reflection.hpp>

#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

    using ZHLN::BoxedObject;
    using ZHLN::OwnedObject;
    using ZHLN::ScriptArray;
    using ZHLN::ScriptError;
    using ZHLN::ScriptVal;

    /// Total number of ZHLN_ScriptVal nodes a value tree needs, including the
    /// elements of every nested array. Counting up front lets the backing
    /// vector be reserved once, so no pointer handed to Lua is invalidated by a
    /// later reallocation during the recursion.
    auto countNodes(const ScriptVal& value) -> std::size_t {
        const auto* arr = std::get_if<ScriptArray>(&value);
        if (arr == nullptr) {
            return 0;
        }
        std::size_t total = arr->elements.size();
        for (const auto& element: arr->elements) {
            total += countNodes(element);
        }
        return total;
    }

    // Storage backing the ZHLN_ScriptVal values returned to Lua. Reset at the
    // start of each entry point, so results stay valid until the next call.
    //
    // Strings live in a deque of std::string rather than one growing buffer:
    // a deque never moves the elements it already holds, so every c_str()
    // handed out stays valid for the life of the scratch area.
    struct Scratch {
        std::deque<std::string>            strings;
        std::vector<ZHLN_ScriptVal>        arrays;
        std::vector<std::shared_ptr<void>> owned; // keeps OwnedObject pointees alive

        void reset() {
            strings.clear();
            arrays.clear();
            owned.clear();
        }

        auto intern(std::string_view text) -> const char* {
            strings.emplace_back(text);
            return strings.back().c_str();
        }
    };

    auto scratch() -> Scratch& {
        thread_local Scratch instance;
        return instance;
    }

    /// Unpack the ScriptError out of a ZHLN::Error. Error is category-tagged,
    /// so only read it as a ScriptError when it is one; anything else is a
    /// failure whose specific code this layer cannot name.
    auto errorOf(const ZHLN::Error& err) -> uint32_t {
        if (err.Is<ScriptError>()) {
            return static_cast<uint32_t>(err.As<ScriptError>());
        }
        return static_cast<uint32_t>(ScriptError::UnsupportedConversion);
    }

    auto toCVal(const ScriptVal& value) -> ZHLN_ScriptVal {
        auto& mem = scratch();
        ZHLN_ScriptVal out {};
        out.kind  = ZHLN_ValNil;
        out.error = 0;

        if (std::holds_alternative<std::monostate>(value)) {
            out.kind = ZHLN_ValNil;
        } else if (const auto* d = std::get_if<double>(&value)) {
            out.kind        = ZHLN_ValNumber;
            out.as.number   = *d;
        } else if (const auto* b = std::get_if<bool>(&value)) {
            out.kind         = ZHLN_ValBool;
            out.as.boolean   = *b ? 1 : 0;
        } else if (const auto* s = std::get_if<std::string>(&value)) {
            out.kind        = ZHLN_ValString;
            out.as.str.data = mem.intern(*s);
            out.as.str.len  = s->size();
        } else if (const auto* boxed = std::get_if<BoxedObject>(&value)) {
            out.kind                = ZHLN_ValObject;
            out.as.object.typeName     = mem.intern(boxed->typeName);
            out.as.object.entity       = boxed->ownerEntity.Pack();
            out.as.object.compName     = mem.intern(boxed->compName);
            out.as.object.propName     = mem.intern(boxed->propName);
            out.as.object.elementIndex = static_cast<uint64_t>(boxed->elementIndex);
            out.as.object.ptr          = boxed->rawPtr;
        } else if (const auto* owned = std::get_if<OwnedObject>(&value)) {
            // The shared_ptr dies with the ScriptVal, so retain it. Lua sees a
            // raw pointer that stays valid until the next call.
            out.kind                = ZHLN_ValObject;
            out.as.object.typeName  = mem.intern(owned->typeName);
            out.as.object.entity    = 0;
            out.as.object.compName  = mem.intern({});
            out.as.object.propName  = mem.intern({});
            out.as.object.elementIndex = SIZE_MAX;
            out.as.object.ptr       = owned->ptr.get();
            mem.owned.push_back(owned->ptr);
        } else if (const auto* arr = std::get_if<ScriptArray>(&value)) {
            out.kind = ZHLN_ValArray;
            // `arrays` was reserved for the whole tree by the entry point, so
            // appending here cannot reallocate and invalidate the `items`
            // pointers already handed out for nested arrays.
            const auto base = mem.arrays.size();
            mem.arrays.resize(base + arr->elements.size());
            for (std::size_t i = 0; i < arr->elements.size(); ++i) {
                mem.arrays[base + i] = toCVal(arr->elements[i]);
            }
            out.as.array.items = mem.arrays.data() + base;
            out.as.array.count = arr->elements.size();
        }

        return out;
    }

    auto fromCVal(const ZHLN_ScriptVal& value) -> ScriptVal {
        switch (value.kind) {
        case ZHLN_ValNumber:
            return ScriptVal {value.as.number};
        case ZHLN_ValBool:
            return ScriptVal {value.as.boolean != 0};
        case ZHLN_ValString:
            return ScriptVal {std::string(value.as.str.data ? value.as.str.data : "",
                                          static_cast<std::size_t>(value.as.str.len))};
        case ZHLN_ValObject: {
            BoxedObject boxed {};
            boxed.typeName     = value.as.object.typeName ? value.as.object.typeName : "";
            boxed.ownerEntity  = ZHLN::Entity::Unpack(value.as.object.entity);
            boxed.compName     = value.as.object.compName ? value.as.object.compName : "";
            boxed.propName     = value.as.object.propName ? value.as.object.propName : "";
            boxed.elementIndex = static_cast<std::size_t>(value.as.object.elementIndex);
            boxed.rawPtr       = value.as.object.ptr;
            return ScriptVal {boxed};
        }
        case ZHLN_ValArray: {
            ScriptArray arr;
            for (uint64_t i = 0; i < value.as.array.count; ++i) {
                arr.elements.push_back(fromCVal(value.as.array.items[i]));
            }
            return ScriptVal {arr};
        }
        default:
            return ScriptVal {std::monostate {}};
        }
    }

    auto findClass(const char* className) -> const ZHLN::ScriptClassInfo* {
        if (className == nullptr) {
            return nullptr;
        }
        const auto& classes = ZHLN::ScriptBinder::Get().classes;
        const auto  it      = classes.find(std::string_view(className));
        return it == classes.end() ? nullptr : &it->second;
    }

    auto fail(ZHLN_ScriptVal* outResult, ScriptError code) -> ZHLN_ScriptStatus {
        if (outResult != nullptr) {
            *outResult           = ZHLN_ScriptVal {};
            outResult->kind      = ZHLN_ValNil;
            outResult->error     = static_cast<uint32_t>(code);
        }
        return ZHLN_ScriptFail;
    }

} // namespace

extern "C" {

ZHLN_ScriptStatus ZHLN_InvokeMethod(const char* className, const char* methodName, void* instance,
                                    const ZHLN_ScriptVal* args, uint64_t argCount,
                                    ZHLN_ScriptVal* outResult) {
    scratch().reset();

    const auto* info = findClass(className);
    if (info == nullptr) {
        return fail(outResult, ScriptError::TypeNotFound);
    }
    if (methodName == nullptr || instance == nullptr) {
        return fail(outResult, ScriptError::MethodNotFound);
    }

    std::vector<ScriptVal> converted;
    converted.reserve(static_cast<std::size_t>(argCount));
    for (uint64_t i = 0; i < argCount; ++i) {
        converted.push_back(fromCVal(args[i]));
    }

    const auto result =
        info->InvokeMethod(instance, methodName, std::span<const ScriptVal>(converted));
    if (!result) {
        if (outResult != nullptr) {
            *outResult       = ZHLN_ScriptVal {};
            outResult->kind  = ZHLN_ValNil;
            outResult->error = errorOf(result.error());
        }
        return ZHLN_ScriptFail;
    }

    if (outResult != nullptr) {
        // One reservation for the entire tree; see countNodes.
        scratch().arrays.reserve(scratch().arrays.size() + countNodes(result.value()));
        *outResult = toCVal(result.value());
    }
    return ZHLN_ScriptOk;
}

ZHLN_ScriptStatus ZHLN_GetProperty(const char* className, void* instance, const char* propertyName,
                                   uint64_t elementIndex, ZHLN_ScriptVal* outResult) {
    scratch().reset();

    const auto* info = findClass(className);
    if (info == nullptr) {
        return fail(outResult, ScriptError::TypeNotFound);
    }
    if (propertyName == nullptr || instance == nullptr || outResult == nullptr) {
        return fail(outResult, ScriptError::PropertyNotFound);
    }

    const auto propIt = info->properties.find(std::string_view(propertyName));
    if (propIt == info->properties.end()) {
        return fail(outResult, ScriptError::PropertyNotFound);
    }

    const auto result = (elementIndex == SIZE_MAX)
                            ? propIt->second.get(instance)
                            : (propIt->second.get_element_at
                                   ? propIt->second.get_element_at(instance,
                                                                   static_cast<std::size_t>(elementIndex))
                                   : std::expected<ScriptVal, ZHLN::Error> {
                                         std::unexpected(ScriptError::UnsupportedConversion)});
    if (!result) {
        outResult->error = errorOf(result.error());
        return ZHLN_ScriptFail;
    }

    scratch().arrays.reserve(scratch().arrays.size() + countNodes(result.value()));
    *outResult = toCVal(result.value());
    return ZHLN_ScriptOk;
}

ZHLN_ScriptStatus ZHLN_SetProperty(const char* className, void* instance, const char* propertyName,
                                   uint64_t elementIndex, const ZHLN_ScriptVal* value) {
    scratch().reset();

    const auto* info = findClass(className);
    if (info == nullptr) {
        return ZHLN_ScriptFail;
    }
    if (propertyName == nullptr || instance == nullptr || value == nullptr) {
        return ZHLN_ScriptFail;
    }

    const auto propIt = info->properties.find(std::string_view(propertyName));
    if (propIt == info->properties.end()) {
        return ZHLN_ScriptFail;
    }

    const auto converted = fromCVal(*value);
    const auto result    = (elementIndex == SIZE_MAX)
                               ? propIt->second.set(instance, converted)
                            : (propIt->second.set_element_at
                                   ? propIt->second.set_element_at(
                                         instance, static_cast<std::size_t>(elementIndex), converted)
                                   : std::expected<void, ZHLN::Error> {
                                         std::unexpected(ScriptError::UnsupportedConversion)});
    return result ? ZHLN_ScriptOk : ZHLN_ScriptFail;
}

uint64_t ZHLN_RegisterCoreScriptTypes(void) {
    return static_cast<uint64_t>(ZHLN::RegisterCoreScriptTypes());
}

const char* ZHLN_ScriptErrorName(uint32_t error) {
    const auto value = static_cast<ScriptError>(error);
    switch (value) {
    case ScriptError::EntityNotFound:        return "EntityNotFound";
    case ScriptError::TypeNotFound:          return "TypeNotFound";
    case ScriptError::ComponentNotFound:     return "ComponentNotFound";
    case ScriptError::PropertyNotFound:      return "PropertyNotFound";
    case ScriptError::MethodNotFound:        return "MethodNotFound";
    case ScriptError::TypeMismatch:          return "TypeMismatch";
    case ScriptError::ArityMismatch:         return "ArityMismatch";
    case ScriptError::InvalidEnumString:     return "InvalidEnumString";
    case ScriptError::IndexOutOfBounds:      return "IndexOutOfBounds";
    case ScriptError::UnsupportedConversion: return "UnsupportedConversion";
    }
    return "None";
}

} // extern "C"
