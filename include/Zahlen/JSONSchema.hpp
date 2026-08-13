// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Core/Reflection.hpp"
#include <string_view>

namespace ZHLN::Reflect {

namespace Detail::JSON {

// Lexer Helpers
constexpr void SkipWhitespace(std::string_view& src) noexcept {
    while (!src.empty() && (src[0] == ' ' || src[0] == '\t' || src[0] == '\n' || src[0] == '\r')) {
        src.remove_prefix(1);
    }
}

constexpr std::string_view ParseString(std::string_view& src) noexcept {
    SkipWhitespace(src);
    if (src.empty() || src[0] != '"') {
        return {};
    }
    src.remove_prefix(1);
    size_t end = 0;
    while (end < src.size() && src[end] != '"') {
        if (src[end] == '\\' && end + 1 < src.size()) {
            end += 2;
        } else {
            end++;
        }
    }
    std::string_view str = src.substr(0, end);
    if (end < src.size()) {
        src.remove_prefix(end + 1);
    } else {
        src.remove_prefix(end);
    }
    return str;
}

constexpr bool IsFloatNumber(std::string_view src) noexcept {
    SkipWhitespace(src);
    size_t i = 0;
    if (i < src.size() && (src[i] == '-' || src[i] == '+')) {
        i++;
    }
    while (i < src.size() && (src[i] >= '0' && src[i] <= '9')) {
        i++;
    }
    return (i < src.size() && (src[i] == '.' || src[i] == 'e' || src[i] == 'E'));
}

constexpr void SkipValue(std::string_view& src) noexcept {
    SkipWhitespace(src);
    if (src.empty()) {
        return;
    }
    if (src[0] == '"') {
        ParseString(src);
    } else if (src[0] == '{' || src[0] == '[') {
        char open  = src[0];
        char close = (open == '{') ? '}' : ']';
        int  depth = 0;
        while (!src.empty()) {
            if (src[0] == '"') {
                ParseString(src);
                continue;
            }
            if (src[0] == open) {
                depth++;
            } else if (src[0] == close) {
                depth--;
                src.remove_prefix(1);
                if (depth == 0) {
                    break;
                }
                continue;
            }
            src.remove_prefix(1);
        }
    } else {
        while (!src.empty() && src[0] != ',' && src[0] != '}' && src[0] != ']' && src[0] != ' ' && src[0] != '\n' && src[0] != '\r') {
            src.remove_prefix(1);
        }
    }
}

// Forward declarations
template <size_t NodeID>
consteval TypeDescriptor InferValueType(std::string_view& src);

template <size_t NodeID>
consteval TypeDescriptor ParseArrayType(std::string_view& src) {
    SkipWhitespace(src);
    if (!src.empty() && src[0] == '[') {
        src.remove_prefix(1);
    }

    size_t         count    = 0;
    TypeDescriptor elemType = TypeDescriptor::Void();

    while (!src.empty()) {
        SkipWhitespace(src);
        if (src.empty() || src[0] == ']') {
            if (!src.empty()) {
                src.remove_prefix(1);
            }
            break;
        }

        if (count == 0) {
            elemType = InferValueType<NodeID + 1>(src);
        } else {
            SkipValue(src);
        }
        count++;

        SkipWhitespace(src);
        if (!src.empty() && src[0] == ',') {
            src.remove_prefix(1);
        }
    }

    return TypeDescriptor::ArrayOf(elemType, (count == 0) ? 0 : count);
}

template <size_t NodeID, size_t FieldIdx, typename TargetStruct>
consteval void ParseObjectFields(std::string_view& src, AggregateBuilder<TargetStruct>& builder) {
    SkipWhitespace(src);
    if (src.empty() || src[0] == '}') {
        if (!src.empty()) {
            src.remove_prefix(1);
        }
        return;
    }

    std::string_view keyName = ParseString(src);

    SkipWhitespace(src);
    if (!src.empty() && src[0] == ':') {
        src.remove_prefix(1);
    }

    constexpr size_t ChildNodeID = NodeID * 100 + FieldIdx + 1;
    TypeDescriptor   fieldType   = InferValueType<ChildNodeID>(src);
    builder.AddField(keyName, fieldType);

    SkipWhitespace(src);
    if (!src.empty() && src[0] == ',') {
        src.remove_prefix(1);
    }

    ParseObjectFields<NodeID, FieldIdx + 1>(src, builder);
}

template <size_t NodeID, typename TargetStruct>
consteval void ParseObjectToBuilder(std::string_view& src, AggregateBuilder<TargetStruct>& builder) {
    SkipWhitespace(src);
    if (!src.empty() && src[0] == '{') {
        src.remove_prefix(1);
    }
    ParseObjectFields<NodeID, 0>(src, builder);
}

constexpr size_t TrueLength  = 4;
constexpr size_t FalseLength = 5;
constexpr size_t NullLength  = 4;

template <size_t NodeID>
consteval TypeDescriptor InferValueType(std::string_view& src) {
    SkipWhitespace(src);
    if (src.empty()) {
        return TypeDescriptor::Void();
    }

    if (src[0] == '"') {
        ParseString(src);
        return TypeDescriptor::String();
    }

    if (src[0] == '{') {
        using NestedType = typename detail::AnonymousNode<NodeID>::type;
        AggregateBuilder<NestedType> nestedBuilder;
        ParseObjectToBuilder<NodeID>(src, nestedBuilder);
        return nestedBuilder.Build();
    }

    if (src[0] == '[') {
        return ParseArrayType<NodeID>(src);
    }

    if (src.starts_with("true") || src.starts_with("false")) {
        src.remove_prefix(src.starts_with("true") ? TrueLength : FalseLength);
        return TypeDescriptor::Boolean();
    }

    if (src.starts_with("null")) {
        src.remove_prefix(NullLength);
        return TypeDescriptor::Null();
    }

    if (src[0] == '-' || (src[0] >= '0' && src[0] <= '9')) {
        bool isFloat = IsFloatNumber(src);
        SkipValue(src);
        return isFloat ? TypeDescriptor::Float64() : TypeDescriptor::Int64();
    }

    SkipValue(src);
    return TypeDescriptor::Void();
}

template <typename TargetStruct>
consteval void PopulateObjectFromJSON(std::string_view& src, TargetStruct& obj) {
    SkipWhitespace(src);
    if (!src.empty() && src[0] == '{') {
        src.remove_prefix(1);
    }

    while (!src.empty()) {
        SkipWhitespace(src);
        if (src.empty() || src[0] == '}') {
            if (!src.empty()) {
                src.remove_prefix(1);
            }
            break;
        }

        std::string_view keyName = ParseString(src);

        SkipWhitespace(src);
        if (!src.empty() && src[0] == ':') {
            src.remove_prefix(1);
        }

        VisitFieldByName(obj, keyName, [&](auto& fieldVal) {
            using FieldType = std::decay_t<decltype(fieldVal)>;

            if constexpr (std::is_same_v<FieldType, std::string_view>) {
                fieldVal = ParseString(src);
            } else if constexpr (std::is_same_v<FieldType, bool>) {
                bool isTrue = src.starts_with("true");
                src.remove_prefix(isTrue ? TrueLength : FalseLength);
                fieldVal = isTrue;
            } else if constexpr (std::is_same_v<FieldType, int64_t>) {
                int64_t val = 0;
                bool    neg = false;
                if (!src.empty() && src[0] == '-') {
                    neg = true;
                    src.remove_prefix(1);
                }
                while (!src.empty() && src[0] >= '0' && src[0] <= '9') {
                    val = val * 10 + (src[0] - '0');
                    src.remove_prefix(1);
                }
                fieldVal = neg ? -val : val;
            } else if constexpr (std::is_class_v<FieldType>) {
                PopulateObjectFromJSON(src, fieldVal);
            } else {
                SkipValue(src);
            }
        });

        SkipWhitespace(src);
        if (!src.empty() && src[0] == ',') {
            src.remove_prefix(1);
        }
    }
}

} // namespace Detail::JSON

// ============================================================================
// Public Declarative Interface
// ============================================================================

// 1. Literal Interface (e.g. R"({...})")
template <StringLiteral JSONStr>
struct JSONSchema {
    struct type;

    consteval {
        std::string_view src = JSONStr;

        AggregateBuilder<type> builder;
        Detail::JSON::ParseObjectToBuilder<0>(src, builder);
        builder.Build();
    }
};

template <StringLiteral JSONStr>
using JSONType = typename JSONSchema<JSONStr>::type;

// 2. Reference Interface (e.g. #embed std::string_view / char[] variables)
template <auto const& JSONSource>
struct JSONSchemaRef {
    struct type;

    consteval {
        std::string_view src = JSONSource;

        AggregateBuilder<type> builder;
        Detail::JSON::ParseObjectToBuilder<0>(src, builder);
        builder.Build();
    }
};

template <auto const& JSONSource>
using JSONTypeRef = typename JSONSchemaRef<JSONSource>::type;

// Overloaded Compile-Time Value Parsers
template <StringLiteral JSONStr>
consteval auto ParseJSONConst() {
    using ConfigType = JSONType<JSONStr>;
    ConfigType       obj {};
    std::string_view src = JSONStr;
    Detail::JSON::PopulateObjectFromJSON(src, obj);
    return obj;
}

template <auto const& JSONSource>
consteval auto ParseJSONConst() {
    using ConfigType = JSONTypeRef<JSONSource>;
    ConfigType       obj {};
    std::string_view src = JSONSource;
    Detail::JSON::PopulateObjectFromJSON(src, obj);
    return obj;
}

} // namespace ZHLN::Reflect
