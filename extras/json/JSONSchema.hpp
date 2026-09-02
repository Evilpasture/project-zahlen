// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/json/JSONSchema.hpp
//
// The serialisation / schema layer for JSON. Two halves:
//
//   * Runtime, reflection-driven: GetJSONValue, ParseObject, TryParse and
//     Parse read a document (extras/json/JSON.hpp -- simdjson behind an
//     opaque handle) into any reflected type, and ReflectJSON::SerializeJSON
//     writes one back out. Field names come from the declarations themselves,
//     so the type is the schema: same contract as extras/toml/TOML.hpp.
//
//   * Compile-time: JSONSchema<"..."> / JSONType / ParseJSONConst synthesize a
//     concrete aggregate from a JSON string literal during constant
//     evaluation.
//
// JSON.hpp by itself is the low-level document; this header is what maps
// documents to and from C++ types.

#include <json/JSON.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cmath>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN {
namespace ReflectJSON {

// Shared option block for the reflection-driven read and write halves.
//
// omitEmpty models glTF's "optional by omission" convention on both sides:
//   * SerializeJSON skips a disengaged std::optional (or an empty map/range)
//     member entirely instead of writing null / [].
//   * TryParse/ParseObject treat a missing member key as "leave the default"
//     (a disengaged optional, an empty container) instead of MissingField.
//
// The default (false) keeps the historical behaviour in both directions:
// empty optional writes null and a missing key is an error.
struct Options {
    bool omitEmpty = false;
};

template <typename T>
auto ParseObject(ValueReader reader, Options options = {}) -> std::expected<T, Error>;

template <typename FieldType>
auto GetJSONValue(ValueReader reader, Options options = {}) -> std::expected<FieldType, Error> {
    using Decayed = std::decay_t<FieldType>;

    if constexpr (std::is_same_v<Decayed, int> || std::is_same_v<Decayed, int32_t>) {
        auto res = reader.GetInt();
        if (!res) {
            return std::unexpected(res.error());
        }
        return static_cast<FieldType>(*res);
    } else if constexpr (std::is_same_v<Decayed, uint32_t>) {
        auto res = reader.GetUInt();
        if (!res) {
            return std::unexpected(res.error());
        }
        return static_cast<FieldType>(*res);
    } else if constexpr (std::is_same_v<Decayed, float>) {
        auto res = reader.GetDouble();
        if (!res) {
            return std::unexpected(res.error());
        }
        return static_cast<float>(*res);
    } else if constexpr (std::is_same_v<Decayed, double>) {
        return reader.GetDouble();
    } else if constexpr (std::is_same_v<Decayed, bool>) {
        return reader.GetBool();
    } else if constexpr (std::is_same_v<Decayed, std::string_view>) {
        return reader.GetString();
    } else if constexpr (std::is_same_v<Decayed, std::string>) {
        auto res = reader.GetString();
        if (!res) {
            return std::unexpected(res.error());
        }
        return std::string(*res);
    } else if constexpr (std::is_enum_v<Decayed>) {
        auto res = reader.GetString();
        if (!res) {
            return std::unexpected(res.error());
        }
        auto enum_opt = ZHLN::Reflect::StringToEnum<Decayed>(*res);
        if (!enum_opt) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return *enum_opt;
    } else if constexpr (requires { typename Decayed::key_type; typename Decayed::mapped_type; }) {
        // Maps (JSON objects with runtime keys). Must precede the range branch:
        // a map is a range of pairs but has no push_back.
        auto keys = reader.GetObjectKeys();
        if (!keys) {
            return std::unexpected(keys.error());
        }
        Decayed container;
        using MappedType = typename Decayed::mapped_type;
        for (const std::string_view key: *keys) {
            auto field = reader.GetKey(key);
            if (!field) {
                return std::unexpected(field.error());
            }
            auto parsed = GetJSONValue<MappedType>(*field, options);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            container.emplace(std::string {key}, std::move(*parsed));
        }
        return container;
    } else if constexpr (std::ranges::range<Decayed>) {
        size_t  size = reader.GetArraySize();
        Decayed container;
        using ElementType = typename Decayed::value_type;
        for (size_t i = 0; i < size; ++i) {
            auto elemReader = reader.GetArrayElement(i);
            if (!elemReader) {
                return std::unexpected(elemReader.error());
            }
            auto parsed_item = GetJSONValue<ElementType>(*elemReader, options);
            if (!parsed_item) {
                return std::unexpected(parsed_item.error());
            }
            container.push_back(std::move(*parsed_item));
        }
        return container;
    } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
        return ParseObject<Decayed>(reader, options);
    } else {
        return std::unexpected(JSONError::UnsupportedType);
    }
}

template <typename T>
auto ParseObject(ValueReader reader, Options options) -> std::expected<T, Error> {
    T                    obj {};
    std::optional<Error> err;

    ZHLN::Reflect::ForEachFieldWithName(obj, [&](std::string_view fieldName, auto& fieldVal) -> auto {
        if (err) {
            return;
        }

        using FieldType = std::decay_t<decltype(fieldVal)>;

        auto keyReader = reader.GetKey(fieldName);
        if (!keyReader) {
            if (options.omitEmpty && keyReader.error().Is(JSONError::MissingField)) {
                return; // Optional-by-omission: absent key keeps the default.
            }
            err = keyReader.error();
            return;
        }

        auto value_res = GetJSONValue<FieldType>(*keyReader, options);
        if (!value_res) {
            err = value_res.error();
            return;
        }

        fieldVal = std::move(*value_res);
    });

    // Error Branch: Must explicitly use std::unexpected
    if (err) {
        return std::unexpected(*err);
    }

    // Success Branch: Move constructed obj or value-initialize T with return {};
    return obj;
}

template <typename T>
auto TryParse(std::string_view jsonString, Options options = {}) -> std::expected<T, Error> {
    auto doc = Document::Parse(jsonString);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    return ParseObject<T>(doc->GetRoot(), options);
}

template <typename T>
auto Parse(std::string_view jsonString, Options options = {}) -> T {
    auto res = TryParse<T>(jsonString, options);
    if (!res) [[unlikely]] {
        ZHLN::Panic("Failed to parse JSON for type '{}': {}", ZHLN::Reflect::TypeName<T>(), res.error().Message());
    }
    return std::move(*res);
}

} // namespace ReflectJSON

// ============================================================================
// Reflection-Driven JSON Serialisation
//
//   Player p {"Hero", 9999, true};
//   std::string json = ZHLN::ReflectJSON::SerializeJSON(p);
//   // {"name":"Hero","score":9999,"isAlive":true}
//
// Field names come from the declarations themselves (the same
// ForEachFieldWithName traversal ParseObject reads with), so every reflected
// struct serialises and deserialises through one source of truth. This also
// covers the compile-time schema parser: a ParseJSONConst<"...">() result
// re-serialises losslessly.
//
// Supported field types: bool, integrals, floating point (non-finite emits
// null), std::string_view/std::string/const char*/char arrays, enums (via
// EnumToString), std::optional (empty emits null), maps with string keys
// (JSON objects, key order = container order), ranges (JSON arrays), and
// nested reflected structs. Anything else fails to compile with a
// static_assert at the offending field.
//
// `indent` spaces per nesting level; 0 (default) emits one compact line.
//
// Options{.omitEmpty = true} switches the document to glTF-style optional-by-
// omission: a disengaged optional member, or an empty map/range member, is
// not written at all. This is the write-side half of the option block above;
// the matching read half treats an absent key as the default value. String
// types are never treated as containers, so an empty name still serialises
// as "".
// ============================================================================
namespace ReflectJSON {

namespace detail {

    /// True when a member value should be omitted entirely under
    /// Options{.omitEmpty = true}: a disengaged optional, or an empty
    /// map/range. Strings are values, never collections.
    template <typename T>
    constexpr auto IsEmptyOmittableMember(const T& value) noexcept -> bool {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (requires { value.has_value(); }) {
            return !value.has_value();
        } else if constexpr (std::is_same_v<Decayed, std::string> || std::is_same_v<Decayed, std::string_view> ||
                             std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*> ||
                             (std::is_array_v<Decayed> && std::is_same_v<std::remove_extent_t<Decayed>, char>)) {
            return false;
        } else if constexpr (requires { value.empty(); }) {
            return value.empty();
        } else {
            return false;
        }
    }


    inline void AppendJSONString(std::string& out, std::string_view text) {
        static constexpr std::string_view kHexDigits {"0123456789abcdef"};

        out += '"';
        for (const char c: text) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default: {
                    const auto byte = static_cast<unsigned char>(c);
                    if (byte < 0x20) {
                        out += "\\u00";
                        out += kHexDigits[(byte >> 4) & 0xF];
                        out += kHexDigits[byte & 0xF];
                    } else {
                        out += c;
                    }
                }
            }
        }
        out += '"';
    }

    inline void AppendJSONSeparator(std::string& out, bool pretty, size_t indent, size_t depth) {
        if (pretty) {
            out += ",\n";
            out.append((depth + 1) * indent, ' ');
        } else {
            out += ',';
        }
    }

    template <typename T>
    void AppendJSONValue(std::string& out, const T& value, size_t indent, size_t depth, bool omitEmpty) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (std::is_same_v<Decayed, bool>) {
            out += value ? "true" : "false";
        } else if constexpr (std::is_integral_v<Decayed>) {
            out += std::format("{}", value);
        } else if constexpr (std::is_floating_point_v<Decayed>) {
            // JSON cannot express NaN/Inf; null keeps the document parseable.
            if (std::isfinite(value)) {
                out += std::format("{}", value); // shortest round-trippable form
            } else {
                out += "null";
            }
        } else if constexpr (std::is_same_v<Decayed, std::string_view> || std::is_same_v<Decayed, std::string>) {
            AppendJSONString(out, value);
        } else if constexpr (std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>) {
            AppendJSONString(out, value != nullptr ? std::string_view {value} : std::string_view {});
        } else if constexpr (std::is_array_v<Decayed> && std::is_same_v<std::remove_extent_t<Decayed>, char>) {
            // char buffers: strlen semantics (stops at the first NUL, so
            // string literals never embed their terminator).
            AppendJSONString(out, std::string_view {value});
        } else if constexpr (std::is_enum_v<Decayed>) {
            AppendJSONString(out, ZHLN::Reflect::EnumToString(value));
        } else if constexpr (requires { value.has_value(); }) {
            // std::optional (and optional-likes): empty emits null (or is
            // skipped by the enclosing reflected struct under omitEmpty).
            if (value.has_value()) {
                AppendJSONValue(out, *value, indent, depth, omitEmpty);
            } else {
                out += "null";
            }
        } else if constexpr (requires { typename Decayed::key_type; typename Decayed::mapped_type; }) {
            // Maps: JSON objects. Key order is the container's (std::map: sorted).
            static_assert(std::is_convertible_v<const typename Decayed::key_type&, std::string_view>,
                          "SerializeJSON: map keys must be string-like (JSON object keys are strings)");
            const bool pretty = indent > 0;
            out += pretty ? "{\n" : "{";
            if (pretty) {
                out.append((depth + 1) * indent, ' ');
            }
            bool first = true;
            for (const auto& [key, mapped]: value) {
                if (!first) {
                    AppendJSONSeparator(out, pretty, indent, depth);
                }
                first = false;
                AppendJSONString(out, key);
                out += pretty ? ": " : ":";
                AppendJSONValue(out, mapped, indent, depth + 1, omitEmpty);
            }
            if (pretty) {
                out += '\n';
                out.append(depth * indent, ' ');
            }
            out += '}';
        } else if constexpr (std::ranges::range<Decayed>) {
            // Ranges: JSON arrays.
            const bool pretty = indent > 0;
            out += pretty ? "[\n" : "[";
            if (pretty) {
                out.append((depth + 1) * indent, ' ');
            }
            bool first = true;
            for (const auto& element: value) {
                if (!first) {
                    AppendJSONSeparator(out, pretty, indent, depth);
                }
                first = false;
                AppendJSONValue(out, element, indent, depth + 1, omitEmpty);
            }
            if (pretty) {
                out += '\n';
                out.append(depth * indent, ' ');
            }
            out += ']';
        } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
            // Reflected structs: field names are the JSON keys, matching
            // ParseObject/GetJSONValue on the read side.
            const bool pretty = indent > 0;
            out += pretty ? "{\n" : "{";
            if (pretty) {
                out.append((depth + 1) * indent, ' ');
            }
            bool first = true;
            ZHLN::Reflect::ForEachFieldWithName(value, [&](std::string_view fieldName, const auto& fieldVal) {
                if (omitEmpty && IsEmptyOmittableMember(fieldVal)) {
                    return; // Optional-by-omission: the key is not written.
                }
                if (!first) {
                    AppendJSONSeparator(out, pretty, indent, depth);
                }
                first = false;
                AppendJSONString(out, fieldName);
                out += pretty ? ": " : ":";
                AppendJSONValue(out, fieldVal, indent, depth + 1, omitEmpty);
            });
            if (pretty) {
                out += '\n';
                out.append(depth * indent, ' ');
            }
            out += '}';
        } else {
            static_assert(!sizeof(Decayed), "SerializeJSON: unsupported field type (see the supported set in json/JSONSchema.hpp)");
        }
    }

} // namespace detail

template <typename T>
[[nodiscard]] auto SerializeJSON(const T& value, size_t indent = 0, Options options = {}) -> std::string {
    std::string out;
    out.reserve(256);
    detail::AppendJSONValue(out, value, indent, 0, options.omitEmpty);
    return out;
}

} // namespace ReflectJSON
} // namespace ZHLN

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
