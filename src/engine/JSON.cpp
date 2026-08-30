// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/JSON.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <simdjson.h>
#include <string>
#include <utility>
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

// --- Value (Writer) Implementation ---

struct Value::Impl {
    enum class Kind : uint8_t { Object, Array, String, Number, Bool, Null };

    Kind kind = Kind::Null;
    // Payloads for the non-container kinds.
    std::string text;
    double      number = 0.0;
    bool        flag   = false;
    // Insertion order is preserved on purpose: written files stay diffable.
    std::vector<std::pair<std::string, std::unique_ptr<Impl>>> members;
    std::vector<std::unique_ptr<Impl>>                         items;

    void Append(std::string& out, size_t indent, size_t depth) const;
};

Value::Value(): _impl(std::make_unique<Impl>()) {
}
Value::~Value() = default;

Value::Value(Value&&) noexcept                    = default;
auto Value::operator=(Value&&) noexcept -> Value& = default;

auto Value::Object() -> Value {
    Value value;
    value._impl->kind = Impl::Kind::Object;
    return value;
}
auto Value::Array() -> Value {
    Value value;
    value._impl->kind = Impl::Kind::Array;
    return value;
}
auto Value::String(std::string_view text) -> Value {
    Value value;
    value._impl->kind = Impl::Kind::String;
    value._impl->text = std::string {text};
    return value;
}
auto Value::Number(double number) -> Value {
    Value value;
    value._impl->kind   = Impl::Kind::Number;
    value._impl->number = number;
    return value;
}
auto Value::Bool(bool flag) -> Value {
    Value value;
    value._impl->kind = Impl::Kind::Bool;
    value._impl->flag = flag;
    return value;
}
auto Value::Null() -> Value {
    Value value;
    value._impl->kind = Impl::Kind::Null;
    return value;
}

auto Value::Set(std::string_view key, Value value) -> std::expected<void, Error> {
    if (_impl->kind != Impl::Kind::Object) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    const auto found = std::ranges::find_if(_impl->members, [&](const auto& member) {
        return member.first == key;
    });
    if (found != _impl->members.end()) {
        found->second = std::move(value._impl);
    } else {
        _impl->members.emplace_back(std::string {key}, std::move(value._impl));
    }
    return {};
}

auto Value::Push(Value value) -> std::expected<void, Error> {
    if (_impl->kind != Impl::Kind::Array) {
        return std::unexpected(JSONError::TypeMismatch);
    }
    _impl->items.push_back(std::move(value._impl));
    return {};
}

static auto EscapeJsonString(std::string_view text) -> std::string {
    static constexpr std::string_view kHexDigits {"0123456789abcdef"};

    std::string out;
    out.reserve(text.size() + 2);
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
    return out;
}

void Value::Impl::Append(std::string& out, size_t indent, size_t depth) const {
    const auto appendString = [&](std::string_view text) {
        out += '"';
        out += EscapeJsonString(text);
        out += '"';
    };

    switch (kind) {
        case Kind::Object:
        case Kind::Array: {
            const bool pretty = indent > 0;
            const char open   = kind == Kind::Object ? '{' : '[';
            const char close  = kind == Kind::Object ? '}' : ']';

            out += open;
            if (pretty) {
                out += '\n';
                out.append((depth + 1) * indent, ' ');
            }

            const auto separator = [&] {
                if (pretty) {
                    out += ",\n";
                    out.append((depth + 1) * indent, ' ');
                } else {
                    out += ',';
                }
            };

            bool first = true;
            if (kind == Kind::Object) {
                for (const auto& [key, child]: members) {
                    if (!first) {
                        separator();
                    }
                    first = false;
                    appendString(key);
                    out += pretty ? ": " : ":";
                    child->Append(out, indent, depth + 1);
                }
            } else {
                for (const auto& child: items) {
                    if (!first) {
                        separator();
                    }
                    first = false;
                    child->Append(out, indent, depth + 1);
                }
            }

            if (pretty) {
                out += '\n';
                out.append(depth * indent, ' ');
            }
            out += close;
            break;
        }
        case Kind::String:
            appendString(text);
            break;
        case Kind::Number:
            // JSON cannot express NaN/Inf; null keeps the document parseable.
            if (std::isfinite(number)) {
                out += std::format("{}", number); // shortest round-trippable form
            } else {
                out += "null";
            }
            break;
        case Kind::Bool:
            out += flag ? "true" : "false";
            break;
        case Kind::Null:
            out += "null";
            break;
    }
}

auto Value::Stringify(size_t indent) const -> std::string {
    std::string out;
    out.reserve(256);
    _impl->Append(out, indent, 0);
    return out;
}

} // namespace ZHLN::ReflectJSON
