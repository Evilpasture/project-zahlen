// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/JSON.hpp>
#include <cstring>
#include <simdjson.h>

namespace ZHLN::ReflectJSON {

struct Document::Impl {
    simdjson::dom::parser   parser;
    simdjson::padded_string padded;
    simdjson::dom::element  doc;
};

Document::Document(): _impl(std::make_unique<Impl>()) {
}
Document::~Document() = default;

Document::Document(Document&&) noexcept            = default;
Document& Document::operator=(Document&&) noexcept = default;

std::expected<Document, Error> Document::Parse(std::string_view jsonString) noexcept {
    Document docObj;
    docObj._impl->padded = simdjson::padded_string(jsonString);
    auto error           = docObj._impl->parser.parse(docObj._impl->padded).get(docObj._impl->doc);
    if (error) {
        return std::unexpected(JSONError::InvalidJSON);
    }
    return docObj;
}

ValueReader Document::GetRoot() const noexcept {
    if (!_impl) {
        return ValueReader();
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

std::expected<int64_t, Error> ValueReader::GetInt() const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
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

std::expected<uint64_t, Error> ValueReader::GetUInt() const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
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

std::expected<double, Error> ValueReader::GetDouble() const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
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

std::expected<bool, Error> ValueReader::GetBool() const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    bool val = false;
    if (elem.get_bool().get(val) == simdjson::SUCCESS) {
        return val;
    }
    return std::unexpected(JSONError::TypeMismatch);
}

std::expected<std::string_view, Error> ValueReader::GetString() const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    std::string_view val;
    if (elem.get_string().get(val) == simdjson::SUCCESS) {
        return val;
    }
    return std::unexpected(JSONError::TypeMismatch);
}

std::expected<ValueReader, Error> ValueReader::GetKey(std::string_view key) const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
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

size_t ValueReader::GetArraySize() const noexcept {
    if (!_valid)
        return 0;
    const auto& elem = *reinterpret_cast<const simdjson::dom::element*>(_opaque);

    simdjson::dom::array arr;
    if (elem.get_array().get(arr) != simdjson::SUCCESS) {
        return 0;
    }
    return arr.size();
}

std::expected<ValueReader, Error> ValueReader::GetArrayElement(size_t index) const noexcept {
    if (!_valid)
        return std::unexpected(JSONError::TypeMismatch);
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

} // namespace ZHLN::ReflectJSON
