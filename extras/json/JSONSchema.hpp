// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <string_view>
#include <type_traits>

namespace ZHLN::Reflect {

namespace Detail::JSON {

// Lexer Helpers
constexpr void SkipWhitespace(std::string_view& src) noexcept {
    while (!src.empty() && (src[0] == ' ' || src[0] == '\t' || src[0] == '\n' || src[0] == '\r')) {
        src.remove_prefix(1);
    }
}

constexpr auto ParseString(std::string_view& src) noexcept -> std::string_view {
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

constexpr auto IsFloatNumber(std::string_view src) noexcept -> bool {
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

constexpr size_t TrueLength  = 4;
constexpr size_t FalseLength = 5;
constexpr size_t NullLength  = 4;

// Forward declarations with compile-time depth bound
template <typename SchemaContext, size_t Depth = 0>
consteval auto InferValueType(std::string_view& src) -> TypeDescriptor;

template <typename SchemaContext, size_t Depth = 0>
consteval auto ParseObjectToBuilder(std::string_view& src) -> TypeDescriptor;

template <typename SchemaContext, size_t Depth = 0>
consteval auto ParseArrayType(std::string_view& src) -> TypeDescriptor {
    if constexpr (Depth >= 4) {
        SkipValue(src);
        return TypeDescriptor::Void();
    } else {
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
                elemType = InferValueType<SchemaContext, Depth + 1>(src);
            } else {
                SkipValue(src);
            }
            count++;

            SkipWhitespace(src);
            if (!src.empty() && src[0] == ',') {
                src.remove_prefix(1);
            }
        }

        return TypeDescriptor::ArrayOf(elemType, count);
    }
}

template <typename SchemaContext, size_t Depth>
consteval auto ParseObjectToBuilder(std::string_view& src) -> TypeDescriptor {
    if constexpr (Depth >= 4) {
        SkipValue(src);
        return TypeDescriptor::Void();
    } else {
        using CurrentStruct = typename SchemaContext::template Node<Depth>;
        AggregateBuilder<CurrentStruct> builder;

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
            SkipWhitespace(src);

            TypeDescriptor fieldType = InferValueType<SchemaContext, Depth + 1>(src);
            builder.AddField(keyName, fieldType);

            SkipWhitespace(src);
            if (!src.empty() && src[0] == ',') {
                src.remove_prefix(1);
            }
        }

        return builder.Build();
    }
}

template <typename SchemaContext, size_t Depth>
consteval auto InferValueType(std::string_view& src) -> TypeDescriptor {
    if constexpr (Depth >= 4) {
        SkipValue(src);
        return TypeDescriptor::Void();
    } else {
        SkipWhitespace(src);
        if (src.empty()) {
            return TypeDescriptor::Void();
        }

        if (src[0] == '"') {
            ParseString(src);
            return TypeDescriptor::String();
        }

        if (src[0] == '{') {
            return ParseObjectToBuilder<SchemaContext, Depth>(src);
        }

        if (src[0] == '[') {
            return ParseArrayType<SchemaContext, Depth>(src);
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
        SkipWhitespace(src); // Ensure whitespace after ':' is consumed

        bool found = VisitFieldByName(obj, keyName, [&](auto& fieldVal) -> auto {
            using FieldType = std::decay_t<decltype(fieldVal)>;

            SkipWhitespace(src);
            if constexpr (std::is_same_v<FieldType, std::string_view>) {
                fieldVal = ParseString(src);
            } else if constexpr (std::is_same_v<FieldType, bool>) {
                if (src.starts_with("true")) {
                    src.remove_prefix(TrueLength);
                    fieldVal = true;
                } else if (src.starts_with("false")) {
                    src.remove_prefix(FalseLength);
                    fieldVal = false;
                }
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
            } else if constexpr (std::is_same_v<FieldType, double>) {
                double val = 0.0;
                bool   neg = false;
                if (!src.empty() && src[0] == '-') {
                    neg = true;
                    src.remove_prefix(1);
                }
                while (!src.empty() && src[0] >= '0' && src[0] <= '9') {
                    val = val * 10.0 + (src[0] - '0');
                    src.remove_prefix(1);
                }
                if (!src.empty() && src[0] == '.') {
                    src.remove_prefix(1);
                    double frac = 0.1;
                    while (!src.empty() && src[0] >= '0' && src[0] <= '9') {
                        val += (src[0] - '0') * frac;
                        frac *= 0.1;
                        src.remove_prefix(1);
                    }
                }
                fieldVal = neg ? -val : val;
            } else if constexpr (std::is_class_v<FieldType>) {
                PopulateObjectFromJSON(src, fieldVal);
            } else {
                SkipValue(src);
            }
        });

        if (!found) {
            SkipValue(src); // Skip unmapped keys to guarantee stream advancement
        }

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

template <StringLiteral JSONStr>
struct JSONSchema {
    template <size_t ID>
    struct Node;

    using type = Node<0>;

    consteval {
        std::string_view src = JSONStr;
        Detail::JSON::ParseObjectToBuilder<JSONSchema, 0>(src);
    }
};

template <StringLiteral JSONStr>
using JSONType = typename JSONSchema<JSONStr>::type;

template <StringLiteral JSONStr>
consteval auto ParseJSONConst() {
    using ConfigType = JSONType<JSONStr>;
    ConfigType       obj {};
    std::string_view src = JSONStr;
    Detail::JSON::PopulateObjectFromJSON(src, obj);
    return obj;
}

} // namespace ZHLN::Reflect
