// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN {

enum class JSONError : uint8_t { InvalidJSON = 1, TypeMismatch, MissingField, UnsupportedType };

namespace ReflectJSON {

// Opaque non-template Value Reader
class ZHLN_API ValueReader {
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
class ZHLN_API Document {
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

// ============================================================================
// JSON Writer — the write-side counterpart of Document/ValueReader.
//
// Values are built bottom-up from the static factories and assembled with
// Set() (objects, runtime keys, insertion order preserved) and Push()
// (arrays). Stringify() emits the whole tree; non-finite numbers are not
// representable in JSON and serialise as null.
// ============================================================================
class ZHLN_API Value {
  public:
    Value();
    ~Value();

    Value(const Value&)                    = delete;
    auto operator=(const Value&) -> Value& = delete;
    Value(Value&&) noexcept;
    auto operator=(Value&&) noexcept -> Value&;

    [[nodiscard]] static auto Object() -> Value;
    [[nodiscard]] static auto Array() -> Value;
    [[nodiscard]] static auto String(std::string_view text) -> Value;
    [[nodiscard]] static auto Number(double value) -> Value;
    [[nodiscard]] static auto Bool(bool value) -> Value;
    [[nodiscard]] static auto Null() -> Value;

    /// Object only (JSONError::TypeMismatch otherwise). Overwrites an existing key.
    auto Set(std::string_view key, Value value) -> std::expected<void, Error>;
    /// Array only (JSONError::TypeMismatch otherwise).
    auto Push(Value value) -> std::expected<void, Error>;

    /// Serialises the tree. `indent` spaces per level (0 = compact one-liner).
    [[nodiscard]] auto Stringify(size_t indent = 2) const -> std::string;

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
} // namespace ZHLN
