// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/ZShader.cpp
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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

auto ReadFile(std::string_view path) -> std::vector<uint8_t> {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
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
        if (!module.reflected || module.spirv.empty()) {
            Fail("--bytes {} was never compiled; the manifest is the only source of shader bytes", module.macro);
        }
        inputs.push_back(BytesInput {
            .symbol          = module.symbol,
            .path            = module.path,
            .size            = static_cast<uint32_t>(module.spirv.size()),
            .hasExpectedSize = true,
            .inlineBytes     = module.spirv,
        });
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

// libslang prints diagnostics in two shapes:
//
//   legacy:  file(line[, col]): error 30019: text
//   current: error[E20002]: text
//             --> file:line:col
//
// GCC/Clang/IDEs want `file:line:col: error: text` on the line that carries
// the severity. Rewrite that line; leave context/caret lines untouched.
auto FormatSlangDiagnostics(std::string_view text, std::string_view fallbackFile) -> std::string {
    auto severityOf = [](std::string_view rest) -> std::string_view {
        for (const std::string_view word: {"fatal error", "error", "warning", "note", "info"}) {
            if (rest.starts_with(word)) {
                return word;
            }
        }
        return {};
    };
    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
            v.remove_prefix(1);
        }
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t')) {
            v.remove_suffix(1);
        }
        return v;
    };

    std::vector<std::string_view> lines;
    size_t                        start = 0;
    while (start <= text.size()) {
        const size_t end  = text.find('\n', start);
        const size_t stop = end == std::string_view::npos ? text.size() : end;
        lines.push_back(text.substr(start, stop - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    // `--> file:line:col` belongs to the severity line just before it.
    auto locationAfter = [&](size_t index) -> std::string {
        for (size_t j = index + 1; j < lines.size() && j <= index + 2; ++j) {
            const std::string_view candidate = trim(lines[j]);
            if (!candidate.starts_with("--> ")) {
                continue;
            }
            const std::string_view loc = trim(candidate.substr(4));
            const size_t colonCol  = loc.rfind(':');
            if (colonCol == std::string_view::npos) {
                continue;
            }
            const size_t colonLine = loc.rfind(':', colonCol - 1);
            if (colonLine == std::string_view::npos) {
                continue;
            }
            const std::string_view file = loc.substr(0, colonLine);
            const std::string_view line = loc.substr(colonLine + 1, colonCol - colonLine - 1);
            const std::string_view col  = loc.substr(colonCol + 1);
            bool digits = !line.empty() && !col.empty();
            for (const char c: line) {
                digits = digits && c >= '0' && c <= '9';
            }
            for (const char c: col) {
                digits = digits && c >= '0' && c <= '9';
            }
            if (digits && !file.empty()) {
                return std::string {file} + ":" + std::string {line} + ":" + std::string {col};
            }
        }
        return {};
    };

    std::string out;
    for (size_t index = 0; index < lines.size(); ++index) {
        std::string_view line = lines[index];
        bool             converted = false;
        const std::string_view body = trim(line);
        const std::string_view severity = severityOf(body);

        if (!severity.empty()) {
            // Current shape: `error[E20002]: text` (+ a following --> line).
            std::string_view rest = body.substr(severity.size());
            std::string      code;
            if (rest.starts_with("[")) {
                const size_t close = rest.find(']');
                if (close != std::string_view::npos) {
                    code = std::string {rest.substr(1, close - 1)};
                    rest = rest.substr(close + 1);
                }
            }
            std::string message = std::string {trim(rest)};
            if (message.starts_with(":")) {
                message.erase(0, 1);
                message = std::string {trim(message)};
            }
            const std::string location = locationAfter(index);
            if (!location.empty() || !fallbackFile.empty()) {
                out += (location.empty() ? std::string {fallbackFile} + ":1:1" : location) + ": " +
                       std::string {severity} + ": " + message;
                if (!code.empty()) {
                    out += " [Slang " + code + "]";
                }
                converted = true;
            }
        }
        if (!converted) {
            // Legacy shape: `file(line[, col]): error 30019: text`.
            const size_t open  = line.find('(');
            const size_t close = open == std::string_view::npos ? std::string_view::npos : line.find(')', open + 1);
            if (close != std::string_view::npos && close + 2 <= line.size() && line[close + 1] == ':') {
                const std::string_view numbers = line.substr(open + 1, close - open - 1);
                const size_t comma = numbers.find(',');
                const std::string_view lineNo = numbers.substr(0, comma);
                const std::string_view colNo  = comma == std::string_view::npos ? std::string_view {"1"} : trim(numbers.substr(comma + 1));
                const std::string_view file   = line.substr(0, open);
                std::string_view rest        = trim(line.substr(close + 2));
                const std::string_view sev   = severityOf(rest);
                if (!file.empty() && !lineNo.empty() && !sev.empty()) {
                    std::string message = std::string {trim(rest.substr(sev.size()))};
                    std::string code;
                    const size_t firstSpace = message.find(' ');
                    const std::string_view head = std::string_view {message}.substr(0, firstSpace);
                    bool digits = !head.empty();
                    for (const char c: head) {
                        digits = digits && c >= '0' && c <= '9';
                    }
                    if (digits) {
                        code = std::string {head};
                        message = std::string {trim(std::string_view {message}.substr(firstSpace))};
                        if (message.starts_with(":")) {
                            message.erase(0, 1);
                            message = std::string {trim(message)};
                        }
                    }
                    out += std::string {file} + ":" + std::string {lineNo} + ":" + std::string {colNo} + ": " +
                           std::string {sev} + ": " + message;
                    if (!code.empty()) {
                        out += " [Slang " + code + "]";
                    }
                    converted = true;
                }
            }
        }
        if (!converted) {
            out.append(line);
        }
        out.push_back('\n');
    }
    return out;
}

auto WriteFileIfChanged(std::string_view path, std::string_view content) -> bool {
    const std::filesystem::path fsPath(path);
    if (fsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fsPath.parent_path(), ec);
        if (ec) {
            Fail("cannot create the directory for {}: {}", path, ec.message());
        }
    }
    {
        std::ifstream existing(fsPath, std::ios::binary);
        if (existing) {
            std::ostringstream current;
            current << existing.rdbuf();
            if (current.str() == content) {
                std::println("zshader: {} is up to date", path);
                return false;
            }
        }
    }
    std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
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
