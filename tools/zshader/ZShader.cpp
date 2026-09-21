// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/ZShader.cpp
//
// The catalog's data, and the text helpers the four halves share: the symbol a
// module is embedded under, the identifier and literal spellings the generated
// C++ needs, the argument shapes every `--flag Name=Value` has, and the two
// lookups the emitters do. Everything here is about what the tool was told and
// what it will say -- reflection is Reflect.cpp's business, and the generated
// files are Emit.cpp's.

#include "ZShader.hpp"

#include <Zahlen/Core/Ranges.hpp>

#include <cctype>
#include <fstream>
#include <iterator>

namespace ZHLN::ZShader {

namespace {

// True for the characters a C++ identifier may start with.
auto IsIdentifierHead(char c) -> bool {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// True for the characters a C++ identifier may continue with.
auto IsIdentifierBody(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

} // namespace

auto ToSymbol(std::string_view macro) -> std::string {
    std::string symbol;
    symbol.reserve(macro.size());
    for (const char c: macro) {
        symbol.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return symbol;
}

auto IsIdentifier(std::string_view name) -> bool {
    if (name.empty() || !IsIdentifierHead(name[0])) {
        return false;
    }
    return ZHLN::Ranges::FindIf(name, [](char c) { return !IsIdentifierBody(c); }) == name.end();
}

auto AsLiteral(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char c: text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

auto ReadFile(const std::string& path) -> std::vector<uint8_t> {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        Fail("cannot open {}", path);
    }
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        Fail("cannot read {}", path);
    }
    return bytes;
}

auto SplitOnce(std::string_view text, char separator) -> std::pair<std::string_view, std::string_view> {
    const size_t at = text.find(separator);
    if (at == std::string_view::npos) {
        return {};
    }
    return {text.substr(0, at), text.substr(at + 1)};
}

auto SplitList(std::string_view text, char separator) -> std::vector<std::string> {
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (start <= text.size()) {
        const size_t end  = text.find(separator, start);
        const size_t stop = end == std::string_view::npos ? text.size() : end;
        // An empty entry is not a name: `A,,B` is two members with a typo, and
        // dropping the hole says so instead of carrying an empty type name into
        // the header.
        if (stop > start) {
            parts.emplace_back(text.substr(start, stop - start));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

auto BytesInputsOf(const Options& options) -> std::vector<BytesInput> {
    std::vector<BytesInput> inputs;
    inputs.reserve(options.modules.size() + options.blobs.size());
    for (const Module& module: options.modules) {
        inputs.push_back(BytesInput {.symbol = module.symbol, .path = module.path, .size = module.byteSize});
    }
    inputs.append_range(options.blobs);
    return inputs;
}

auto ModuleFor(const Options& options, std::string_view type, std::string_view macro) -> const Module& {
    const auto found = ZHLN::Ranges::FindIf(options.modules, [macro](const Module& module) { return module.macro == macro; });
    if (found == options.modules.end() || !found->reflected) {
        Fail("catalog type {} names macro {}, which no --bytes input carries", type, macro);
    }
    return *found;
}

auto WriteFileIfChanged(const std::string& path, const std::string& content) -> bool {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            const std::string current {std::istreambuf_iterator<char>(existing), std::istreambuf_iterator<char>()};
            if (current == content) {
                std::println("zshader: {} is up to date", path);
                return false;
            }
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Fail("cannot write {}", path);
    }
    file << content;
    if (!file) {
        Fail("cannot write {}", path);
    }
    std::println("zshader: wrote {}", path);
    return true;
}

} // namespace ZHLN::ZShader
