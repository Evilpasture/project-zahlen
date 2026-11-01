// JSON: the metadata documents inside an archive, and the remote index.
//
// The bash version of zpkg parsed these with sed against a fixed single-line
// layout, which worked only because one CMake function writes them. That is a
// parser that fails silently on a reordered key, so the C++ version uses
// simdjson -- already a project dependency -- and fails loudly instead.
//
// "Loudly" without exceptions means every helper that can reject a document
// returns std::expected, and the parse functions thread those results through in
// order. The nesting is shallow (a document, a toolchain, a package), so the
// propagation reads as a checklist rather than as plumbing.
#include "zpkg/zpkg.hpp"
#include <cstdio>
#include <simdjson.h>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN {

namespace {

using simdjson::dom::element;
using simdjson::dom::object;

[[nodiscard]] auto Bad(std::string_view origin, std::string_view what) -> std::unexpected<Fail> {
    return std::unexpected(Fail {std::string(origin) + ": " + std::string(what)});
}

auto RequireString(element parent, const char* key, std::string_view origin) -> std::expected<std::string_view, Fail> {
    std::string_view value;
    const auto       error = parent[key].get_string().get(value);
    if (error != simdjson::SUCCESS) {
        return Bad(origin, std::string("missing or non-string \"") + key + "\"");
    }
    return value;
}

std::string StringOr(element parent, const char* key, std::string fallback) {
    std::string_view value;
    if (parent[key].get_string().get(value) != simdjson::SUCCESS)
        return fallback;
    return std::string(value);
}

auto StringArray(element parent, const char* key, std::string_view origin, bool required) -> std::expected<std::vector<std::string>, Fail> {
    std::vector<std::string> out;
    simdjson::dom::array     array;
    const auto               error = parent[key].get_array().get(array);
    if (error != simdjson::SUCCESS) {
        if (required)
            return Bad(origin, std::string("missing or non-array \"") + key + "\"");
        return out;
    }
    for (auto value: array) {
        std::string_view text;
        if (value.get_string().get(text) != simdjson::SUCCESS) {
            return Bad(origin, std::string("\"") + key + "\" must contain only strings");
        }
        out.emplace_back(text);
    }
    return out;
}

auto RequireFormat(element parent, std::string_view origin) -> std::expected<int, Fail> {
    int64_t    format = 0;
    const auto error  = parent["format"].get_int64().get(format);
    if (error != simdjson::SUCCESS)
        return Bad(origin, "missing or non-numeric \"format\"");
    if (format != PackageFormat) {
        return Bad(
            origin,
            "format " + std::to_string(format) + " is not supported by zpkg " + std::string(ToolVersion) + " (expects " + std::to_string(PackageFormat) + ")"
        );
    }
    return static_cast<int>(format);
}

auto ParseDocument(simdjson::dom::parser& parser, std::string_view json, std::string_view origin) -> std::expected<element, Fail> {
    element    document;
    const auto error = parser.parse(json).get(document);
    if (error != simdjson::SUCCESS) {
        return Bad(origin, std::string("is not valid JSON: ") + simdjson::error_message(error));
    }
    if (document.type() != simdjson::dom::element_type::OBJECT) {
        return Bad(origin, "must contain a JSON object");
    }
    return document;
}

std::string Escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (char c: text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace

auto ParseToolchainJson(std::string_view json, std::string_view origin) -> std::expected<ToolchainInfo, Fail> {
    simdjson::dom::parser parser;
    auto                  document = ParseDocument(parser, json, origin);
    if (!document)
        return std::unexpected(std::move(document.error()));

    auto format = RequireFormat(*document, origin);
    if (!format)
        return std::unexpected(std::move(format.error()));
    auto id = RequireString(*document, "id", origin);
    if (!id)
        return std::unexpected(std::move(id.error()));
    if (id->empty())
        return Bad(origin, "has an empty \"id\"");

    ToolchainInfo info;
    info.format          = *format;
    info.id              = std::string(*id);
    info.version         = StringOr(*document, "version", "");
    info.commit          = StringOr(*document, "commit", "");
    info.compiler        = StringOr(*document, "compiler", "");
    info.compilerVersion = StringOr(*document, "compiler_version", "");
    info.processor       = StringOr(*document, "processor", "");
    info.profile         = StringOr(*document, "profile", "");
    info.abi             = StringOr(*document, "abi", "");
    return info;
}

auto ParseManifestJson(std::string_view json, std::string_view origin) -> std::expected<PackageInfo, Fail> {
    simdjson::dom::parser parser;
    auto                  document = ParseDocument(parser, json, origin);
    if (!document)
        return std::unexpected(std::move(document.error()));

    auto format = RequireFormat(*document, origin);
    if (!format)
        return std::unexpected(std::move(format.error()));
    auto package = RequireString(*document, "package", origin);
    if (!package)
        return std::unexpected(std::move(package.error()));
    auto component = RequireString(*document, "component", origin);
    if (!component)
        return std::unexpected(std::move(component.error()));
    auto requiredNames = StringArray(*document, "requires", origin, true);
    if (!requiredNames)
        return std::unexpected(std::move(requiredNames.error()));
    auto provides = StringArray(*document, "provides", origin, false);
    if (!provides)
        return std::unexpected(std::move(provides.error()));
    if (package->empty())
        return Bad(origin, "has an empty \"package\"");

    PackageInfo info;
    info.format       = *format;
    info.package      = std::string(*package);
    info.component    = std::string(*component);
    info.toolchain    = StringOr(*document, "toolchain", "");
    info.profile      = StringOr(*document, "profile", "");
    info.dependencies = std::move(*requiredNames);
    info.provides     = std::move(*provides);
    return info;
}

auto ParseIndexJson(std::string_view json, std::string_view origin) -> std::expected<Index, Fail> {
    simdjson::dom::parser parser;
    auto                  document = ParseDocument(parser, json, origin);
    if (!document)
        return std::unexpected(std::move(document.error()));

    auto format = RequireFormat(*document, origin);
    if (!format)
        return std::unexpected(std::move(format.error()));

    simdjson::dom::array toolchains;
    if ((*document)["toolchains"].get_array().get(toolchains) != simdjson::SUCCESS) {
        return Bad(origin, "missing or non-array \"toolchains\"");
    }

    Index index;
    index.format = *format;
    for (auto entry: toolchains) {
        auto id = RequireString(entry, "id", origin);
        if (!id)
            return std::unexpected(std::move(id.error()));

        IndexToolchain toolchain;
        toolchain.id      = std::string(*id);
        toolchain.version = StringOr(entry, "version", "");
        toolchain.profile = StringOr(entry, "profile", "");

        simdjson::dom::array packages;
        if (entry["packages"].get_array().get(packages) != simdjson::SUCCESS) {
            return Bad(origin, "toolchain '" + toolchain.id + "' has no \"packages\" array");
        }
        for (auto item: packages) {
            auto name = RequireString(item, "package", origin);
            if (!name)
                return std::unexpected(std::move(name.error()));
            auto file = RequireString(item, "file", origin);
            if (!file)
                return std::unexpected(std::move(file.error()));
            auto sha = RequireString(item, "sha256", origin);
            if (!sha)
                return std::unexpected(std::move(sha.error()));

            IndexPackage package;
            package.package   = std::string(*name);
            package.component = StringOr(item, "component", "");
            package.file      = std::string(*file);
            package.sha256    = std::string(*sha);
            int64_t size      = 0;
            if (item["size"].get_int64().get(size) == simdjson::SUCCESS) {
                package.size = static_cast<uint64_t>(size);
            }
            toolchain.packages.push_back(std::move(package));
        }
        index.toolchains.push_back(std::move(toolchain));
    }
    return index;
}

std::string BuildIndexJson(const std::vector<IndexToolchain>& toolchains) {
    std::ostringstream out;
    out << "{\"format\":" << PackageFormat << ",\"toolchains\":[";
    bool firstToolchain = true;
    for (const auto& toolchain: toolchains) {
        if (!firstToolchain)
            out << ',';
        firstToolchain = false;
        out << "\n  {\"id\":\"" << Escape(toolchain.id) << "\",\"version\":\"" << Escape(toolchain.version) << "\",\"profile\":\"" << Escape(toolchain.profile)
            << "\",\"packages\":[";
        bool firstPackage = true;
        for (const auto& package: toolchain.packages) {
            if (!firstPackage)
                out << ',';
            firstPackage = false;
            out << "\n    {\"package\":\"" << Escape(package.package) << "\",\"component\":\"" << Escape(package.component) << "\",\"file\":\""
                << Escape(package.file) << "\",\"sha256\":\"" << Escape(package.sha256) << "\",\"size\":" << package.size << '}';
        }
        out << "\n  ]}";
    }
    out << "\n]}\n";
    return out.str();
}

} // namespace ZHLN
