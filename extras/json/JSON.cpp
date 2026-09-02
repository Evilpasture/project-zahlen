// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <json/JSON.hpp>
#include <simdjson.h>
#include <vector>

namespace ZHLN::ReflectJSON {

struct Document::Impl {
    simdjson::dom::parser   parser;
    simdjson::padded_string padded;
    simdjson::dom::element  doc;
};

Document::Document(): _impl(std::make_unique<Impl>()) {
}
Document::~Document() = default;

Document::Document(Document&&) noexcept                    = default;
auto Document::operator=(Document&&) noexcept -> Document& = default;

auto Document::Parse(std::string_view jsonString) noexcept -> std::expected<Document, Error> {
    Document docObj;
    docObj._impl->padded = simdjson::padded_string(jsonString);
    auto error           = docObj._impl->parser.parse(docObj._impl->padded).get(docObj._impl->doc);
    if (error) {
        return std::unexpected(JSONError::InvalidJSON);
    }
    return docObj;
}

auto Document::GetRoot() const noexcept -> ValueReader {
    if (!_impl) {
        return {};
    }
    return ValueReader(&_impl->doc);
}

// --- ValueReader Implementation ---

static_assert(sizeof(simdjson::dom::element) <= sizeof(uint64_t) * 2, "simdjson element exceeds ValueReader storage size!");

ValueReader::ValueReader(const void* internalNode) {
    if (internalNode != nullptr) {
        std::memcpy(_opaque, internalNode, sizeof(simdjson::dom::element));
        _valid = true;
    }
}

auto ValueReader::GetInt() const noexcept -> std::expected<int64_t, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    int64_t val = 0;
    if (elem.get_int64().get(val) == simdjson::SUCCESS) {
        return val;
    }
    uint64_t uval = 0;
    if (elem.get_uint64().get(uval) == simdjson::SUCCESS) {
        return static_cast<int64_t>(uval);
    }
    return std::unexpected(JSONError::TypeMismatch);
}

auto ValueReader::GetUInt() const noexcept -> std::expected<uint64_t, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    uint64_t uval = 0;
    if (elem.get_uint64().get(uval) == simdjson::SUCCESS) {
        return uval;
    }
    int64_t val = 0;
    if (elem.get_int64().get(val) == simdjson::SUCCESS && val >= 0) {
        return static_cast<uint64_t>(val);
    }
    return std::unexpected(JSONError::TypeMismatch);
}

auto ValueReader::GetDouble() const noexcept -> std::expected<double, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    double val = 0.0;
    if (elem.get_double().get(val) == simdjson::SUCCESS) {
        return val;
    }
    int64_t ival = 0;
    if (elem.get_int64().get(ival) == simdjson::SUCCESS) {
        return static_cast<double>(ival);
    }
    uint64_t uval = 0;
    if (elem.get_uint64().get(uval) == simdjson::SUCCESS) {
        return static_cast<double>(uval);
    }
    return std::unexpected(JSONError::TypeMismatch);
}

auto ValueReader::GetBool() const noexcept -> std::expected<bool, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    bool val = false;
    if (elem.get_bool().get(val) == simdjson::SUCCESS) {
        return val;
    }
    return std::unexpected(JSONError::TypeMismatch);
}

auto ValueReader::GetString() const noexcept -> std::expected<std::string_view, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    std::string_view val;
    if (elem.get_string().get(val) == simdjson::SUCCESS) {
        return val;
    }
    return std::unexpected(JSONError::TypeMismatch);
}

auto ValueReader::IsNull() const noexcept -> bool {
    if (!_valid) {
        return false;
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);
    return elem.is_null();
}

auto ValueReader::GetKey(std::string_view key) const noexcept -> std::expected<ValueReader, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    simdjson::dom::element field_elem;
    auto                   err = elem.at_key(key).get(field_elem);
    if (err == simdjson::NO_SUCH_FIELD) {
        return std::unexpected(JSONError::MissingField);
    }
    if (err != simdjson::SUCCESS) {
        return std::unexpected(JSONError::TypeMismatch);
    }

    return ValueReader(&field_elem);
}

auto ValueReader::GetArraySize() const noexcept -> size_t {
    if (!_valid) {
        return 0;
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    simdjson::dom::array arr;
    if (elem.get_array().get(arr) != simdjson::SUCCESS) {
        return 0;
    }
    return arr.size();
}

auto ValueReader::GetArrayElement(size_t index) const noexcept -> std::expected<ValueReader, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    simdjson::dom::array arr;
    if (elem.get_array().get(arr) != simdjson::SUCCESS) {
        return std::unexpected(JSONError::TypeMismatch);
    }

    size_t i = 0;
    for (auto val: arr) {
        if (i == index) {
            simdjson::dom::element childElem = val;
            return ValueReader(&childElem);
        }
        i++;
    }
    return std::unexpected(JSONError::MissingField);
}

auto ValueReader::GetObjectKeys() const -> std::expected<std::vector<std::string_view>, Error> {
    if (!_valid) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    simdjson::dom::object obj;
    if (elem.get_object().get(obj) != simdjson::SUCCESS) {
        return std::unexpected(JSONError::TypeMismatch);
    }

    std::vector<std::string_view> keys;
    keys.reserve(obj.size());
    for (const auto [key, field]: obj) {
        (void) field;
        keys.push_back(key);
    }
    return keys;
}

} // namespace ZHLN::ReflectJSON
