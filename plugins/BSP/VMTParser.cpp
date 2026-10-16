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
                // KeyValues only needs the quote escape here. A backslash in
                // material/texture paths is a path separator, not a generic
                // escape prefix, and must survive for SourceVFS to normalize.
                if (_src[_cursor] == '\\' && _cursor + 1 < _src.size() && _src[_cursor + 1] == '"') {
                    ++_cursor;
                }
                str.push_back(_src[_cursor]);
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

auto ShaderEmissiveStrength(std::string_view shader) noexcept -> float {
    const std::string name = ToLower(shader);
    if (name == "unlitgeneric" || name == "unlittwotexture" || name == "monitorscreen" || name == "sprite" || name == "spritecard" ||
        name == "cable" || name == "beam") {
        return 1.0f;
    }
    if (name == "vertexlitgeneric" || name == "vertexlittwotexture") {
        return 0.55f;
    }
    if (name == "lightmappedgeneric" || name == "worldvertextransition" || name == "patch") {
        return 0.35f;
    }
    if (name == "eyerefract" || name == "water" || name == "refract") {
        return 0.40f;
    }
    return 0.0f;
}

auto Rec709Luma(const JPH::Vec3& rgb) noexcept -> float {
    return rgb.GetX() * 0.2126f + rgb.GetY() * 0.7152f + rgb.GetZ() * 0.0722f;
}

namespace {

// Consumes the remainder of a brace block whose opening brace was already
// read. Nested braces are balanced and everything inside is dropped.
void SkipBlock(Tokenizer& tokenizer) {
    int depth = 1;
    while (depth > 0) {
        const Token tok = tokenizer.Next();
        if (tok.kind == Token::Kind::EndOfFile) {
            return;
        }
        if (tok.kind == Token::Kind::OpenBrace) {
            depth++;
        } else if (tok.kind == Token::Kind::CloseBrace) {
            depth--;
        }
    }
}

// Reads one brace block's direct key/value pairs into `out` (lowercased keys).
// A key that introduces a nested block has its block dropped: the structures
// that nest one level deeper ("proxies" and friends) carry no material keys.
void ParseBlockPairs(Tokenizer& tokenizer, std::unordered_map<std::string, std::string>& out) {
    int depth = 1;
    while (depth > 0) {
        const Token tok = tokenizer.Next();
        if (tok.kind == Token::Kind::EndOfFile) {
            return;
        }
        if (tok.kind == Token::Kind::OpenBrace) {
            SkipBlock(tokenizer);
            continue;
        }
        if (tok.kind == Token::Kind::CloseBrace) {
            depth--;
            continue;
        }
        if (tok.kind != Token::Kind::String) {
            continue;
        }
        const std::string key = ToLower(tok.value);
        const Token       valTok = tokenizer.Next();
        if (valTok.kind == Token::Kind::EndOfFile) {
            return;
        }
        if (valTok.kind == Token::Kind::OpenBrace) {
            SkipBlock(tokenizer);
            continue;
        }
        if (valTok.kind == Token::Kind::CloseBrace) {
            depth--;
            continue;
        }
        if (valTok.kind == Token::Kind::String) {
            out[key] = valTok.value;
        }
    }
}

// The nested blocks that are engine plumbing rather than material parameters:
// effect scripts, proxy lists, the editor's bookkeeping, and the pre-DX9
// shader-class variants (a modern pipeline is never those).
bool IsIgnoredNestedBlock(std::string_view lowerKey) {
    return lowerKey == "proxies" || lowerKey == "effects" || lowerKey == "editor" || lowerKey.ends_with("_dx7") || lowerKey.ends_with("_dx8");
}

// Recomputes the material's typed fields from its parameter map. Called after
// parsing and after every patch merge, so a "replace" overlay that retargets
// $basetexture or $bumpmap is not lost to a field the merge never touched.
auto DeriveVmtFields(VMTMaterial& mat) noexcept -> void {
    mat.baseTexture  = mat.parameters.contains("$basetexture") ? mat.parameters["$basetexture"] : std::string {};
    mat.bumpMap      = mat.parameters.contains("$bumpmap")     ? mat.parameters["$bumpmap"]
                       : mat.parameters.contains("$normalmap") ? mat.parameters["$normalmap"]
                                                               : std::string {};
    mat.surfaceProp  = mat.parameters.contains("$surfaceprop") ? mat.parameters["$surfaceprop"] : std::string {};
    mat.isTranslucent = mat.parameters.contains("$translucent") && mat.parameters["$translucent"] == "1";
    mat.isAlphaTest   = mat.parameters.contains("$alphatest") && mat.parameters["$alphatest"] == "1";
    mat.noCull        = mat.parameters.contains("$nocull") && mat.parameters["$nocull"] == "1";
    mat.alphaCutoff   = 0.5f;
    if (const auto it = mat.parameters.find("$alphatestreference"); it != mat.parameters.end()) {
        if (auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), mat.alphaCutoff); ec != std::errc {}) {
            mat.alphaCutoff = 0.5f;
        }
    }
    mat.baseColor = JPH::Float4 {1.0f, 1.0f, 1.0f, 1.0f};
    if (const auto it = mat.parameters.find("$color"); it != mat.parameters.end()) {
        ParseColorValues(it->second, mat.baseColor);
    } else if (const auto it2 = mat.parameters.find("$color2"); it2 != mat.parameters.end()) {
        ParseColorValues(it2->second, mat.baseColor);
    }
}

} // namespace

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

    // 3. Body keyvalues. Keys with a string value are parameters; keys with a
    // block dispatch on the block's purpose (see ParseVMT's contract).
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
        if (tok.kind != Token::Kind::String) {
            continue;
        }

        const std::string key     = std::move(tok.value);
        const std::string lowerKey = ToLower(key);
        Token             valTok  = tokenizer.Next();
        if (valTok.kind == Token::Kind::EndOfFile) {
            break;
        }
        if (valTok.kind == Token::Kind::OpenBrace) {
            if (lowerKey == "replace") {
                ParseBlockPairs(tokenizer, mat.patchReplace);
            } else if (lowerKey == "add") {
                ParseBlockPairs(tokenizer, mat.patchAdd);
            } else if (IsIgnoredNestedBlock(lowerKey)) {
                SkipBlock(tokenizer);
            } else {
                // A shader-class block ("LightmappedGeneric_DX9" and kin): its
                // keys are the material's, overriding the top level -- this is
                // where materials keep $bumpmap.
                std::unordered_map<std::string, std::string> nested;
                ParseBlockPairs(tokenizer, nested);
                for (auto& [nestedKey, nestedValue]: nested) {
                    mat.parameters[nestedKey] = std::move(nestedValue);
                }
            }
            continue;
        }
        if (valTok.kind == Token::Kind::CloseBrace) {
            braceDepth--;
            continue;
        }
        if (valTok.kind == Token::Kind::String) {
            mat.parameters[lowerKey] = std::move(valTok.value);
        }
    }

    DeriveVmtFields(mat);
    return mat;
}

auto ResolveVmtMaterial(std::string_view text, const VmtIncludeLoader& loadInclude) -> std::expected<VMTMaterial, ErrorCode> {
    auto matExp = ParseVMT(text);
    if (!matExp.has_value()) {
        return matExp;
    }
    VMTMaterial mat = std::move(*matExp);

    for (int depth = 0; depth < 4; ++depth) {
        if (ToLower(mat.shader) != "patch") {
            return mat;
        }
        const auto include = mat.Find("include");
        if (!include || include->empty()) {
            return mat;
        }
        std::string parent(*include);
        if (!parent.ends_with(".vmt")) {
            parent += ".vmt";
        }
        const auto parentText = loadInclude(parent);
        if (!parentText.has_value()) {
            return std::unexpected(VMTError::FileNotFound);
        }
        auto parentExp = ParseVMT(*parentText);
        if (!parentExp.has_value()) {
            return parentExp;
        }

        // The patch layers over the material it includes: its own stray keys,
        // then "add" (absent keys only), then "replace" (always).
        VMTMaterial merged = std::move(*parentExp);
        for (auto& [key, value]: mat.parameters) {
            if (key == "include" || key == "replace" || key == "add") {
                continue;
            }
            merged.parameters[key] = value;
        }
        for (auto& [key, value]: mat.patchAdd) {
            merged.parameters.emplace(key, value);
        }
        for (auto& [key, value]: mat.patchReplace) {
            merged.parameters[key] = value;
        }
        DeriveVmtFields(merged);
        merged.patchAdd.clear();
        merged.patchReplace.clear();
        mat = std::move(merged);
    }
    return mat;
}

} // namespace ZHLN::BSP
