// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// ============================================================================
// Reflection-driven TOML
//
//   const std::string text = ZHLN::Reflect::SerializeTOML(scene);
//   const auto         back = ZHLN::ReflectTOML::TryParse<Scene>(text);
//
// Same contract as extras/json/JSONSchema.hpp -- field names come from the
// themselves, so the type is the schema and there is no second definition to
// drift -- with the differences TOML forces:
//
//   * A reflected struct is a TABLE. Nested structs become [dotted.headers],
//     vectors of structs become [[array.of.tables]], and everything scalar is
//     written as `key = value` before the first sub-table, because TOML binds
//     bare keys to the header above them.
//   * A MISSING KEY LEAVES THE FIELD AT ITS DEFAULT instead of failing. That
//     is the point of using this for scene files: the type carries the
//     defaults, and a document only says what differs from them. JSON's
//     ParseObject errors on a missing field; this one does not.
//   * An UNKNOWN KEY is ignored, but logged. A silently-dropped `postion =`
//     is the worst failure mode a hand-edited format has.
//   * std::optional is omitted entirely when empty rather than written null,
//     which is how TOML expresses absence.
//   * A struct can opt out of table form and serialise as `[x, y, z]` by
//     specialising ReflectTOML::TOMLVector -- see there. SceneTOML.hpp in
//     this directory uses it so Jolt's Float3/Float4 read as coordinates in
//     a scene document while staying Jolt types in the code.
//
// The parser accepts the subset the serialiser emits plus what hand-written
// documents normally use: comments, bare/quoted/dotted keys, table and
// array-of-table headers, basic and literal strings, integers (with _
// separators), floats (including inf/nan), booleans, arrays, and inline
// tables. Dates, times and multi-line strings are not supported -- no engine
// type maps to them.
// ============================================================================

namespace ZHLN {

enum class TOMLError : uint8_t { InvalidTOML = 1, TypeMismatch, MissingField, UnsupportedType, DuplicateKey };

namespace ReflectTOML {

/// A node in a parsed document. Non-owning: valid while its Document lives.
class Value {
  public:
    Value() = default;
    explicit Value(const void* node) noexcept: _node(node) {
    }

    [[nodiscard]] auto IsValid() const noexcept -> bool {
        return _node != nullptr;
    }

    [[nodiscard]] auto GetInt() const noexcept -> std::expected<int64_t, Error>;
    [[nodiscard]] auto GetUInt() const noexcept -> std::expected<uint64_t, Error>;
    [[nodiscard]] auto GetDouble() const noexcept -> std::expected<double, Error>;
    [[nodiscard]] auto GetBool() const noexcept -> std::expected<bool, Error>;
    [[nodiscard]] auto GetString() const noexcept -> std::expected<std::string_view, Error>;

    /// Table lookup. TOMLError::MissingField when the key is absent,
    /// TOMLError::TypeMismatch when this node is not a table.
    [[nodiscard]] auto GetKey(std::string_view key) const noexcept -> std::expected<Value, Error>;
    [[nodiscard]] auto HasKey(std::string_view key) const noexcept -> bool;

    /// Table only. Keys in document order; the views live as long as the Document.
    [[nodiscard]] auto GetTableKeys() const -> std::expected<std::vector<std::string_view>, Error>;

    /// True only for array nodes. Checked before reading a sequence, because
    /// GetArraySize() answers 0 for a table and a field would otherwise take a
    /// wrong-shaped value as an empty one.
    [[nodiscard]] auto IsArray() const noexcept -> bool;

    [[nodiscard]] auto GetArraySize() const noexcept -> size_t;
    [[nodiscard]] auto GetArrayElement(size_t index) const noexcept -> std::expected<Value, Error>;

  private:
    const void* _node = nullptr;
};

/// Owns the parsed node tree.
class Document {
  public:
    Document();
    ~Document();

    Document(const Document&)                    = delete;
    auto operator=(const Document&) -> Document& = delete;
    Document(Document&&) noexcept;
    auto operator=(Document&&) noexcept -> Document&;

    /// Parses a whole document. On failure the offending line is logged and
    /// TOMLError::InvalidTOML (or DuplicateKey) is returned.
    [[nodiscard]] static auto Parse(std::string_view tomlText) noexcept -> std::expected<Document, Error>;

    [[nodiscard]] auto GetRoot() const noexcept -> Value;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/// Opt-in: serialise T as a TOML array of its fields, `[x, y, z]`, instead of
/// a table of named members.
///
/// A small vector type is a struct in C++ and a coordinate in a document, and
/// nobody wants to read
///
///     [entities.transform.position]
///     x = 0.0
///     y = 8.0
///
/// where `position = [0.0, 8.0, 0.0]` says the same thing on one line and
/// diffs as one line. Specialise this next to the type that needs it -- see
/// Zahlen/Scene.hpp, which does it for JPH::Float2/Float3/Float4 -- so this
/// header keeps depending on nothing but the reflection layer.
///
/// Fields are read and written in declaration order through reflection, so a
/// specialisation is one line and supplies no accessors.
template <typename T>
struct TOMLVector : std::false_type {};

namespace detail {

    /// std::array and other fixed-size ranges: sized at compile time, so they
    /// are filled by index instead of push_back.
    template <typename T>
    concept FixedArray = requires(T& t) {
        typename T::value_type;
        { std::tuple_size<T>::value } -> std::convertible_to<size_t>;
    };

    template <typename T>
    concept StringLike = std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

    template <typename T>
    concept MapLike = requires {
        typename T::key_type;
        typename T::mapped_type;
    };

    /// A struct that opted in to array form via TOMLVector.
    template <typename T>
    concept VectorLike = TOMLVector<T>::value && (ZHLN::Reflect::FieldCount<T>() > 0);

    template <typename T>
    concept OptionalLike = requires { typename T::value_type; } && std::is_same_v<T, std::optional<typename T::value_type>>;

} // namespace detail

template <typename T>
auto ParseObject(Value reader) -> std::expected<T, Error>;

template <typename FieldType>
auto GetTOMLValue(Value reader) -> std::expected<FieldType, Error> {
    using Decayed = std::decay_t<FieldType>;

    if constexpr (std::is_same_v<Decayed, bool>) {
        return reader.GetBool();
    } else if constexpr (detail::StringLike<Decayed>) {
        auto res = reader.GetString();
        if (!res) {
            return std::unexpected(res.error());
        }
        return Decayed {*res};
    } else if constexpr (std::is_enum_v<Decayed>) {
        auto res = reader.GetString();
        if (!res) {
            return std::unexpected(res.error());
        }
        const auto parsed = ZHLN::Reflect::StringToEnum<Decayed>(*res);
        if (!parsed) {
            ZHLN::Log("[TOML] '{}' is not a value of enum {}", *res, ZHLN::Reflect::TypeName<Decayed>());
            return std::unexpected(TOMLError::TypeMismatch);
        }
        return *parsed;
    } else if constexpr (std::is_integral_v<Decayed>) {
        if constexpr (std::is_unsigned_v<Decayed>) {
            auto res = reader.GetUInt();
            if (!res) {
                return std::unexpected(res.error());
            }
            return static_cast<Decayed>(*res);
        } else {
            auto res = reader.GetInt();
            if (!res) {
                return std::unexpected(res.error());
            }
            return static_cast<Decayed>(*res);
        }
    } else if constexpr (std::is_floating_point_v<Decayed>) {
        // Integer nodes are accepted: `scale = 1` is what a person writes.
        auto res = reader.GetDouble();
        if (!res) {
            return std::unexpected(res.error());
        }
        return static_cast<Decayed>(*res);
    } else if constexpr (detail::OptionalLike<Decayed>) {
        // std::optional: a present key means an engaged value.
        auto parsed = GetTOMLValue<typename Decayed::value_type>(reader);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return Decayed {std::move(*parsed)};
    } else if constexpr (detail::MapLike<Decayed>) {
        auto keys = reader.GetTableKeys();
        if (!keys) {
            return std::unexpected(keys.error());
        }
        Decayed container;
        for (const std::string_view key: *keys) {
            auto entry = reader.GetKey(key);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            auto parsed = GetTOMLValue<typename Decayed::mapped_type>(*entry);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            container.emplace(std::string {key}, std::move(*parsed));
        }
        return container;
    } else if constexpr (detail::FixedArray<Decayed>) {
        if (!reader.IsArray()) {
            return std::unexpected(TOMLError::TypeMismatch);
        }
        constexpr size_t kCapacity = std::tuple_size<Decayed>::value;
        const size_t     size      = reader.GetArraySize();
        if (size > kCapacity) {
            ZHLN::Log("[TOML] array of {} values does not fit {}[{}]", size, ZHLN::Reflect::TypeName<Decayed>(), kCapacity);
            return std::unexpected(TOMLError::TypeMismatch);
        }
        // Short arrays keep the defaults of the trailing elements, the same way
        // a missing key keeps the default of a field.
        Decayed container {};
        for (size_t i = 0; i < size; ++i) {
            auto element = reader.GetArrayElement(i);
            if (!element) {
                return std::unexpected(element.error());
            }
            auto parsed = GetTOMLValue<typename Decayed::value_type>(*element);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            container[i] = std::move(*parsed);
        }
        return container;
    } else if constexpr (std::ranges::range<Decayed>) {
        if (!reader.IsArray()) {
            return std::unexpected(TOMLError::TypeMismatch);
        }
        const size_t size = reader.GetArraySize();
        Decayed      container;
        for (size_t i = 0; i < size; ++i) {
            auto element = reader.GetArrayElement(i);
            if (!element) {
                return std::unexpected(element.error());
            }
            auto parsed = GetTOMLValue<typename Decayed::value_type>(*element);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            container.push_back(std::move(*parsed));
        }
        return container;
    } else if constexpr (detail::VectorLike<Decayed>) {
        // `[x, y, z]` back into the fields, in declaration order.
        if (!reader.IsArray()) {
            return std::unexpected(TOMLError::TypeMismatch);
        }
        constexpr size_t kCapacity = ZHLN::Reflect::FieldCount<Decayed>();
        const size_t     size      = reader.GetArraySize();
        if (size > kCapacity) {
            ZHLN::Log("[TOML] array of {} values does not fit {}[{}]", size, ZHLN::Reflect::TypeName<Decayed>(), kCapacity);
            return std::unexpected(TOMLError::TypeMismatch);
        }

        Decayed              container {};
        std::optional<Error> err;
        size_t               index = 0;
        ZHLN::Reflect::ForEachField(container, [&](auto& component) -> void {
            const size_t at = index++;
            if (err || at >= size) {
                return; // A short array leaves the trailing components alone.
            }
            auto element = reader.GetArrayElement(at);
            if (!element) {
                err = element.error();
                return;
            }
            auto parsed = GetTOMLValue<std::remove_cvref_t<decltype(component)>>(*element);
            if (!parsed) {
                err = parsed.error();
                return;
            }
            component = std::move(*parsed);
        });
        if (err) {
            return std::unexpected(*err);
        }
        return container;
    } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
        return ParseObject<Decayed>(reader);
    } else {
        return std::unexpected(TOMLError::UnsupportedType);
    }
}

/// Fills a reflected struct from a table. Absent keys keep their default;
/// keys the struct does not declare are logged and skipped.
template <typename T>
auto ParseObject(Value reader) -> std::expected<T, Error> {
    if (!reader.IsValid()) {
        return std::unexpected(TOMLError::MissingField);
    }

    auto documentKeys = reader.GetTableKeys();
    if (!documentKeys) {
        return std::unexpected(documentKeys.error());
    }

    T                    obj {};
    std::optional<Error> err;
    std::vector<bool>    consumed(documentKeys->size(), false);

    ZHLN::Reflect::ForEachFieldWithName(obj, [&](std::string_view fieldName, auto& fieldVal) -> void {
        if (err) {
            return;
        }

        using FieldType = std::decay_t<decltype(fieldVal)>;

        for (size_t i = 0; i < documentKeys->size(); ++i) {
            if ((*documentKeys)[i] == fieldName) {
                consumed[i] = true;
            }
        }

        auto keyReader = reader.GetKey(fieldName);
        if (!keyReader) {
            // Absent: the field keeps whatever the type declared as its
            // default. Documents say what differs, not everything.
            return;
        }

        auto value = GetTOMLValue<FieldType>(*keyReader);
        if (!value) {
            ZHLN::Log("[TOML] field '{}' of {} rejected the document value", fieldName, ZHLN::Reflect::TypeName<T>());
            err = value.error();
            return;
        }

        fieldVal = std::move(*value);
    });

    if (err) {
        return std::unexpected(*err);
    }

    for (size_t i = 0; i < documentKeys->size(); ++i) {
        if (!consumed[i]) {
            ZHLN::Log("[TOML] {} has no field named '{}' -- key ignored", ZHLN::Reflect::TypeName<T>(), (*documentKeys)[i]);
        }
    }

    return obj;
}

template <typename T>
auto TryParse(std::string_view tomlText) -> std::expected<T, Error> {
    auto doc = Document::Parse(tomlText);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    return ParseObject<T>(doc->GetRoot());
}

/// Panics on a malformed document. For call sites where a bad document is a
/// programming error (baked-in defaults, tests); use TryParse for user files.
template <typename T>
auto Parse(std::string_view tomlText) -> T {
    auto res = TryParse<T>(tomlText);
    if (!res) [[unlikely]] {
        ZHLN::Panic("Failed to parse TOML for type '{}': {}", ZHLN::Reflect::TypeName<T>(), res.error().Message());
    }
    return std::move(*res);
}

} // namespace ReflectTOML

namespace Reflect {

namespace detail {

    using ZHLN::ReflectTOML::detail::FixedArray;
    using ZHLN::ReflectTOML::detail::MapLike;
    using ZHLN::ReflectTOML::detail::StringLike;
    using ZHLN::ReflectTOML::detail::VectorLike;

    /// Serialises as a TOML table: [header] on its own, rather than inline
    /// after an `=`.
    template <typename T>
    consteval auto IsTOMLTable() -> bool {
        using Decayed = std::remove_cvref_t<T>;
        if constexpr (StringLike<Decayed> || std::is_enum_v<Decayed> || std::is_arithmetic_v<Decayed>) {
            return false;
        } else if constexpr (VectorLike<Decayed>) {
            return false; // `[x, y, z]` after an `=`, not a table of its own.
        } else if constexpr (MapLike<Decayed>) {
            return true;
        } else if constexpr (FixedArray<Decayed> || std::ranges::range<Decayed>) {
            return false;
        } else {
            return ZHLN::Reflect::FieldCount<Decayed>() > 0;
        }
    }

    /// Serialises as [[array of tables]]: a sequence of struct-shaped things.
    template <typename T>
    consteval auto IsTOMLTableArray() -> bool {
        using Decayed = std::remove_cvref_t<T>;
        if constexpr (StringLike<Decayed> || MapLike<Decayed>) {
            return false;
        } else if constexpr (std::ranges::range<Decayed>) {
            return IsTOMLTable<std::ranges::range_value_t<Decayed>>();
        } else {
            return false;
        }
    }

    /// TOML bare keys are [A-Za-z0-9_-]+; anything else has to be quoted.
    inline void AppendTOMLKey(std::string& out, std::string_view key) {
        const bool bare = !key.empty() && std::ranges::all_of(key, [](char c) {
                              return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                          });
        if (bare) {
            out += key;
            return;
        }
        out += '"';
        for (const char c: key) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        out += '"';
    }

    inline void AppendTOMLString(std::string& out, std::string_view text) {
        static constexpr std::string_view kHexDigits {"0123456789abcdef"};

        out += '"';
        for (const char c: text) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default: {
                    const auto byte = static_cast<unsigned char>(c);
                    if (byte < 0x20 || byte == 0x7F) {
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

    /// TOML distinguishes 1 from 1.0, and a float field that emitted `1` would
    /// come back as an integer node. Every float therefore carries a fraction
    /// or an exponent.
    inline void AppendTOMLFloat(std::string& out, double value) {
        if (std::isnan(value)) {
            out += "nan";
            return;
        }
        if (std::isinf(value)) {
            out += value > 0 ? "inf" : "-inf";
            return;
        }

        const std::string text = std::format("{}", value);
        out += text;
        if (text.find_first_of(".eE") == std::string::npos) {
            out += ".0";
        }
    }

    template <typename T>
    void AppendTOMLInline(std::string& out, const T& value);

    template <typename T>
    void AppendTOMLInlineTable(std::string& out, const T& value) {
        out += '{';
        bool first = true;
        ZHLN::Reflect::ForEachFieldWithName(value, [&](std::string_view fieldName, const auto& fieldVal) {
            if (!first) {
                out += ", ";
            }
            first = false;
            AppendTOMLKey(out, fieldName);
            out += " = ";
            AppendTOMLInline(out, fieldVal);
        });
        out += '}';
    }

    /// Everything that can sit to the right of an `=`.
    template <typename T>
    void AppendTOMLInline(std::string& out, const T& value) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (std::is_same_v<Decayed, bool>) {
            out += value ? "true" : "false";
        } else if constexpr (std::is_enum_v<Decayed>) {
            AppendTOMLString(out, ZHLN::Reflect::EnumToString(value));
        } else if constexpr (std::is_integral_v<Decayed>) {
            out += std::format("{}", value);
        } else if constexpr (std::is_floating_point_v<Decayed>) {
            AppendTOMLFloat(out, static_cast<double>(value));
        } else if constexpr (StringLike<Decayed>) {
            AppendTOMLString(out, value);
        } else if constexpr (std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>) {
            AppendTOMLString(out, value != nullptr ? std::string_view {value} : std::string_view {});
        } else if constexpr (std::is_array_v<Decayed> && std::is_same_v<std::remove_extent_t<Decayed>, char>) {
            AppendTOMLString(out, std::string_view {value});
        } else if constexpr (requires { value.has_value(); }) {
            if (value.has_value()) {
                AppendTOMLInline(out, *value);
            } else {
                // Unreachable through AppendTOMLTableBody, which drops empty
                // optionals rather than writing them: TOML has no null.
                out += "\"\"";
            }
        } else if constexpr (MapLike<Decayed>) {
            out += '{';
            bool first = true;
            for (const auto& [key, mapped]: value) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                AppendTOMLKey(out, key);
                out += " = ";
                AppendTOMLInline(out, mapped);
            }
            out += '}';
        } else if constexpr (std::ranges::range<Decayed> || FixedArray<Decayed>) {
            out += '[';
            bool first = true;
            for (const auto& element: value) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                AppendTOMLInline(out, element);
            }
            out += ']';
        } else if constexpr (VectorLike<Decayed>) {
            out += '[';
            bool first = true;
            ZHLN::Reflect::ForEachField(value, [&](const auto& component) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                AppendTOMLInline(out, component);
            });
            out += ']';
        } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
            AppendTOMLInlineTable(out, value);
        } else {
            static_assert(!sizeof(Decayed), "SerializeTOML: unsupported field type (see the supported set in toml/TOML.hpp)");
        }
    }

    inline void AppendTOMLHeader(std::string& out, std::string_view path, bool arrayElement) {
        if (!out.empty() && !out.ends_with("\n\n")) {
            out += '\n';
        }
        out += arrayElement ? "[[" : "[";
        out += path;
        out += arrayElement ? "]]\n" : "]\n";
    }

    [[nodiscard]] inline auto JoinTOMLPath(std::string_view prefix, std::string_view key) -> std::string {
        std::string path;
        if (!prefix.empty()) {
            path += prefix;
            path += '.';
        }
        AppendTOMLKey(path, key);
        return path;
    }

    template <typename T>
    void AppendTOMLTableBody(std::string& out, const T& value, std::string_view path);

    /// One sub-table, whatever shape it takes: struct, map of structs, or a
    /// sequence that becomes [[array of tables]].
    template <typename T>
    void AppendTOMLSubTable(std::string& out, const T& value, const std::string& path) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (IsTOMLTableArray<Decayed>()) {
            for (const auto& element: value) {
                AppendTOMLHeader(out, path, true);
                AppendTOMLTableBody(out, element, path);
            }
        } else if constexpr (MapLike<Decayed>) {
            for (const auto& [key, mapped]: value) {
                const std::string childPath = JoinTOMLPath(path, key);
                AppendTOMLHeader(out, childPath, false);
                AppendTOMLTableBody(out, mapped, childPath);
            }
        } else {
            AppendTOMLHeader(out, path, false);
            AppendTOMLTableBody(out, value, path);
        }
    }

    /// Scalars first, then sub-tables. TOML binds a bare key to the header
    /// above it, so a `key = value` emitted after a [sub.table] would silently
    /// land in the wrong table -- this ordering is a correctness requirement,
    /// not a style choice.
    template <typename T>
    void AppendTOMLTableBody(std::string& out, const T& value, std::string_view path) {
        ZHLN::Reflect::ForEachFieldWithName(value, [&](std::string_view fieldName, const auto& fieldVal) {
            using FieldType = std::remove_cvref_t<decltype(fieldVal)>;
            if constexpr (!IsTOMLTable<FieldType>() && !IsTOMLTableArray<FieldType>()) {
                if constexpr (requires { fieldVal.has_value(); }) {
                    if (!fieldVal.has_value()) {
                        return; // TOML says "absent" by omission.
                    }
                }
                AppendTOMLKey(out, fieldName);
                out += " = ";
                AppendTOMLInline(out, fieldVal);
                out += '\n';
            }
        });

        ZHLN::Reflect::ForEachFieldWithName(value, [&](std::string_view fieldName, const auto& fieldVal) {
            using FieldType = std::remove_cvref_t<decltype(fieldVal)>;
            if constexpr (IsTOMLTable<FieldType>() || IsTOMLTableArray<FieldType>()) {
                AppendTOMLSubTable(out, fieldVal, JoinTOMLPath(path, fieldName));
            }
        });
    }

} // namespace detail

/// Serialises a reflected struct as a TOML document.
template <typename T>
[[nodiscard]] auto SerializeTOML(const T& value) -> std::string {
    static_assert(
        ZHLN::Reflect::FieldCount<std::remove_cvref_t<T>>() > 0, "SerializeTOML: a TOML document is a table at the root, so it must be a reflected struct"
    );

    std::string out;
    out.reserve(512);
    detail::AppendTOMLTableBody(out, value, std::string_view {});
    return out;
}

} // namespace Reflect
} // namespace ZHLN
