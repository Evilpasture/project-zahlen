// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/Fonts.cpp
//
// The production baked-font loader: fontbm `.fnt`+`.png` pairs and cooked
// 'FNT0' containers into core's BakedFontAsset. No outline-font parser exists
// here either -- everything this file reads was baked offline.

#include "Fonts.hpp"
#include "FontBMParser.hpp"

#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <stb_image.h>

namespace fs = std::filesystem;

namespace ZHLN::Fonts {

namespace {

// --- Small scanners ---------------------------------------------------------

auto ParseFloat(std::string_view token, float fallback = 0.0f) -> float {
    char buf[32];
    const size_t n = std::min(token.size(), sizeof(buf) - 1);
    std::memcpy(buf, token.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    return (end == buf) ? fallback : v;
}

auto ParseU32(std::string_view token, uint32_t fallback = 0) -> uint32_t {
    char buf[32];
    const size_t n = std::min(token.size(), sizeof(buf) - 1);
    std::memcpy(buf, token.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    const unsigned long v = std::strtoul(buf, &end, 10);
    return (end == buf) ? fallback : static_cast<uint32_t>(v);
}

/// One `key=value` pair, value unquoted when it was quoted.
struct Token {
    std::string_view key;
    std::string_view value;
};

/// Tokenises the rest of a BMFont line into `key=value` pairs. Values may be
/// double-quoted (face names, page files).
auto ScanKeyValues(std::string_view line, auto&& onToken) -> void {
    size_t i = 0;
    while (i < line.size()) {
        while ((i < line.size()) && ((line[i] == ' ') || (line[i] == '\t'))) {
            ++i;
        }
        const size_t keyBegin = i;
        while ((i < line.size()) && (line[i] != '=') && (line[i] != ' ') && (line[i] != '\t')) {
            ++i;
        }
        if ((i >= line.size()) || (line[i] != '=')) {
            // Skip any bare word (should not occur in a well-formed descriptor).
            if (i == keyBegin) {
                ++i;
            }
            continue;
        }
        const std::string_view key = line.substr(keyBegin, i - keyBegin);
        ++i; // '='
        if (i < line.size() && line[i] == '"') {
            ++i;
            const size_t valueBegin = i;
            while ((i < line.size()) && (line[i] != '"')) {
                ++i;
            }
            onToken(Token {key, line.substr(valueBegin, i - valueBegin)});
            if (i < line.size()) {
                ++i; // closing quote
            }
        } else {
            const size_t valueBegin = i;
            while ((i < line.size()) && (line[i] != ' ') && (line[i] != '\t')) {
                ++i;
            }
            onToken(Token {key, line.substr(valueBegin, i - valueBegin)});
        }
    }
}

// --- Byte sources: mounted paks first, then unpacked files -------------------

auto ReadVirtual(CreativeWorksManager& mgr, std::string_view path, std::vector<uint8_t>& out) -> bool {
    CreativeWorkLoadRequest req;
    req.assetID = HashCreativeWorkPath(path);
    if (!mgr.LoadSync(req) || (req.outData == nullptr) || (req.outSize == 0)) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(req.outData);
    out.assign(bytes, bytes + req.outSize);
    mgr.FreeCreativeWorkMemory(req);
    return true;
}

auto ReadUnpacked(std::string_view path, std::vector<uint8_t>& out) -> bool {
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

/// Reads @p path from the paks (when a manager is attached) and then from the
/// working directory (when the source allows unpacked fallbacks).
auto ReadBytes(const BakedFontSource& source, CreativeWorksManager* assets, std::string_view path, std::vector<uint8_t>& out) -> bool {
    if (assets != nullptr) {
        if (ReadVirtual(*assets, path, out)) {
            return true;
        }
    }
    return source.allowUnpackedFallback && ReadUnpacked(path, out);
}

auto JoinVirtualDir(std::string_view base, std::string_view name) -> std::string {
    const size_t slash = base.find_last_of('/');
    std::string  joined = (slash == std::string_view::npos) ? std::string() : std::string(base.substr(0, slash + 1));
    joined += name;
    return joined;
}

// --- Resolution -------------------------------------------------------------

struct LoaderInstance {
    CreativeWorksManager* assets = nullptr;
    BakedFontSource       source;
    GUI::BakedFontAsset   cache;
    bool                  attempted = false;
};

auto LoadFontBMPair(LoaderInstance& self, GUI::BakedFontAsset& out) -> bool {
    std::vector<uint8_t> fntBytes;
    if (!ReadBytes(self.source, self.assets, self.source.fntPath, fntBytes)) {
        return false;
    }

    const std::string_view text(reinterpret_cast<const char*>(fntBytes.data()), fntBytes.size());
    auto                   desc = ParseFontBMDescriptor(text);
    if (!desc) {
        Log("WARNING: BMFont descriptor {} failed to parse; trying the cooked font.", self.source.fntPath);
        return false;
    }

    std::vector<uint8_t> pngBytes;
    const std::string    pagePath = JoinVirtualDir(self.source.fntPath, desc->pageFile);
    if (!ReadBytes(self.source, self.assets, pagePath, pngBytes)) {
        Log("WARNING: BMFont coverage page {} not found.", pagePath);
        return false;
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(pngBytes.data(), static_cast<int>(pngBytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        Log("WARNING: BMFont coverage page {} failed to decode.", pagePath);
        return false;
    }

    const std::span<const uint8_t> rgba(pixels, static_cast<size_t>(width) * height * 4);
    auto                           baked = AssembleBakedFont(*desc, rgba);
    stbi_image_free(pixels);

    if (!baked) {
        Log("WARNING: BMFont bake {} + {} failed to assemble.", self.source.fntPath, pagePath);
        return false;
    }

    Log("Loaded baked font: {} + {} ({} glyphs).", self.source.fntPath, pagePath, baked->glyphs.size());
    out = std::move(*baked);
    return true;
}

auto LoadCookedFont(LoaderInstance& self, GUI::BakedFontAsset& out) -> bool {
    std::vector<uint8_t> zfontBytes;
    if (!ReadBytes(self.source, self.assets, self.source.zfontPath, zfontBytes)) {
        return false;
    }
    auto baked = GUI::DecodeCookedFont(std::span<const std::byte>(reinterpret_cast<const std::byte*>(zfontBytes.data()), zfontBytes.size()));
    if (!baked) {
        Log("WARNING: Cooked font {} failed to decode.", self.source.zfontPath);
        return false;
    }
    Log("Loaded cooked font: {} ({} glyphs).", self.source.zfontPath, baked->glyphs.size());
    out = std::move(*baked);
    return true;
}

auto LoaderFn(void* user, GUI::BakedFontAsset& out) -> bool {
    auto& self = *static_cast<LoaderInstance*>(user);
    if (!self.attempted) {
        self.attempted = true;
        self.cache     = GUI::BakedFontAsset {};
        if (!LoadFontBMPair(self, self.cache)) {
            LoadCookedFont(self, self.cache);
        }
    }
    if (self.cache.coverage.empty()) {
        return false;
    }
    out = self.cache;
    return true;
}

LoaderInstance g_instance;

} // namespace

// --- FontBM Descriptor Parsing ----------------------------------------------
// Supports both:
//  * classic AngelCode text format (key=value per line)
//  * JSON format (fontbm --data-format json, the default of tools/fontbm.sh)
// No external JSON library is pulled in; a tiny hand-rolled scanner extracts
// only the fields the runtime needs.

namespace {

// ---- Minimal JSON helpers (no external lib) --------------------------------

inline void JsonSkipWs(std::string_view t, size_t& p) {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) {
        ++p;
    }
}

inline bool JsonIsDigit(char c) {
    return c >= '0' && c <= '9';
}

// Skip a JSON string, assuming t[p]=='"'. Advances p to after closing quote.
// Handles \" \\ \/ \b \f \n \r \t and \uXXXX (skipped).
inline bool JsonSkipString(std::string_view t, size_t& p) {
    if (p >= t.size() || t[p] != '"') {
        return false;
    }
    ++p; // opening "
    while (p < t.size()) {
        char c = t[p];
        if (c == '\\') {
            // escape
            ++p;
            if (p < t.size()) {
                if (t[p] == 'u') {
                    // \uXXXX
                    ++p;
                    for (int i = 0; i < 4 && p < t.size(); ++i) {
                        ++p;
                    }
                } else {
                    ++p;
                }
            }
        } else if (c == '"') {
            ++p;
            return true;
        } else {
            ++p;
        }
    }
    return false;
}

// Parse a JSON string, returning unescaped content. Assumes t[p]=='"'.
inline std::string JsonParseString(std::string_view t, size_t& p) {
    std::string out;
    if (p >= t.size() || t[p] != '"') {
        return out;
    }
    ++p;
    out.reserve(32);
    while (p < t.size()) {
        char c = t[p];
        if (c == '\\') {
            ++p;
            if (p >= t.size()) break;
            char e = t[p];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    // \uXXXX -> best-effort: if ASCII, emit it, else '?'
                    if (p + 4 < t.size()) {
                        // hex
                        auto hexVal = [](char ch) -> int {
                            if (ch >= '0' && ch <= '9') return ch - '0';
                            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                            return -1;
                        };
                        int v = 0;
                        bool ok = true;
                        for (int i = 1; i <= 4; ++i) {
                            int hv = hexVal(t[p + i]);
                            if (hv < 0) { ok = false; break; }
                            v = (v << 4) | hv;
                        }
                        if (ok && v >= 0 && v < 128) {
                            out.push_back(static_cast<char>(v));
                        } else if (ok) {
                            out.push_back('?');
                        }
                        p += 4;
                    }
                    break;
                }
                default: out.push_back(e); break;
            }
            ++p;
        } else if (c == '"') {
            ++p;
            break;
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return out;
}

inline double JsonParseNumber(std::string_view t, size_t& p) {
    size_t start = p;
    if (p < t.size() && (t[p] == '-' || t[p] == '+')) ++p;
    while (p < t.size() && (JsonIsDigit(t[p]) || t[p] == '.' || t[p] == 'e' || t[p] == 'E' || t[p] == '+' || t[p] == '-')) {
        ++p;
    }
    std::string_view num = t.substr(start, p - start);
    char buf[64];
    size_t n = std::min(num.size(), sizeof(buf) - 1);
    std::memcpy(buf, num.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    return (end == buf) ? 0.0 : v;
}

// Find matching closing bracket/brace, handling strings and nesting.
// t[start]==open. Returns position of matching close, or npos.
inline size_t JsonFindMatching(std::string_view t, size_t start, char open, char close) {
    if (start >= t.size() || t[start] != open) return std::string_view::npos;
    int depth = 0;
    size_t p = start;
    while (p < t.size()) {
        char c = t[p];
        if (c == '"') {
            if (!JsonSkipString(t, p)) return std::string_view::npos;
            continue;
        }
        if (c == open) {
            ++depth;
        } else if (c == close) {
            --depth;
            if (depth == 0) return p;
        }
        ++p;
    }
    return std::string_view::npos;
}

inline void JsonSkipValue(std::string_view t, size_t& p) {
    JsonSkipWs(t, p);
    if (p >= t.size()) return;
    char c = t[p];
    if (c == '"') {
        JsonSkipString(t, p);
    } else if (c == '{') {
        size_t m = JsonFindMatching(t, p, '{', '}');
        p = (m == std::string_view::npos) ? t.size() : m + 1;
    } else if (c == '[') {
        size_t m = JsonFindMatching(t, p, '[', ']');
        p = (m == std::string_view::npos) ? t.size() : m + 1;
    } else {
        // number / true / false / null
        while (p < t.size() && t[p] != ',' && t[p] != '}' && t[p] != ']' && t[p] != '\n' && t[p] != '\r' && t[p] != ' ' && t[p] != '\t') {
            ++p;
        }
    }
}

// Extract a number field from a JSON object substring (object includes braces).
// Returns true if found.
inline bool JsonExtractNumberInObject(std::string_view obj, std::string_view key, double& out) {
    // search for "key"
    std::string quoted = std::string("\"") + std::string(key) + "\"";
    size_t pos = 0;
    while (true) {
        size_t k = obj.find(quoted, pos);
        if (k == std::string_view::npos) return false;
        size_t p = k + quoted.size();
        JsonSkipWs(obj, p);
        if (p >= obj.size() || obj[p] != ':') { pos = p; continue; }
        ++p;
        JsonSkipWs(obj, p);
        if (p >= obj.size()) return false;
        // number expected
        size_t numStart = p;
        double v = JsonParseNumber(obj, p);
        // check that we actually parsed something (p advanced)
        if (p == numStart) { pos = p; continue; }
        out = v;
        return true;
    }
}

inline bool JsonExtractStringInObject(std::string_view obj, std::string_view key, std::string& out) {
    std::string quoted = std::string("\"") + std::string(key) + "\"";
    size_t pos = 0;
    while (true) {
        size_t k = obj.find(quoted, pos);
        if (k == std::string_view::npos) return false;
        size_t p = k + quoted.size();
        JsonSkipWs(obj, p);
        if (p >= obj.size() || obj[p] != ':') { pos = p; continue; }
        ++p;
        JsonSkipWs(obj, p);
        if (p >= obj.size() || obj[p] != '"') { pos = p; continue; }
        out = JsonParseString(obj, p);
        return true;
    }
}

// Parse pages array: first entry as string, or object with "file"
inline bool JsonParsePagesArray(std::string_view arr, std::string& outPageFile) {
    // arr includes [ ]
    size_t p = 0;
    JsonSkipWs(arr, p);
    if (p >= arr.size() || arr[p] != '[') return false;
    ++p;
    JsonSkipWs(arr, p);
    if (p < arr.size() && arr[p] == ']') return false; // empty
    // first element
    if (arr[p] == '"') {
        outPageFile = JsonParseString(arr, p);
        return !outPageFile.empty();
    } else if (arr[p] == '{') {
        size_t objStart = p;
        size_t objEnd = JsonFindMatching(arr, objStart, '{', '}');
        if (objEnd == std::string_view::npos) return false;
        std::string_view obj = arr.substr(objStart, objEnd - objStart + 1);
        std::string file;
        if (JsonExtractStringInObject(obj, "file", file) && !file.empty()) {
            outPageFile = file;
            return true;
        }
        return false;
    }
    return false;
}

// Parse chars array into vector<FontBMChar>
inline bool JsonParseCharsArray(std::string_view arr, std::vector<FontBMChar>& outChars) {
    size_t p = 0;
    JsonSkipWs(arr, p);
    if (p >= arr.size() || arr[p] != '[') return false;
    ++p;
    while (true) {
        JsonSkipWs(arr, p);
        if (p >= arr.size()) break;
        if (arr[p] == ']') { ++p; break; }
        if (arr[p] != '{') {
            // skip non-object
            JsonSkipValue(arr, p);
            JsonSkipWs(arr, p);
            if (p < arr.size() && arr[p] == ',') { ++p; continue; }
            if (p < arr.size() && arr[p] == ']') { ++p; break; }
            continue;
        }
        size_t objStart = p;
        size_t objEnd = JsonFindMatching(arr, objStart, '{', '}');
        if (objEnd == std::string_view::npos) return false;
        std::string_view obj = arr.substr(objStart, objEnd - objStart + 1);

        FontBMChar ch{};
        bool hasId = false;
        double v = 0;

        if (JsonExtractNumberInObject(obj, "id", v)) { ch.id = static_cast<uint32_t>(v); hasId = true; }
        if (JsonExtractNumberInObject(obj, "x", v)) ch.x = static_cast<float>(v);
        if (JsonExtractNumberInObject(obj, "y", v)) ch.y = static_cast<float>(v);
        if (JsonExtractNumberInObject(obj, "width", v)) ch.width = static_cast<float>(v);
        if (JsonExtractNumberInObject(obj, "height", v)) ch.height = static_cast<float>(v);
        if (JsonExtractNumberInObject(obj, "xoffset", v)) ch.xoffset = static_cast<float>(v);
        if (JsonExtractNumberInObject(obj, "yoffset", v)) ch.yoffset = static_cast<float>(v);
        if (JsonExtractNumberInObject(obj, "xadvance", v)) ch.xadvance = static_cast<float>(v);

        if (hasId) outChars.push_back(ch);

        p = objEnd + 1;
        JsonSkipWs(arr, p);
        if (p < arr.size() && arr[p] == ',') { ++p; continue; }
        if (p < arr.size() && arr[p] == ']') { ++p; break; }
    }
    return !outChars.empty();
}

// Top-level JSON object parser: extracts info, common, pages, chars
inline std::expected<FontBMDescriptor, ErrorCode> ParseJsonDescriptor(std::string_view text) {
    size_t p = 0;
    JsonSkipWs(text, p);
    if (p >= text.size() || text[p] != '{') {
        return std::unexpected(FontBMError::Malformed);
    }
    size_t topStart = p;
    size_t topEnd = JsonFindMatching(text, topStart, '{', '}');
    if (topEnd == std::string_view::npos) {
        return std::unexpected(FontBMError::Malformed);
    }
    std::string_view top = text.substr(topStart, topEnd - topStart + 1);

    FontBMDescriptor desc;
    bool sawCommon = false;
    bool sawPage = false;

    // Iterate top-level keys
    size_t it = 1; // after '{'
    while (it < top.size()) {
        JsonSkipWs(top, it);
        if (it >= top.size() || top[it] == '}') break;
        if (top[it] != '"') { // skip malformed
            JsonSkipValue(top, it);
            JsonSkipWs(top, it);
            if (it < top.size() && top[it] == ',') { ++it; continue; }
            break;
        }
        std::string key = JsonParseString(top, it);
        JsonSkipWs(top, it);
        if (it >= top.size() || top[it] != ':') { JsonSkipValue(top, it); continue; }
        ++it;
        JsonSkipWs(top, it);
        size_t valStart = it;
        // Determine value type and capture substring
        size_t valEnd = valStart;
        if (it < top.size() && top[it] == '{') {
            size_t m = JsonFindMatching(top, it, '{', '}');
            if (m == std::string_view::npos) break;
            valEnd = m + 1;
            std::string_view val = top.substr(valStart, valEnd - valStart);

            if (key == "info") {
                double sz = 0;
                if (JsonExtractNumberInObject(val, "size", sz)) {
                    desc.fontSize = static_cast<float>(std::abs(sz));
                }
            } else if (key == "common") {
                sawCommon = true;
                double v = 0;
                if (JsonExtractNumberInObject(val, "lineHeight", v)) desc.lineHeight = static_cast<float>(v);
                if (JsonExtractNumberInObject(val, "base", v)) desc.baseline = static_cast<float>(v);
                if (JsonExtractNumberInObject(val, "scaleW", v)) desc.atlasWidth = static_cast<uint32_t>(v);
                if (JsonExtractNumberInObject(val, "scaleH", v)) desc.atlasHeight = static_cast<uint32_t>(v);
            }
            it = valEnd;
        } else if (it < top.size() && top[it] == '[') {
            size_t m = JsonFindMatching(top, it, '[', ']');
            if (m == std::string_view::npos) break;
            valEnd = m + 1;
            std::string_view val = top.substr(valStart, valEnd - valStart);
            if (key == "pages") {
                std::string pageFile;
                if (JsonParsePagesArray(val, pageFile)) {
                    desc.pageFile = pageFile;
                    sawPage = true;
                }
            } else if (key == "chars") {
                std::vector<FontBMChar> chars;
                if (JsonParseCharsArray(val, chars)) {
                    desc.chars = std::move(chars);
                }
            }
            it = valEnd;
        } else {
            // string / number / literal - skip
            JsonSkipValue(top, it);
            valEnd = it;
        }

        JsonSkipWs(top, it);
        if (it < top.size() && top[it] == ',') { ++it; continue; }
        if (it < top.size() && top[it] == '}') break;
    }

    if (!sawCommon || !sawPage || desc.pageFile.empty() || desc.chars.empty() || desc.atlasWidth == 0 || desc.atlasHeight == 0) {
        return std::unexpected(FontBMError::MissingMetrics);
    }
    return desc;
}

} // anonymous

auto ParseFontBMDescriptor(std::string_view text) -> std::expected<FontBMDescriptor, ErrorCode> {
    if (text.substr(0, 3) == "BMF") {
        return std::unexpected(FontBMError::UnsupportedFormat);
    }

    // Detect JSON: first non-whitespace char is '{'
    size_t p = 0;
    JsonSkipWs(text, p);
    if (p < text.size() && text[p] == '{') {
        auto jsonDesc = ParseJsonDescriptor(text);
        if (jsonDesc.has_value()) {
            return jsonDesc;
        }
        // If JSON parsing failed but text looks like JSON, return its error
        // (don't fall through to legacy text parser which would also fail).
        // However, if the error is MissingMetrics we still return it.
        return jsonDesc;
    }

    FontBMDescriptor desc;
    bool             sawCommon = false;
    bool             sawPage   = false;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t      eol  = text.find('\n', pos);
        const std::string_view line = text.substr(pos, (eol == std::string_view::npos) ? std::string_view::npos : (eol - pos));
        pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;

        std::string_view stripped = line;
        while (!stripped.empty() && ((stripped.back() == '\r') || (stripped.back() == ' ') || (stripped.back() == '\t'))) {
            stripped.remove_suffix(1);
        }
        while (!stripped.empty() && ((stripped.front() == ' ') || (stripped.front() == '\t'))) {
            stripped.remove_prefix(1);
        }
        if (stripped.empty()) {
            continue;
        }

        const size_t      wordEnd = stripped.find_first_of(" \t");
        const std::string_view word = stripped.substr(0, wordEnd);
        const std::string_view rest = (wordEnd == std::string_view::npos) ? std::string_view() : stripped.substr(wordEnd);

        if (word == "info") {
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if (tok.key == "size") {
                    desc.fontSize = std::abs(ParseFloat(tok.value, desc.fontSize));
                }
            });
        } else if (word == "common") {
            sawCommon = true;
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if (tok.key == "lineHeight") {
                    desc.lineHeight = ParseFloat(tok.value);
                } else if (tok.key == "base") {
                    desc.baseline = ParseFloat(tok.value);
                } else if (tok.key == "scaleW") {
                    desc.atlasWidth = ParseU32(tok.value);
                } else if (tok.key == "scaleH") {
                    desc.atlasHeight = ParseU32(tok.value);
                }
            });
        } else if (word == "page") {
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if ((tok.key == "id") && (ParseU32(tok.value) == 0)) {
                    sawPage = true;
                } else if (tok.key == "file") {
                    desc.pageFile = std::string(tok.value);
                    sawPage       = true;
                }
            });
        } else if (word == "char") {
            FontBMChar ch;
            bool       hasId = false;
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if (tok.key == "id") {
                    ch.id   = ParseU32(tok.value);
                    hasId = true;
                } else if (tok.key == "x") {
                    ch.x = ParseFloat(tok.value);
                } else if (tok.key == "y") {
                    ch.y = ParseFloat(tok.value);
                } else if (tok.key == "width") {
                    ch.width = ParseFloat(tok.value);
                } else if (tok.key == "height") {
                    ch.height = ParseFloat(tok.value);
                } else if (tok.key == "xoffset") {
                    ch.xoffset = ParseFloat(tok.value);
                } else if (tok.key == "yoffset") {
                    ch.yoffset = ParseFloat(tok.value);
                } else if (tok.key == "xadvance") {
                    ch.xadvance = ParseFloat(tok.value);
                }
            });
            if (hasId) {
                desc.chars.push_back(ch);
            }
        }
        // `chars`, `kerning(s)` counts and unknown lines carry nothing the
        // runtime needs; the records themselves matter.
    }

    if (!sawCommon || !sawPage || desc.pageFile.empty() || desc.chars.empty() || (desc.atlasWidth == 0) || (desc.atlasHeight == 0)) {
        return std::unexpected(FontBMError::MissingMetrics);
    }
    return desc;
}

auto AssembleBakedFont(const FontBMDescriptor& desc, std::span<const uint8_t> rgba8)
    -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const size_t texels = static_cast<size_t>(desc.atlasWidth) * desc.atlasHeight;
    if (rgba8.size() != texels * 4) {
        return std::unexpected(FontBMError::BadPage);
    }
    if (desc.chars.empty() || (desc.atlasWidth == 0) || (desc.atlasHeight == 0)) {
        return std::unexpected(FontBMError::MissingMetrics);
    }

    // fontbm writes white glyphs on a transparent page: alpha is the coverage.
    // An opaque page (black on white) falls back to inverted luma.
    bool anyAlpha = false;
    for (size_t i = 0; i < texels; ++i) {
        if (rgba8[i * 4 + 3] != 0) {
            anyAlpha = true;
            break;
        }
    }

    uint32_t firstCodepoint = desc.chars.front().id;
    uint32_t lastCodepoint  = desc.chars.front().id;
    for (const auto& ch: desc.chars) {
        firstCodepoint = std::min(firstCodepoint, ch.id);
        lastCodepoint  = std::max(lastCodepoint, ch.id);
    }
    const uint64_t span = static_cast<uint64_t>(lastCodepoint) - firstCodepoint + 1;
    if (span > 4096) {
        return std::unexpected(FontBMError::Malformed);
    }

    // Advance used by codepoints the bake skipped inside its own range: the
    // space glyph's when there is one, so gaps keep the pen moving.
    float gapAdvance = desc.fontSize * 0.5f;
    for (const auto& ch: desc.chars) {
        if (ch.id == static_cast<uint32_t>(' ')) {
            gapAdvance = ch.xadvance;
            break;
        }
    }

    GUI::BakedFontAsset asset;
    asset.atlasWidth     = desc.atlasWidth;
    asset.atlasHeight    = desc.atlasHeight;
    asset.firstCodepoint = firstCodepoint;
    asset.fontSize       = desc.fontSize;
    asset.baseline       = desc.baseline;
    asset.lineHeight     = desc.lineHeight;
    asset.isSDF          = false; // fontbm bakes plain bitmap coverage, not a distance field
    asset.coverage.assign(texels, 0);
    asset.glyphs.resize(static_cast<size_t>(span));

    for (uint64_t i = 0; i < span; ++i) {
        asset.glyphs[i].xadvance = gapAdvance;
    }

    for (const auto& ch: desc.chars) {
        const uint32_t x0 = static_cast<uint32_t>(ch.x);
        const uint32_t y0 = static_cast<uint32_t>(ch.y);
        const uint32_t w  = static_cast<uint32_t>(ch.width);
        const uint32_t h  = static_cast<uint32_t>(ch.height);
        if ((x0 + w > desc.atlasWidth) || (y0 + h > desc.atlasHeight)) {
            return std::unexpected(FontBMError::BadPage);
        }

        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const size_t texel   = static_cast<size_t>(y0 + row) * desc.atlasWidth + (x0 + col);
                const size_t channel = texel * 4;
                const uint8_t coverage = anyAlpha
                                             ? rgba8[channel + 3]
                                             : static_cast<uint8_t>(255 - ((rgba8[channel] + rgba8[channel + 1] + rgba8[channel + 2]) / 3));
                asset.coverage[texel] = coverage;
            }
        }

        // BMFont measures glyph tops from the line-box top; the engine
        // measures from the baseline (see Fonts.hpp).
        GlyphMetric metric {};
        metric.x0       = ch.x;
        metric.y0       = ch.y;
        metric.x1       = ch.x + ch.width;
        metric.y1       = ch.y + ch.height;
        metric.xoff     = ch.xoffset;
        metric.yoff     = ch.yoffset - desc.baseline;
        metric.xadvance = ch.xadvance;
        asset.glyphs[ch.id - firstCodepoint] = metric;
    }

    return asset;
}

// --- Install (the composition root calls this) -------------------------------

void InstallBakedFontLoader(CreativeWorksManager& assets, const BakedFontSource& source) {
    g_instance.assets    = &assets;
    g_instance.source    = source;
    g_instance.cache     = GUI::BakedFontAsset {};
    g_instance.attempted = false;

    GUI::InstallBakedFontLoader(&LoaderFn, &g_instance);
}

void InstallBakedFontLoader(Engine& engine, const BakedFontSource& source) {
    InstallBakedFontLoader(engine.GetCreativeWorksManager(), source);
    // The hook outlives the engine it was installed for unless told otherwise.
    // Drop the association on teardown so nothing serves through a dead
    // manager; a later Install resolves fresh.
    engine.AddTeardownHook(+[](Engine& e) noexcept -> void {
        (void)e;
        g_instance.assets    = nullptr;
        g_instance.attempted = false;
        g_instance.cache     = GUI::BakedFontAsset {};
        GUI::UninstallBakedFontLoader();
    });
}

} // namespace ZHLN::Fonts
