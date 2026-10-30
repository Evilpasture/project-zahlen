// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Host-owned reflection binding. The tokenizer in zahlen_command_line knows
// nothing about the engine; only this application/host-runner layer selects
// EngineConfig as a schema and handles process commands.
#include <CommandLine/CommandLine.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// Optional member annotations shared by EngineConfig and game launch configs.
template <auto Min, auto Max>
struct CliRange {
    static_assert(
        std::is_arithmetic_v<decltype(Min)> && std::is_arithmetic_v<decltype(Max)> &&
        !std::same_as<std::remove_cv_t<decltype(Min)>, bool> &&
        !std::same_as<std::remove_cv_t<decltype(Max)>, bool>,
        "CliRange bounds must be numeric (not bool)"
    );
    static_assert([] {
        if constexpr (std::integral<decltype(Min)> && std::integral<decltype(Max)> &&
                      !std::same_as<std::remove_cv_t<decltype(Min)>, bool> &&
                      !std::same_as<std::remove_cv_t<decltype(Max)>, bool>) {
            return std::cmp_less_equal(Min, Max);
        } else {
            return static_cast<long double>(Min) <= static_cast<long double>(Max);
        }
    }(), "CliRange minimum must not exceed maximum");
};

template <char Key>
struct CliShort {
    static_assert((Key >= 'a' && Key <= 'z') || (Key >= 'A' && Key <= 'Z'), "CliShort requires a letter");
};

namespace TemplatedDetail {
template <typename T>
inline constexpr bool IsCliFixedString = false;
template <size_t N>
inline constexpr bool IsCliFixedString<FixedString<N>> = true;
template <typename T>
inline constexpr bool UnsupportedCliField = false;
}

// A copyable, owned flag schema. Only actions borrow the objects bound by
// WithConfig; the generated keys, aliases and enum choices survive copies.
class ConfigCli {
  public:
    auto Add(CliFlag flag) -> ConfigCli& {
        for (const CliFlag& existing: _flags) {
            if (existing.key == flag.key || (!flag.shortKey.empty() && (existing.key == flag.shortKey || existing.shortKey == flag.shortKey)) ||
                (!existing.shortKey.empty() && existing.shortKey == flag.key)) {
                _duplicate = true;
            }
        }
        _flags.push_back(std::move(flag));
        return *this;
    }

    auto Extend(std::span<const CliFlag> flags) -> ConfigCli& {
        for (const CliFlag& flag: flags) Add(flag);
        return *this;
    }

    [[nodiscard]] auto Flags() const noexcept -> std::span<const CliFlag> { return _flags; }
    [[nodiscard]] auto Parse(std::span<char* const> args, const CliParseConfig& options = {}) const
        -> std::expected<CliParseResult, CommandLineError> {
        if (_duplicate) return std::unexpected(CommandLineError::DuplicateFlag);
        return ParseCli(args, _flags, options);
    }

    // Ordinary scalar fields bind automatically; nested aggregates use a
    // kebab-case prefix (physics.maxBodies -> --physics-max-bodies). The sole
    // programmatic-only EngineConfig field is its non-owning pipeline byte span.
    template <typename T>
        requires(!std::is_const_v<T> && std::is_class_v<T>)
    auto WithConfig(T& target) -> ConfigCli& {
        Bind(target, "--");
        return *this;
    }

  private:
    static auto Kebab(std::string_view name) -> std::string {
        std::string key;
        for (size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            if (c == '_') {
                if (key.empty() || key.back() != '-') key.push_back('-');
            } else if (c >= 'A' && c <= 'Z') {
                const char prev = i > 0 ? name[i - 1] : '\0';
                const char next = i + 1 < name.size() ? name[i + 1] : '\0';
                const bool boundary = (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9') ||
                                      (prev >= 'A' && prev <= 'Z' && next >= 'a' && next <= 'z');
                if (boundary && !key.empty() && key.back() != '-') key.push_back('-');
                key.push_back(static_cast<char>(c - 'A' + 'a'));
            } else {
                key.push_back(c);
            }
        }
        return key;
    }

    template <typename T>
    void Bind(T& target, std::string prefix) {
        Reflect::ForEachDataMember<T>([&]<auto member>() {
            using Field = std::remove_cvref_t<decltype(Reflect::MemberValue<member>(target))>;
            constexpr std::string_view name = Reflect::MemberName<member>();
            static_assert(!name.empty(), "Reflected CLI fields must be named");
            constexpr bool supported = std::integral<Field> || std::floating_point<Field> ||
                                       std::same_as<Field, std::string> || TemplatedDetail::IsCliFixedString<Field> ||
                                       std::is_enum_v<Field>;
            if constexpr (supported) {
                constexpr size_t rangeCount = Reflect::AnnotationCountOf<CliRange, member>();
                static_assert(rangeCount <= 1, "Only one CliRange annotation is allowed per field");
                static_assert(
                    rangeCount == 0 || ((std::integral<Field> && !std::same_as<Field, bool>) || std::floating_point<Field>),
                    "CliRange applies only to numeric config fields"
                );
                static_assert(Reflect::AnnotationCountOf<CliShort, member>() <= 1, "Only one CliShort annotation is allowed per field");
                constexpr std::string_view description = Reflect::GetDescriptionText<member>();
                auto key = std::make_shared<const std::string>(prefix + Kebab(name));
                std::shared_ptr<const std::string> shortKey;
                if constexpr (Reflect::AnnotationCountOf<CliShort, member>() > 0) {
                    constexpr char letter = Reflect::AnnotationTemplateArgument<CliShort, member, 0, char>();
                    shortKey = std::make_shared<const std::string>(std::string("-") + letter);
                }
                std::string_view placeholder;
                std::shared_ptr<const std::string> enumChoices;
                if constexpr (std::same_as<Field, bool>) {
                    placeholder = "[on|off|true|false]";
                } else if constexpr (std::integral<Field>) {
                    placeholder = "<integer>";
                } else if constexpr (std::floating_point<Field>) {
                    placeholder = "<number>";
                } else if constexpr (std::same_as<Field, std::string> || TemplatedDetail::IsCliFixedString<Field>) {
                    placeholder = "<text>";
                } else if constexpr (std::is_enum_v<Field>) {
                    constexpr auto names = Reflect::EnumNames<Field>();
                    static_assert(!names.empty(), "A reflected CLI enum must have at least one enumerator");
                    std::string choices = "<";
                    for (std::string_view choice: names) {
                        if (choices.size() > 1) choices.push_back('|');
                        choices.append(choice);
                    }
                    choices.push_back('>');
                    enumChoices = std::make_shared<const std::string>(std::move(choices));
                    placeholder = *enumChoices;
                }
                auto* field = &Reflect::MemberValue<member>(target);
                Add(CliFlag {
                    .key         = *key,
                    .shortKey    = shortKey ? std::string_view(*shortKey) : std::string_view {},
                    .placeholder = placeholder,
                    .description = description.empty() ? std::string_view(*key).substr(2) : description,
                    .action      = [field](std::string_view value) -> std::expected<void, CommandLineError> {
                        Field parsed {};
                        if constexpr (std::same_as<Field, bool>) {
                            if (value.empty() || value == "on" || value == "true" || value == "1" || value == "yes") parsed = true;
                            else if (value == "off" || value == "false" || value == "0" || value == "no") parsed = false;
                            else return std::unexpected(CommandLineError::InvalidValue);
                        } else {
                            if (value.empty()) return std::unexpected(CommandLineError::MissingValue);
                            if constexpr (std::integral<Field>) {
                                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                                if (error != std::errc {} || end != value.data() + value.size())
                                    return std::unexpected(CommandLineError::InvalidValue);
                            } else if constexpr (std::floating_point<Field>) {
                                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, std::chars_format::general);
                                if (error != std::errc {} || end != value.data() + value.size() || !std::isfinite(parsed))
                                    return std::unexpected(CommandLineError::InvalidValue);
                            } else if constexpr (std::same_as<Field, std::string>) {
                                parsed.assign(value);
                            } else if constexpr (TemplatedDetail::IsCliFixedString<Field>) {
                                if (value.size() > Field::kMaxTextLength) return std::unexpected(CommandLineError::InvalidValue);
                                parsed.assign(value); // never silently truncate names or paths
                            } else if constexpr (std::is_enum_v<Field>) {
                                auto selected = Reflect::StringToEnum<Field>(value);
                                if (!selected) return std::unexpected(CommandLineError::InvalidValue);
                                parsed = *selected;
                            }
                        }
                        if constexpr (rangeCount > 0) {
                            constexpr long double min = Reflect::AnnotationTemplateArgument<CliRange, member, 0, long double>();
                            constexpr long double max = Reflect::AnnotationTemplateArgument<CliRange, member, 1, long double>();
                            const long double number = static_cast<long double>(parsed);
                            if (number < min || number > max) return std::unexpected(CommandLineError::InvalidValue);
                        }
                        *field = std::move(parsed); // commit only after validation
                        return {};
                    },
                });
                _ownedText.push_back(std::move(key));
                if (shortKey) _ownedText.push_back(std::move(shortKey));
                if (enumChoices) _ownedText.push_back(std::move(enumChoices));
            } else if constexpr (std::same_as<Field, std::span<const uint8_t>>) {
                // Binary data has no textual CLI representation; use C++ config.
            } else if constexpr (std::is_aggregate_v<Field> && std::is_class_v<Field>) {
                Bind(Reflect::MemberValue<member>(target), prefix + Kebab(name) + "-");
            } else {
                static_assert(TemplatedDetail::UnsupportedCliField<Field>, "CLI field needs a scalar, reflected enum or nested aggregate");
            }
        });
    }

    std::vector<CliFlag> _flags;
    std::vector<std::shared_ptr<const std::string>> _ownedText;
    bool _duplicate = false;
};

struct EngineCliResult {
    bool helpRequested = false;
    bool versionRequested = false;
    bool graphRequested = false;
};

// One host-side binding for apps, Kernel tools and bespoke samples. Extra
// flags are game-specific; duplicate keys (including reflected keys) fail.
[[nodiscard]] auto ParseEngineConfig(
    std::span<char* const> args, EngineConfig& config, std::span<const CliFlag> extraFlags = {},
    CliParseConfig parseConfig = {}
) -> std::expected<EngineCliResult, ErrorCode>;

} // namespace ZHLN
