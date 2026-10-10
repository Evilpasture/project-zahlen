// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VMTParser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <vector>

namespace ZHLN::BSP {

namespace {

auto ToLower(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (char c: s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

struct Token {
    enum class Kind {
        String,
        OpenBrace,
        CloseBrace,
        EndOfFile
    };
    Kind        kind = Kind::EndOfFile;
    std::string value;
};

class Tokenizer {
  public:
    explicit Tokenizer(std::string_view source) : _src(source) {}

    auto Next() -> Token {
        SkipWhitespaceAndComments();
        if (_cursor >= _src.size()) {
            return Token {.kind = Token::Kind::EndOfFile, .value = ""};
        }

        const char c = _src[_cursor];
        if (c == '{') {
            _cursor++;
            return Token {.kind = Token::Kind::OpenBrace, .value = "{"};
        }
        if (c == '}') {
            _cursor++;
            return Token {.kind = Token::Kind::CloseBrace, .value = "}"};
        }

        if (c == '"') {
            // Quoted string
            _cursor++;
            std::string str;
            while (_cursor < _src.size() && _src[_cursor] != '"') {
                if (_src[_cursor] == '\\' && _cursor + 1 < _src.size()) {
                    _cursor++;
                    str.push_back(_src[_cursor]);
                } else {
                    str.push_back(_src[_cursor]);
                }
                _cursor++;
            }
            if (_cursor < _src.size() && _src[_cursor] == '"') {
                _cursor++;
            }
            return Token {.kind = Token::Kind::String, .value = std::move(str)};
        }

        // Bare word
        const size_t start = _cursor;
        while (_cursor < _src.size() && !std::isspace(static_cast<unsigned char>(_src[_cursor])) &&
               _src[_cursor] != '{' && _src[_cursor] != '}' && _src[_cursor] != '"') {
            _cursor++;
        }
        return Token {.kind = Token::Kind::String, .value = std::string(_src.substr(start, _cursor - start))};
    }

  private:
    void SkipWhitespaceAndComments() {
        while (_cursor < _src.size()) {
            const char c = _src[_cursor];
            if (std::isspace(static_cast<unsigned char>(c))) {
                _cursor++;
                continue;
            }
            if (c == '/' && _cursor + 1 < _src.size() && _src[_cursor + 1] == '/') {
                // Line comment
                _cursor += 2;
                while (_cursor < _src.size() && _src[_cursor] != '\n') {
                    _cursor++;
                }
                continue;
            }
            break;
        }
    }

    std::string_view _src;
    size_t           _cursor = 0;
};

void ParseColorValues(std::string_view val, JPH::Float4& outColor) {
    if (val.empty()) {
        return;
    }
    // Formats: "[1 1 1]", "{255 255 255}", "1 1 1"
    const bool  isByteColor = (val.front() == '{');
    const char* cur         = val.data();
    const char* end         = val.data() + val.size();
    float       nums[4]     = {1.0f, 1.0f, 1.0f, 1.0f};
    int         count       = 0;

    while (cur < end && count < 4) {
        while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '[' || *cur == ']' || *cur == '{' || *cur == '}' || *cur == '"')) {
            cur++;
        }
        if (cur >= end) {
            break;
        }
        float f = 0.0f;
        auto [ptr, ec] = std::from_chars(cur, end, f);
        if (ec != std::errc {}) {
            break;
        }
        nums[count++] = f;
        cur           = ptr;
    }

    if (count >= 3) {
        const float scale = isByteColor ? (1.0f / 255.0f) : 1.0f;
        outColor.x        = nums[0] * scale;
        outColor.y        = nums[1] * scale;
        outColor.z        = nums[2] * scale;
        outColor.w        = (count >= 4) ? (nums[3] * scale) : 1.0f;
    }
}

} // namespace

auto VMTMaterial::Find(std::string_view key) const noexcept -> std::optional<std::string_view> {
    if (auto it = parameters.find(ToLower(key)); it != parameters.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto VMTMaterial::Has(std::string_view key) const noexcept -> bool {
    return Find(key).has_value();
}

auto VMTMaterial::Get(std::string_view key, std::string_view defaultVal) const noexcept -> std::string_view {
    return Find(key).value_or(defaultVal);
}

auto ParseVMT(std::string_view text) -> std::expected<VMTMaterial, ErrorCode> {
    Tokenizer tokenizer(text);

    // 1. Shader name (first token)
    Token tok = tokenizer.Next();
    if (tok.kind != Token::Kind::String) {
        return std::unexpected(VMTError::UnexpectedToken);
    }

    VMTMaterial mat;
    mat.shader = std::move(tok.value);

    // 2. Open brace
    tok = tokenizer.Next();
    if (tok.kind != Token::Kind::OpenBrace) {
        return std::unexpected(VMTError::InvalidSyntax);
    }

    // 3. Body keyvalues
    int braceDepth = 1;
    while (braceDepth > 0) {
        tok = tokenizer.Next();
        if (tok.kind == Token::Kind::EndOfFile) {
            break;
        }
        if (tok.kind == Token::Kind::OpenBrace) {
            braceDepth++;
            continue;
        }
        if (tok.kind == Token::Kind::CloseBrace) {
            braceDepth--;
            continue;
        }

        if (tok.kind == Token::Kind::String) {
            std::string key = std::move(tok.value);
            Token valTok = tokenizer.Next();
            if (valTok.kind == Token::Kind::OpenBrace) {
                // Nested block (e.g. "proxies" { ... })
                braceDepth++;
                continue;
            }
            if (valTok.kind == Token::Kind::String) {
                const std::string lowerKey = ToLower(key);
                mat.parameters[lowerKey] = valTok.value;

                if (lowerKey == "$basetexture") {
                    mat.baseTexture = valTok.value;
                } else if (lowerKey == "$bumpmap" || lowerKey == "$normalmap") {
                    mat.bumpMap = valTok.value;
                } else if (lowerKey == "$surfaceprop") {
                    mat.surfaceProp = valTok.value;
                } else if (lowerKey == "$translucent") {
                    mat.isTranslucent = (valTok.value == "1");
                } else if (lowerKey == "$alphatest") {
                    mat.isAlphaTest = (valTok.value == "1");
                } else if (lowerKey == "$alphatestreference") {
                    float cutoff = 0.5f;
                    if (auto [ptr, ec] = std::from_chars(valTok.value.data(), valTok.value.data() + valTok.value.size(), cutoff); ec == std::errc {}) {
                        mat.alphaCutoff = cutoff;
                    }
                } else if (lowerKey == "$nocull") {
                    mat.noCull = (valTok.value == "1");
                } else if (lowerKey == "$color" || lowerKey == "$color2") {
                    ParseColorValues(valTok.value, mat.baseColor);
                }
            }
        }
    }

    return mat;
}

} // namespace ZHLN::BSP
