// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/toml/TOML.cpp
//
// The document parser behind extras/toml/TOML.hpp.
//
// Hand-written rather than vendored: the grammar the engine needs is small
// (tables, arrays of tables, inline tables, arrays, strings, numbers, bools),
// the reflection layer on top is where all the interesting behaviour lives,
// and a scene format is a bad place to inherit a dependency's error reporting.
// Dates, times and multi-line strings are the deliberate omissions -- no
// engine type maps to them, and accepting them would mean carrying a value
// kind that nothing can consume.
//
// The tree is a deque of nodes so that node addresses stay stable while the
// document is still being built (Value holds raw pointers into it).

#include <Zahlen/Log.hpp>
#include <toml/TOML.hpp>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ReflectTOML {

// Named rather than anonymous: Document::Impl holds a deque<Node>, and a
// member of an externally-linked class whose type has internal linkage is an
// ODR violation waiting to happen (and a -Wsubobject-linkage warning today).
namespace document {

enum class Kind : uint8_t { Table, Array, String, Integer, Float, Boolean };

struct Node {
    Kind kind = Kind::Table;

    // Table members, in document order. A vector of pairs rather than a map:
    // tables are small, order is part of the output, and the keys have to be
    // stable string storage for GetTableKeys to hand out views.
    std::vector<std::pair<std::string, Node*>> members;
    std::vector<Node*>                         elements;

    std::string text;
    int64_t     integer = 0;
    double      real    = 0.0;
    bool        boolean = false;

    /// Set for tables that were created by a [header] rather than implied by a
    /// dotted key, so a second [header] on the same path can be rejected.
    bool explicitTable = false;

    [[nodiscard]] auto Find(std::string_view key) noexcept -> Node* {
        for (auto& [name, node]: members) {
            if (name == key) {
                return node;
            }
        }
        return nullptr;
    }
};

/// Cursor over the document text. Every failure path logs the line number
/// before returning, because "InvalidTOML" on its own is useless in a log.
class Parser {
  public:
    Parser(std::string_view text, std::deque<Node>& arena) noexcept: _text(text), _arena(arena) {
    }

    [[nodiscard]] auto ParseDocument(Node& root) -> std::expected<void, Error> {
        Node* current = &root;

        while (true) {
            SkipInsignificant(true);
            if (AtEnd()) {
                return {};
            }

            if (Peek() == '[') {
                auto header = ParseHeader(root);
                if (!header) {
                    return std::unexpected(header.error());
                }
                current = *header;
                continue;
            }

            auto pair = ParseKeyValue(*current);
            if (!pair) {
                return std::unexpected(pair.error());
            }
        }
    }

  private:
    [[nodiscard]] auto AtEnd() const noexcept -> bool {
        return _pos >= _text.size();
    }

    [[nodiscard]] auto Peek(size_t ahead = 0) const noexcept -> char {
        return (_pos + ahead < _text.size()) ? _text[_pos + ahead] : '\0';
    }

    [[nodiscard]] auto Line() const noexcept -> size_t {
        size_t line = 1;
        for (size_t i = 0; i < _pos && i < _text.size(); ++i) {
            if (_text[i] == '\n') {
                ++line;
            }
        }
        return line;
    }

    auto Fail(std::string_view reason, Error error = TOMLError::InvalidTOML) -> std::unexpected<Error> {
        ZHLN::Log("[TOML] line {}: {}", Line(), reason);
        return std::unexpected(error);
    }

    /// Spaces, tabs, comments and -- when `newlines` -- line breaks.
    void SkipInsignificant(bool newlines) noexcept {
        while (!AtEnd()) {
            const char c = Peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                ++_pos;
            } else if (newlines && c == '\n') {
                ++_pos;
            } else if (c == '#') {
                while (!AtEnd() && Peek() != '\n') {
                    ++_pos;
                }
            } else {
                return;
            }
        }
    }

    [[nodiscard]] auto NewNode(Kind kind) -> Node* {
        _arena.emplace_back();
        _arena.back().kind = kind;
        return &_arena.back();
    }

    // --- keys -------------------------------------------------------------

    /// One segment of a key: bare, "basic" or 'literal'.
    [[nodiscard]] auto ParseKeySegment() -> std::expected<std::string, Error> {
        if (Peek() == '"' || Peek() == '\'') {
            return ParseQuotedString();
        }

        const size_t start = _pos;
        while (!AtEnd()) {
            const char c = Peek();
            const bool bare =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!bare) {
                break;
            }
            ++_pos;
        }
        if (_pos == start) {
            return Fail("expected a key");
        }
        return std::string {_text.substr(start, _pos - start)};
    }

    /// A dotted key path: a.b."c d".
    [[nodiscard]] auto ParseKeyPath() -> std::expected<std::vector<std::string>, Error> {
        std::vector<std::string> path;
        while (true) {
            auto segment = ParseKeySegment();
            if (!segment) {
                return std::unexpected(segment.error());
            }
            path.push_back(std::move(*segment));

            SkipInsignificant(false);
            if (Peek() != '.') {
                return path;
            }
            ++_pos;
            SkipInsignificant(false);
        }
    }

    /// Walks `path` from `root`, creating intermediate tables. Stops one short
    /// of the end and returns the parent plus the final segment.
    [[nodiscard]] auto ResolveParent(Node& root, const std::vector<std::string>& path) -> std::expected<Node*, Error> {
        Node* current = &root;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            Node* child = current->Find(path[i]);
            if (child == nullptr) {
                child = NewNode(Kind::Table);
                current->members.emplace_back(path[i], child);
            } else if (child->kind == Kind::Array) {
                // Dotted path through an array of tables addresses its last
                // element, which is what [[a]] followed by [a.b] means.
                if (child->elements.empty()) {
                    return Fail("dotted key walks into an empty array of tables");
                }
                child = child->elements.back();
            }
            if (child->kind != Kind::Table) {
                return Fail(std::format("'{}' is a value, not a table", path[i]));
            }
            current = child;
        }
        return current;
    }

    // --- headers ----------------------------------------------------------

    /// [table] or [[array of tables]]. Returns the table subsequent key/value
    /// lines belong to.
    [[nodiscard]] auto ParseHeader(Node& root) -> std::expected<Node*, Error> {
        const bool arrayOfTables = Peek(1) == '[';
        _pos += arrayOfTables ? 2 : 1;
        SkipInsignificant(false);

        auto path = ParseKeyPath();
        if (!path) {
            return std::unexpected(path.error());
        }
        SkipInsignificant(false);

        const size_t closers = arrayOfTables ? 2 : 1;
        for (size_t i = 0; i < closers; ++i) {
            if (Peek() != ']') {
                return Fail("unterminated table header");
            }
            ++_pos;
        }

        auto parent = ResolveParent(root, *path);
        if (!parent) {
            return std::unexpected(parent.error());
        }

        const std::string& leaf     = path->back();
        Node*              existing = (*parent)->Find(leaf);

        if (arrayOfTables) {
            if (existing == nullptr) {
                existing = NewNode(Kind::Array);
                (*parent)->members.emplace_back(leaf, existing);
            }
            if (existing->kind != Kind::Array) {
                return Fail(std::format("'{}' was already defined as something other than an array of tables", leaf), TOMLError::DuplicateKey);
            }
            Node* element = NewNode(Kind::Table);
            existing->elements.push_back(element);
            return element;
        }

        if (existing == nullptr) {
            existing = NewNode(Kind::Table);
            (*parent)->members.emplace_back(leaf, existing);
        } else if (existing->kind != Kind::Table || existing->explicitTable) {
            return Fail(std::format("table '{}' is defined twice", leaf), TOMLError::DuplicateKey);
        }
        existing->explicitTable = true;
        return existing;
    }

    // --- key/value pairs --------------------------------------------------

    [[nodiscard]] auto ParseKeyValue(Node& table) -> std::expected<void, Error> {
        auto path = ParseKeyPath();
        if (!path) {
            return std::unexpected(path.error());
        }

        SkipInsignificant(false);
        if (Peek() != '=') {
            return Fail("expected '=' after a key");
        }
        ++_pos;
        SkipInsignificant(false);

        auto value = ParseValue();
        if (!value) {
            return std::unexpected(value.error());
        }

        auto parent = ResolveParent(table, *path);
        if (!parent) {
            return std::unexpected(parent.error());
        }
        if ((*parent)->Find(path->back()) != nullptr) {
            return Fail(std::format("key '{}' is defined twice in the same table", path->back()), TOMLError::DuplicateKey);
        }
        (*parent)->members.emplace_back(path->back(), *value);

        // Nothing but a comment may follow a value on its line.
        SkipInsignificant(false);
        if (!AtEnd() && Peek() != '\n') {
            return Fail("trailing characters after a value");
        }
        return {};
    }

    // --- values -----------------------------------------------------------

    [[nodiscard]] auto ParseValue() -> std::expected<Node*, Error> {
        if (AtEnd()) {
            return Fail("expected a value");
        }

        const char c = Peek();
        if (c == '"' || c == '\'') {
            auto text = ParseQuotedString();
            if (!text) {
                return std::unexpected(text.error());
            }
            Node* node = NewNode(Kind::String);
            node->text = std::move(*text);
            return node;
        }
        if (c == '[') {
            return ParseArray();
        }
        if (c == '{') {
            return ParseInlineTable();
        }
        if (_text.compare(_pos, 4, "true") == 0) {
            _pos += 4;
            Node* node    = NewNode(Kind::Boolean);
            node->boolean = true;
            return node;
        }
        if (_text.compare(_pos, 5, "false") == 0) {
            _pos += 5;
            Node* node    = NewNode(Kind::Boolean);
            node->boolean = false;
            return node;
        }
        return ParseNumber();
    }

    [[nodiscard]] auto ParseQuotedString() -> std::expected<std::string, Error> {
        const char quote = Peek();
        ++_pos;

        std::string out;
        while (true) {
            if (AtEnd() || Peek() == '\n') {
                return Fail("unterminated string");
            }
            const char c = Peek();
            if (c == quote) {
                ++_pos;
                return out;
            }
            // Literal strings ('...') have no escapes at all, by definition.
            if (c == '\\' && quote == '"') {
                ++_pos;
                if (AtEnd()) {
                    return Fail("unterminated escape sequence");
                }
                const char escape = Peek();
                ++_pos;
                switch (escape) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u':  {
                        auto decoded = ParseUnicodeEscape();
                        if (!decoded) {
                            return std::unexpected(decoded.error());
                        }
                        out += *decoded;
                        break;
                    }
                    default: return Fail(std::format("unknown escape '\\{}'", escape));
                }
                continue;
            }
            out += c;
            ++_pos;
        }
    }

    /// \uXXXX, encoded to UTF-8. Surrogate pairs are not handled: the engine's
    /// strings are ASCII-to-UTF-8 pass-through and nothing produces them.
    [[nodiscard]] auto ParseUnicodeEscape() -> std::expected<std::string, Error> {
        if (_pos + 4 > _text.size()) {
            return Fail("truncated \\u escape");
        }
        uint32_t   code   = 0;
        const auto digits = _text.substr(_pos, 4);
        for (const char c: digits) {
            uint32_t value = 0;
            if (c >= '0' && c <= '9') {
                value = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value = static_cast<uint32_t>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                value = static_cast<uint32_t>(c - 'A') + 10;
            } else {
                return Fail("\\u escape is not four hex digits");
            }
            code = (code << 4) | value;
        }
        _pos += 4;

        std::string out;
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        return out;
    }

    [[nodiscard]] auto ParseArray() -> std::expected<Node*, Error> {
        ++_pos; // '['
        Node* node = NewNode(Kind::Array);

        while (true) {
            SkipInsignificant(true);
            if (AtEnd()) {
                return Fail("unterminated array");
            }
            if (Peek() == ']') {
                ++_pos;
                return node;
            }

            auto element = ParseValue();
            if (!element) {
                return std::unexpected(element.error());
            }
            node->elements.push_back(*element);

            SkipInsignificant(true);
            if (Peek() == ',') {
                ++_pos;
                continue; // A trailing comma before ']' is legal.
            }
            if (Peek() != ']') {
                return Fail("expected ',' or ']' in an array");
            }
        }
    }

    [[nodiscard]] auto ParseInlineTable() -> std::expected<Node*, Error> {
        ++_pos; // '{'
        Node* node = NewNode(Kind::Table);

        SkipInsignificant(false);
        if (Peek() == '}') {
            ++_pos;
            return node;
        }

        while (true) {
            SkipInsignificant(false);
            auto path = ParseKeyPath();
            if (!path) {
                return std::unexpected(path.error());
            }

            SkipInsignificant(false);
            if (Peek() != '=') {
                return Fail("expected '=' in an inline table");
            }
            ++_pos;
            SkipInsignificant(false);

            auto value = ParseValue();
            if (!value) {
                return std::unexpected(value.error());
            }

            auto parent = ResolveParent(*node, *path);
            if (!parent) {
                return std::unexpected(parent.error());
            }
            if ((*parent)->Find(path->back()) != nullptr) {
                return Fail(std::format("key '{}' is defined twice in the same inline table", path->back()), TOMLError::DuplicateKey);
            }
            (*parent)->members.emplace_back(path->back(), *value);

            SkipInsignificant(false);
            if (Peek() == ',') {
                ++_pos;
                continue;
            }
            if (Peek() == '}') {
                ++_pos;
                return node;
            }
            return Fail("expected ',' or '}' in an inline table");
        }
    }

    /// Integers, floats and the inf/nan spellings. Underscores are stripped
    /// before the number is handed to from_chars.
    [[nodiscard]] auto ParseNumber() -> std::expected<Node*, Error> {
        const size_t start = _pos;
        std::string  digits;
        bool         isFloat = false;

        if (Peek() == '+' || Peek() == '-') {
            digits += Peek();
            ++_pos;
        }

        if (_text.compare(_pos, 3, "inf") == 0) {
            _pos += 3;
            Node* node = NewNode(Kind::Float);
            node->real = digits == "-" ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            return node;
        }
        if (_text.compare(_pos, 3, "nan") == 0) {
            _pos += 3;
            Node* node = NewNode(Kind::Float);
            node->real = std::numeric_limits<double>::quiet_NaN();
            return node;
        }

        while (!AtEnd()) {
            const char c = Peek();
            if (c == '_') {
                ++_pos; // Digit separators carry no meaning.
                continue;
            }
            if (c >= '0' && c <= '9') {
                digits += c;
                ++_pos;
                continue;
            }
            if (c == '.' || c == 'e' || c == 'E') {
                isFloat = true;
                digits += c;
                ++_pos;
                continue;
            }
            if ((c == '+' || c == '-') && !digits.empty() && (digits.back() == 'e' || digits.back() == 'E')) {
                digits += c;
                ++_pos;
                continue;
            }
            break;
        }

        if (digits.empty() || digits == "-" || digits == "+") {
            _pos = start;
            return Fail("expected a value");
        }

        if (isFloat) {
            // strtod rather than from_chars: the floating-point overload is
            // still not universally available in the standard libraries this
            // builds against, and a scene file is not worth the portability
            // gamble.
            char*        end   = nullptr;
            const double value = std::strtod(digits.c_str(), &end);
            if (end != digits.c_str() + digits.size()) {
                return Fail(std::format("'{}' is not a number", digits));
            }
            Node* node = NewNode(Kind::Float);
            node->real = value;
            return node;
        }

        Node*      node   = NewNode(Kind::Integer);
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), node->integer);
        if (result.ec != std::errc {} || result.ptr != digits.data() + digits.size()) {
            return Fail(std::format("'{}' is not an integer", digits));
        }
        return node;
    }

    std::string_view  _text;
    std::deque<Node>& _arena;
    size_t            _pos = 0;
};

[[nodiscard]] inline auto AsNode(const void* pointer) noexcept -> const Node* {
    return static_cast<const Node*>(pointer);
}

} // namespace document

using namespace document;

// --- Document ---------------------------------------------------------------

struct Document::Impl {
    std::deque<Node> arena;
    Node*            root = nullptr;
};

Document::Document(): _impl(std::make_unique<Impl>()) {
}
Document::~Document() = default;

Document::Document(Document&&) noexcept                    = default;
auto Document::operator=(Document&&) noexcept -> Document& = default;

auto Document::Parse(std::string_view tomlText) noexcept -> std::expected<Document, Error> {
    Document doc;

    doc._impl->arena.emplace_back();
    doc._impl->root                = &doc._impl->arena.back();
    doc._impl->root->kind          = Kind::Table;
    doc._impl->root->explicitTable = true;

    Parser     parser(tomlText, doc._impl->arena);
    const auto result = parser.ParseDocument(*doc._impl->root);
    if (!result) {
        return std::unexpected(result.error());
    }
    return doc;
}

auto Document::GetRoot() const noexcept -> Value {
    if (!_impl || _impl->root == nullptr) {
        return {};
    }
    return Value {_impl->root};
}

// --- Value ------------------------------------------------------------------

auto Value::GetInt() const noexcept -> std::expected<int64_t, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr) {
        return std::unexpected(TOMLError::MissingField);
    }
    if (node->kind == Kind::Integer) {
        return node->integer;
    }
    // A float only converts when it is exactly an integer, so `count = 2.5`
    // fails loudly instead of silently becoming 2.
    if (node->kind == Kind::Float && node->real == static_cast<double>(static_cast<int64_t>(node->real))) {
        return static_cast<int64_t>(node->real);
    }
    return std::unexpected(TOMLError::TypeMismatch);
}

auto Value::GetUInt() const noexcept -> std::expected<uint64_t, Error> {
    const auto signedValue = GetInt();
    if (!signedValue) {
        return std::unexpected(signedValue.error());
    }
    if (*signedValue < 0) {
        return std::unexpected(TOMLError::TypeMismatch);
    }
    return static_cast<uint64_t>(*signedValue);
}

auto Value::GetDouble() const noexcept -> std::expected<double, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr) {
        return std::unexpected(TOMLError::MissingField);
    }
    if (node->kind == Kind::Float) {
        return node->real;
    }
    if (node->kind == Kind::Integer) {
        return static_cast<double>(node->integer);
    }
    return std::unexpected(TOMLError::TypeMismatch);
}

auto Value::GetBool() const noexcept -> std::expected<bool, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr) {
        return std::unexpected(TOMLError::MissingField);
    }
    if (node->kind != Kind::Boolean) {
        return std::unexpected(TOMLError::TypeMismatch);
    }
    return node->boolean;
}

auto Value::GetString() const noexcept -> std::expected<std::string_view, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr) {
        return std::unexpected(TOMLError::MissingField);
    }
    if (node->kind != Kind::String) {
        return std::unexpected(TOMLError::TypeMismatch);
    }
    return std::string_view {node->text};
}

auto Value::GetKey(std::string_view key) const noexcept -> std::expected<Value, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr) {
        return std::unexpected(TOMLError::MissingField);
    }
    if (node->kind != Kind::Table) {
        return std::unexpected(TOMLError::TypeMismatch);
    }
    for (const auto& [name, child]: node->members) {
        if (name == key) {
            return Value {child};
        }
    }
    return std::unexpected(TOMLError::MissingField);
}

auto Value::HasKey(std::string_view key) const noexcept -> bool {
    return GetKey(key).has_value();
}

auto Value::GetTableKeys() const -> std::expected<std::vector<std::string_view>, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr) {
        return std::unexpected(TOMLError::MissingField);
    }
    if (node->kind != Kind::Table) {
        return std::unexpected(TOMLError::TypeMismatch);
    }
    std::vector<std::string_view> keys;
    keys.reserve(node->members.size());
    for (const auto& [name, child]: node->members) {
        keys.emplace_back(name);
    }
    return keys;
}

auto Value::IsArray() const noexcept -> bool {
    const Node* node = AsNode(_node);
    return node != nullptr && node->kind == Kind::Array;
}

auto Value::GetArraySize() const noexcept -> size_t {
    const Node* node = AsNode(_node);
    if (node == nullptr || node->kind != Kind::Array) {
        return 0;
    }
    return node->elements.size();
}

auto Value::GetArrayElement(size_t index) const noexcept -> std::expected<Value, Error> {
    const Node* node = AsNode(_node);
    if (node == nullptr || node->kind != Kind::Array) {
        return std::unexpected(TOMLError::TypeMismatch);
    }
    if (index >= node->elements.size()) {
        return std::unexpected(TOMLError::MissingField);
    }
    return Value {node->elements[index]};
}

} // namespace ZHLN::ReflectTOML
