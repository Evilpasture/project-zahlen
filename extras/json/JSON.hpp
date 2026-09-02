// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/json/JSON.hpp
//
// The opaque document layer for JSON: JSONError, Document and ValueReader
// wrap simdjson behind a non-template interface (implementation in
// extras/json/JSON.cpp). Reflection-driven serialisation -- reading a JSON
// document into a reflected type, writing one back out, and the compile-time
// schema parser -- lives in extras/json/JSONSchema.hpp, which includes this
// header. Include this one when you need a raw document; include
// JSONSchema.hpp when mapping documents to and from C++ types is the point.

#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
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

    /// True only for a JSON null value. Lets the reflection layer read a null
    /// as "no value": std::optional members parse disengaged, matching the
    /// writer that emits null for a disengaged optional (default mode).
    [[nodiscard]] auto IsNull() const noexcept -> bool;

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

} // namespace ReflectJSON

} // namespace ZHLN
