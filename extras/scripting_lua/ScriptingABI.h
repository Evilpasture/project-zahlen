/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/scripting_lua/ScriptingABI.h
//
// The C ABI that LuaJIT's ffi.C calls through. It lives here, not in core: an
// integer command ID, a void* argument blob and a jump table are how a dynamic
// language reaches C++, and core has no reason to know any of that exists.
// The declarations mirror the ffi.cdef block the Lua side loads.

#pragma once

#include <Zahlen/Common.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZHLN_API struct ZHLN_Engine* ZHLN_GetEngineContext(void);

// 1. Interns the string once and assigns it a fast array index
ZHLN_API uint32_t ZHLN_GetCommandID(const char* cmdName);

// 2. Dispatches via O(1) jump table
ZHLN_API uint64_t ZHLN_DispatchCommand(struct ZHLN_Engine* engine, uint32_t cmdID, const void* args);

// ---------------------------------------------------------------------------
// Reflection-driven access through ScriptBinder.
//
// Three constraints shaped this, all of them visible in the C++ it wraps:
//
//   ScriptVal is 80 bytes and holds a std::string, a std::vector and a
//   std::shared_ptr. It is not trivially copyable, so ffi.cdef cannot describe
//   it and Lua cannot allocate one. ZHLN_ScriptVal is the POD stand-in that
//   crosses the boundary; the shim converts.
//
//   ZHLN::Error static_asserts that an error enum has no 0 enumerator, and
//   traps at runtime if one is constructed, because success is an engaged
//   std::expected rather than a code. So ScriptError cannot carry an Ok, and
//   the boundary needs its own status type.
//
//   Failure still has to say which failure. The `error` field carries the
//   ScriptError enumerator, which Lua can compare or turn into a message.
// ---------------------------------------------------------------------------

typedef enum ZHLN_ScriptStatus {
    ZHLN_ScriptOk   = 0,
    ZHLN_ScriptFail = 1
} ZHLN_ScriptStatus;

// Which member of the union below is live.
enum {
    ZHLN_ValNil    = 0,
    ZHLN_ValNumber = 1,
    ZHLN_ValBool   = 2,
    ZHLN_ValString = 3,
    ZHLN_ValObject = 4,
    ZHLN_ValArray  = 5
};

// Mirrors ScriptBinder's BoxedObject: a handle on a property of a component,
// which is how a script holds a reference to something it does not own.
typedef struct ZHLN_ScriptObjectRef {
    const char* typeName;
    uint64_t    entity;
    const char* compName;
    const char* propName;
    uint64_t    elementIndex; // SIZE_MAX when not addressing an element
    void*       ptr;
} ZHLN_ScriptObjectRef;

typedef struct ZHLN_ScriptArrayRef {
    struct ZHLN_ScriptVal* items;
    uint64_t               count;
} ZHLN_ScriptArrayRef;

typedef struct ZHLN_ScriptVal {
    uint32_t kind;
    uint32_t error; // a ScriptError code; meaningful when a call returns Fail
    union {
        double number;
        int32_t boolean;
        struct {
            const char* data;
            uint64_t    len;
        } str;
        ZHLN_ScriptObjectRef object;
        ZHLN_ScriptArrayRef  array;
    } as;
} ZHLN_ScriptVal;

// Invoke a method by class name and method name on an instance the caller
// already has a pointer to. Overloads are resolved by argument count, which is
// what ScriptClassInfo::InvokeMethod does.
//
// `instance` must point at a live object of `className`; the binder casts it
// without being able to check. outResult may be NULL when the return value is
// not wanted.
ZHLN_API ZHLN_ScriptStatus ZHLN_InvokeMethod(const char* className, const char* methodName,
                                             void* instance, const ZHLN_ScriptVal* args,
                                             uint64_t argCount, ZHLN_ScriptVal* outResult);

// Read and write a property by name. set_element_at / get_element_at are used
// when elementIndex is not SIZE_MAX.
ZHLN_API ZHLN_ScriptStatus ZHLN_GetProperty(const char* className, void* instance,
                                            const char* propertyName, uint64_t elementIndex,
                                            ZHLN_ScriptVal* outResult);

ZHLN_API ZHLN_ScriptStatus ZHLN_SetProperty(const char* className, void* instance,
                                            const char* propertyName, uint64_t elementIndex,
                                            const ZHLN_ScriptVal* value);

// Register every component type core declares with the binder. Must run before
// any of the above; until it does, the registry is empty and every lookup fails.
// Returns the number of types registered.
ZHLN_API uint64_t ZHLN_RegisterCoreScriptTypes(void);

// Human-readable name for a ScriptError code, for script-side diagnostics.
ZHLN_API const char* ZHLN_ScriptErrorName(uint32_t error);

#ifdef __cplusplus
}
#endif
