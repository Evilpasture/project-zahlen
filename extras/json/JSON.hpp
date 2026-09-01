// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cmath>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN {

enum class JSONError : uint8_t { InvalidJSON = 1, TypeMismatch, MissingField, UnsupportedType };

namespace ReflectJSON {

// Opaque non-template Value Reader
class ValueReader {
  public:
    ValueReader() = default;
    explicit ValueReader(const void* internalNode);

    [[nodiscard]] auto GetInt() const noexcept -> std::expected<int64_t, Error>;
    [[nodiscard]] auto GetUInt() const noexcept -> std::expected<uint64_t, Error>;
    [[nodiscard]] auto GetDouble() const noexcept -> std::expected<double, Error>;
    [[nodiscard]] auto GetBool() const noexcept -> std::expected<bool, Error>;
    [[nodiscard]] auto GetString() const noexcept -> std::expected<std::string_view, Error>;
    [[nodiscard]] auto GetKey(std::string_view key) const noexcept -> std::expected<ValueReader, Error>;

    /// Object only (JSONError::TypeMismatch otherwise). Member keys in document
    /// order; the views remain valid for the owning Document's lifetime.
    [[nodiscard]] auto GetObjectKeys() const -> std::expected<std::vector<std::string_view>, Error>;

    [[nodiscard]] auto GetArraySize() const noexcept -> size_t;
    [[nodiscard]] auto GetArrayElement(size_t index) const noexcept -> std::expected<ValueReader, Error>;

  private:
    uint64_t _opaque[2] = {0, 0};
    bool     _valid     = false;
};

// Opaque non-template Document Parser
class Document {
  public:
    Document();
    ~Document();

    Document(const Document&)                    = delete;
    auto operator=(const Document&) -> Document& = delete;
    Document(Document&&) noexcept;
    auto operator=(Document&&) noexcept -> Document&;

    [[nodiscard]] static auto Parse(std::string_view jsonString) noexcept -> std::expected<Document, Error>;
    [[nodiscard]] auto        GetRoot() const noexcept -> ValueReader;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

template <typename T>
auto ParseObject(ValueReader reader) -> std::expected<T, Error>;

template <typename FieldType>
auto GetJSONValue(ValueReader reader) -> std::expected<FieldType, Error> {
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
            auto parsed = GetJSONValue<MappedType>(*field);
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
            auto parsed_item = GetJSONValue<ElementType>(*elemReader);
            if (!parsed_item) {
                return std::unexpected(parsed_item.error());
            }
            container.push_back(std::move(*parsed_item));
        }
        return container;
    } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
        return ParseObject<Decayed>(reader);
    } else {
        return std::unexpected(JSONError::UnsupportedType);
    }
}

template <typename T>
auto ParseObject(ValueReader reader) -> std::expected<T, Error> {
    T                    obj {};
    std::optional<Error> err;

    ZHLN::Reflect::ForEachFieldWithName(obj, [&](std::string_view fieldName, auto& fieldVal) -> auto {
        if (err) {
            return;
        }

        using FieldType = std::decay_t<decltype(fieldVal)>;

        auto keyReader = reader.GetKey(fieldName);
        if (!keyReader) {
            err = JSONError::MissingField;
            return;
        }

        auto value_res = GetJSONValue<FieldType>(*keyReader);
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
auto TryParse(std::string_view jsonString) -> std::expected<T, Error> {
    auto doc = Document::Parse(jsonString);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    return ParseObject<T>(doc->GetRoot());
}

template <typename T>
auto Parse(std::string_view jsonString) -> T {
    auto res = TryParse<T>(jsonString);
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
//   std::string json = ZHLN::Reflect::SerializeJSON(p);
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
// ============================================================================
namespace Reflect {

namespace detail {

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
    void AppendJSONValue(std::string& out, const T& value, size_t indent, size_t depth) {
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
            AppendJSONString(out, EnumToString(value));
        } else if constexpr (requires { value.has_value(); }) {
            // std::optional (and optional-likes): empty emits null.
            if (value.has_value()) {
                AppendJSONValue(out, *value, indent, depth);
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
                AppendJSONValue(out, mapped, indent, depth + 1);
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
                AppendJSONValue(out, element, indent, depth + 1);
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
            ForEachFieldWithName(value, [&](std::string_view fieldName, const auto& fieldVal) {
                if (!first) {
                    AppendJSONSeparator(out, pretty, indent, depth);
                }
                first = false;
                AppendJSONString(out, fieldName);
                out += pretty ? ": " : ":";
                AppendJSONValue(out, fieldVal, indent, depth + 1);
            });
            if (pretty) {
                out += '\n';
                out.append(depth * indent, ' ');
            }
            out += '}';
        } else {
            static_assert(!sizeof(Decayed), "SerializeJSON: unsupported field type (see the supported set in json/JSON.hpp)");
        }
    }

} // namespace detail

template <typename T>
[[nodiscard]] auto SerializeJSON(const T& value, size_t indent = 0) -> std::string {
    std::string out;
    out.reserve(256);
    detail::AppendJSONValue(out, value, indent, 0);
    return out;
}

} // namespace Reflect
} // namespace ZHLN
