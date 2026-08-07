// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/JSON.hpp>
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
    return ValueReader(&_impl->doc);
}

// --- ValueReader Implementation ---

std::expected<int64_t, Error> ValueReader::GetInt() const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto* elem = static_cast<const simdjson::dom::element*>(_node);
    int64_t     val  = 0;
    if (elem->get<int64_t>().get(val)) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    return val;
}

std::expected<uint64_t, Error> ValueReader::GetUInt() const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto* elem = static_cast<const simdjson::dom::element*>(_node);
    uint64_t    val  = 0;
    if (elem->get<uint64_t>().get(val)) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    return val;
}

std::expected<double, Error> ValueReader::GetDouble() const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto* elem = static_cast<const simdjson::dom::element*>(_node);
    double      val  = 0.0;
    if (elem->get<double>().get(val)) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    return val;
}

std::expected<bool, Error> ValueReader::GetBool() const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto* elem = static_cast<const simdjson::dom::element*>(_node);
    bool        val  = false;
    if (elem->get<bool>().get(val)) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    return val;
}

std::expected<std::string_view, Error> ValueReader::GetString() const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto*      elem = static_cast<const simdjson::dom::element*>(_node);
    std::string_view val;
    if (elem->get<std::string_view>().get(val)) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    return val;
}

std::expected<ValueReader, Error> ValueReader::GetKey(std::string_view key) const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto* elem = static_cast<const simdjson::dom::element*>(_node);

    simdjson::dom::object obj;
    if (elem->get<simdjson::dom::object>().get(obj)) {
        return std::unexpected(JSONError::TypeMismatch);
    }

    simdjson::dom::element field_elem;
    if (obj.at_key(key).get(field_elem)) {
        return std::unexpected(JSONError::MissingField);
    }

    static thread_local simdjson::dom::element s_tempElement;
    s_tempElement = field_elem;
    return ValueReader(&s_tempElement);
}

size_t ValueReader::GetArraySize() const noexcept {
    if (!_node)
        return 0;
    const auto*          elem = static_cast<const simdjson::dom::element*>(_node);
    simdjson::dom::array arr;
    if (elem->get<simdjson::dom::array>().get(arr)) {
        return 0;
    }
    return arr.size();
}

std::expected<ValueReader, Error> ValueReader::GetArrayElement(size_t index) const noexcept {
    if (!_node)
        return std::unexpected(JSONError::TypeMismatch);
    const auto*          elem = static_cast<const simdjson::dom::element*>(_node);
    simdjson::dom::array arr;
    if (elem->get<simdjson::dom::array>().get(arr)) {
        return std::unexpected(JSONError::TypeMismatch);
    }

    size_t i = 0;
    for (auto val: arr) {
        if (i == index) {
            static thread_local simdjson::dom::element s_tempElem;
            s_tempElem = val;
            return ValueReader(&s_tempElem);
        }
        i++;
    }
    return std::unexpected(JSONError::MissingField);
}

} // namespace ZHLN::ReflectJSON
