// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN {

struct DumpOptions {
    size_t bytes_per_line = 16;
    bool   show_ascii     = true;
    bool   show_interpret = true;
};

auto TraceStructCallback(const char* fmt, ...) -> int;
void TraceStructHeader(std::string_view name, std::string_view label, const char* file, uint32_t line);
void TraceStructFooter();

template <typename T>
inline void TraceStructInternal(const T& obj, std::string_view name, LogContext ctx) {
    TraceStructHeader(name, ctx.fmt, ctx.loc.file_name(), ctx.loc.line());

    auto dumpObject = [](const auto& val) -> auto {
        using Decayed = std::remove_cvref_t<decltype(val)>;
        if constexpr (Reflect::FieldCount<Decayed>() > 0) {
            Reflect::ForEachFieldWithName(val, [](std::string_view fieldName, const auto& fieldVal) -> auto {
                std::string debugVal = Reflect::ToDebugString(fieldVal);
                Println(stderr, "│   {}: {}", fieldName, debugVal);
            });
        } else {
            std::string debugVal = Reflect::ToDebugString(val);
            Println(stderr, "│   {}", debugVal);
        }
    };

    if constexpr (std::is_pointer_v<T>) {
        if (obj != nullptr) {
            dumpObject(*obj);
        } else {
            Println(stderr, "│   (null pointer)");
        }
    } else if constexpr (requires { obj.get(); }) {
        if (obj.get() != nullptr) {
            dumpObject(*obj.get());
        } else {
            Println(stderr, "│   (null pointer)");
        }
    } else {
        dumpObject(obj);
    }

    TraceStructFooter();
}

void MemoryDump(const void* ptr, size_t size, std::string_view label, LogContext ctx, DumpOptions opts = {});

template <typename T>
void SmartDumpInternal(const T& var, std::string_view name, LogContext ctx) {
    if constexpr (requires {
                      var.data();
                      var.size();
                  }) {
        MemoryDump(var.data(), var.size() * sizeof(var[0]), name, ctx);
    } else {
        MemoryDump(&var, sizeof(var), name, ctx);
    }
}

template <typename T>
void Dump(const T& var, std::string_view label = {}, std::source_location loc = std::source_location::current()) {
    std::string_view name = label.empty() ? Reflect::TypeName<T>() : label;
    LogContext       ctx(name, loc);
    SmartDumpInternal(var, name, ctx);
}

template <typename T>
void Trace(const T& var, std::string_view label = {}, std::source_location loc = std::source_location::current()) {
    std::string_view name = label.empty() ? Reflect::TypeName<T>() : label;
    LogContext       ctx(name, loc);
    TraceStructInternal(var, name, ctx);
}

} // namespace ZHLN
