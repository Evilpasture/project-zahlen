// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Application facade: unlike Core's Engine.hpp, this host-facing header owns
// its CLI contract. zahlen_application publishes zahlen_engine_cli to callers.
#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/AppOutcome.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/String.hpp>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// Application-layer preset. Installs the available gameplay extensions before
// the first scene; Core does not depend on optional gameplay targets.
void InstallApplicationDefaults(Engine& engine);

// A layer may implement any of Start, Tick, UI and Stop. Start runs after
// scene creation; Tick runs after event/input pumping; UI runs in the Engine's
// UI frame phase. These run in registration order, and Stop runs in reverse
// order before Engine teardown. Layers can own move-only state.
template <typename T>
concept AppLayer = requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.Start(engine);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine, float dt) {
    layer.Tick(engine, dt);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.UI(engine);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.Stop(engine);
};

// Bounds for integral or floating-point CLI configuration fields. The
// ZHLN_ANNOTATION syntax attaches this to a reflected member.
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

// Optional single-letter short alias for a reflected field (e.g. -c).
template <char Key>
struct CliShort {
    static_assert((Key >= 'a' && Key <= 'z') || (Key >= 'A' && Key <= 'Z'), "CliShort requires a letter");
};

namespace TemplatedDetail {
template <typename T>
inline constexpr bool IsAppConfigFixedString = false;
template <size_t N>
inline constexpr bool IsAppConfigFixedString<FixedString<N>> = true;
} // namespace TemplatedDetail

class Application {
  public:
    explicit Application(EngineRunConfig config = {}): _config(std::move(config)) {}

    // Ordinary games get the installed extensions automatically. Custom hosts
    // may opt out, or add modules before InitializeDefaultScene builds graphs.
    auto WithDefaultModules(bool enabled = true) noexcept -> Application& {
        _defaultModules = enabled;
        return *this;
    }

    auto Install(std::function<void(Engine&)> installer) -> Application& {
        if (installer) {
            _installers.push_back(std::move(installer));
        }
        return *this;
    }

    // Add game-specific flags to the same parse and --help menu as engine flags.
    // Actions run during parsing and may capture game setup state; the handler's
    // string_views must remain valid until Run returns.
    auto AddCliOption(CommandHandler handler) -> Application& {
        _cliHandlers.push_back(std::move(handler));
        return *this;
    }

    auto WithCliOptions(std::span<const CommandHandler> handlers) -> Application& {
        _cliHandlers.insert(_cliHandlers.end(), handlers.begin(), handlers.end());
        return *this;
    }

    auto AddCliOption(
        std::string_view key,
        std::string_view placeholder,
        std::string_view description,
        std::function<std::expected<void, ErrorCode>(CommandLineOptions&, std::string_view)> action,
        std::string_view shortKey = ""
    ) -> Application& {
        return AddCliOption(CommandHandler {
            .key         = key,
            .shortKey    = shortKey,
            .placeholder = placeholder,
            .description = description,
            .action      = std::move(action),
        });
    }

    // Bind reflected config fields to --kebab-case-member-name. Text for keys,
    // short aliases and enum choices is owned here; the target config is borrowed
    // and must outlive synchronous Run. Each field is committed only when valid.
    template <typename ConfigStruct>
        requires(!std::is_const_v<ConfigStruct> && std::is_class_v<ConfigStruct>)
    auto WithConfig(ConfigStruct& target) -> Application& {
        Reflect::ForEachDataMember<ConfigStruct>([&]<auto member>() {
            using Field = std::remove_cvref_t<decltype(Reflect::MemberValue<member>(target))>;
            static_assert(
                (std::integral<Field> && !std::same_as<Field, bool>) || std::same_as<Field, bool> ||
                std::floating_point<Field> || std::same_as<Field, std::string> ||
                TemplatedDetail::IsAppConfigFixedString<Field> || std::is_enum_v<Field>,
                "WithConfig supports bool, numbers, std::string, FixedString<N> and reflected enums"
            );
            constexpr size_t rangeCount = Reflect::AnnotationCountOf<CliRange, member>();
            static_assert(rangeCount <= 1, "Only one CliRange annotation is allowed per field");
            static_assert(
                rangeCount == 0 || ((std::integral<Field> && !std::same_as<Field, bool>) || std::floating_point<Field>),
                "CliRange applies only to numeric config fields"
            );
            static_assert(Reflect::AnnotationCountOf<CliShort, member>() <= 1, "Only one CliShort annotation is allowed per field");
            constexpr std::string_view name = Reflect::MemberName<member>();
            static_assert(!name.empty(), "Reflected CLI fields must be named");
            constexpr std::string_view description = Reflect::GetDescriptionText<member>();

            std::string keyText = "--";
            for (size_t i = 0; i < name.size(); ++i) {
                const char c = name[i];
                if (c == '_') {
                    if (keyText.back() != '-') {
                        keyText.push_back('-');
                    }
                } else if (c >= 'A' && c <= 'Z') {
                    const char prev = i > 0 ? name[i - 1] : '\0';
                    const char next = i + 1 < name.size() ? name[i + 1] : '\0';
                    const bool boundary = (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9') ||
                                          (prev >= 'A' && prev <= 'Z' && next >= 'a' && next <= 'z');
                    if (boundary && keyText.back() != '-') {
                        keyText.push_back('-');
                    }
                    keyText.push_back(static_cast<char>(c - 'A' + 'a'));
                } else {
                    keyText.push_back(c);
                }
            }
            auto key = std::make_shared<const std::string>(std::move(keyText));
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
            } else if constexpr (std::same_as<Field, std::string> || TemplatedDetail::IsAppConfigFixedString<Field>) {
                placeholder = "<text>";
            } else if constexpr (std::is_enum_v<Field>) {
                constexpr auto names = Reflect::EnumNames<Field>();
                static_assert(!names.empty(), "A reflected CLI enum must have at least one enumerator");
                std::string choices = "<";
                for (std::string_view choice: names) {
                    if (choices.size() > 1) {
                        choices.push_back('|');
                    }
                    choices.append(choice);
                }
                choices.push_back('>');
                enumChoices = std::make_shared<const std::string>(std::move(choices));
                placeholder = *enumChoices;
            }
            AddCliOption(CommandHandler {
                .key              = *key,
                .shortKey         = shortKey ? std::string_view(*shortKey) : std::string_view {},
                .placeholder      = placeholder,
                .description      = description.empty() ? std::string_view(*key).substr(2) : description,
                .action           = [&target](CommandLineOptions&, std::string_view value) -> std::expected<void, ErrorCode> {
                    Field parsed {};
                    if constexpr (std::same_as<Field, bool>) {
                        if (value.empty() || value == "on" || value == "true" || value == "1" || value == "yes") {
                            parsed = true;
                        } else if (value == "off" || value == "false" || value == "0" || value == "no") {
                            parsed = false;
                        } else {
                            return std::unexpected(CommandLineError::InvalidValue);
                        }
                    } else {
                        if (value.empty()) {
                            return std::unexpected(CommandLineError::MissingValue);
                        }
                        if constexpr (std::integral<Field>) {
                            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                            if (error != std::errc {} || end != value.data() + value.size()) {
                                return std::unexpected(CommandLineError::InvalidValue);
                            }
                        } else if constexpr (std::floating_point<Field>) {
                            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, std::chars_format::general);
                            if (error != std::errc {} || end != value.data() + value.size() || !std::isfinite(parsed)) {
                                return std::unexpected(CommandLineError::InvalidValue);
                            }
                        } else if constexpr (std::same_as<Field, std::string>) {
                            parsed.assign(value);
                        } else if constexpr (TemplatedDetail::IsAppConfigFixedString<Field>) {
                            if (value.size() > Field::kMaxTextLength) {
                                return std::unexpected(CommandLineError::InvalidValue); // never truncate a CLI path/name
                            }
                            parsed.assign(value);
                        } else if constexpr (std::is_enum_v<Field>) {
                            auto selected = Reflect::StringToEnum<Field>(value);
                            if (!selected) {
                                return std::unexpected(CommandLineError::InvalidValue);
                            }
                            parsed = *selected;
                        }
                    }
                    if constexpr (rangeCount > 0) {
                        constexpr long double minimum = Reflect::AnnotationTemplateArgument<CliRange, member, 0, long double>();
                        constexpr long double maximum = Reflect::AnnotationTemplateArgument<CliRange, member, 1, long double>();
                        const long double number = static_cast<long double>(parsed);
                        if (number < minimum || number > maximum) {
                            return std::unexpected(CommandLineError::InvalidValue);
                        }
                    }
                    Reflect::MemberValue<member>(target) = std::move(parsed); // commit only after validation
                    return {};
                },
                .mirrorEngineFlag = true, // built-in flags also update matching reflected fields
            });
            _reflectedOptionText.push_back(std::move(key));
            if (shortKey) {
                _reflectedOptionText.push_back(std::move(shortKey));
            }
            if (enumChoices) {
                _reflectedOptionText.push_back(std::move(enumChoices));
            }
        });
        return *this;
    }

    template <AppLayer Layer>
        requires std::constructible_from<std::remove_cvref_t<Layer>, Layer>
    auto AddLayer(Layer&& layer) -> Application& {
        using Stored = std::remove_cvref_t<Layer>;
        LayerSlot slot;
        slot.state = std::make_shared<Stored>(std::forward<Layer>(layer));
        if constexpr (requires(Stored& l, Engine& engine) { l.Start(engine); }) {
            slot.start = [](void* state, Engine& engine) { static_cast<Stored*>(state)->Start(engine); };
        }
        if constexpr (requires(Stored& l, Engine& engine, float dt) { l.Tick(engine, dt); }) {
            slot.tick = [](void* state, Engine& engine, float dt) { static_cast<Stored*>(state)->Tick(engine, dt); };
        }
        if constexpr (requires(Stored& l, Engine& engine) { l.UI(engine); }) {
            slot.ui = [](void* state, Engine& engine) { static_cast<Stored*>(state)->UI(engine); };
        }
        if constexpr (requires(Stored& l, Engine& engine) { l.Stop(engine); }) {
            slot.stop = [](void* state, Engine& engine) { static_cast<Stored*>(state)->Stop(engine); };
        }
        _layers.push_back(std::move(slot));
        return *this;
    }

    // Application lifecycle, not launch configuration. Called once after the
    // initial scene is built (e.g. to spawn persistent entities).
    auto OnStart(std::function<void(Engine&)> start) -> Application& {
        _start = std::move(start);
        return *this;
    }

    // System-driven apps need no per-frame callable. With no CLI parse there is
    // no CommandLineOptions service; a span-based Run publishes parsed options.
    [[nodiscard]] auto Run() const -> AppRunResult { return RunConfigured(_config, {}, nullptr); }
    [[nodiscard]] auto Run(std::span<char* const> args) const -> AppRunResult { return RunFromCLI(_config, args, {}); }

    // Adapter for main(int argc, char* argv[]). Reject malformed C arguments
    // instead of silently turning them into a valid empty command line.
    [[nodiscard]] auto Run(int argc, char* argv[]) const -> AppRunResult {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return std::unexpected(AppError::InvalidArguments);
        }
        return Run(MakeSpan(argc, argv));
    }

    // Gameplay runs once per frame after events/resize and before Engine::Tick.
    // Both (Engine&) and (Engine&, float dt) work. The callable is borrowed
    // synchronously until Run returns; even move-only state can be captured.
    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    [[nodiscard]] auto Run(GameLogic&& logic) const -> AppRunResult {
        return RunConfigured(_config, BindLogic(logic), nullptr);
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    [[nodiscard]] auto Run(std::span<char* const> args, GameLogic&& logic) const -> AppRunResult {
        return RunFromCLI(_config, args, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    [[nodiscard]] auto Run(int argc, char* argv[], GameLogic&& logic) const -> AppRunResult {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return std::unexpected(AppError::InvalidArguments);
        }
        return Run(MakeSpan(argc, argv), std::forward<GameLogic>(logic));
    }

  private:
    struct LayerSlot {
        // Shared ownership keeps the existing copyable Application API and
        // leaves no virtual dispatch in any of the four lifecycle phases.
        std::shared_ptr<void> state;
        void (*start)(void*, Engine&)       = nullptr;
        void (*tick)(void*, Engine&, float) = nullptr;
        void (*ui)(void*, Engine&)          = nullptr;
        void (*stop)(void*, Engine&)        = nullptr;
    };

    using Update = std::function<void(Engine&, float)>;

    [[nodiscard]] static constexpr auto MakeSpan(int argc, char* argv[]) noexcept -> std::span<char* const> {
        return argc > 0 ? std::span<char* const>(argv, static_cast<size_t>(argc)) : std::span<char* const> {};
    }

    template <typename GameLogic>
    static auto BindLogic(GameLogic& logic) -> Update {
        if constexpr (std::invocable<GameLogic&, Engine&, float>) {
            return [&logic](Engine& engine, float dt) { std::invoke(logic, engine, dt); };
        } else {
            return [&logic](Engine& engine, float) { std::invoke(logic, engine); };
        }
    }

    auto RunConfigured(const EngineRunConfig& config, const Update& update, const CommandLineOptions* options) const -> AppRunResult;
    auto RunFromCLI(EngineRunConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult;

    EngineRunConfig                           _config;
    std::function<void(Engine&)>              _start;
    std::vector<std::function<void(Engine&)>> _installers;
    std::vector<CommandHandler>               _cliHandlers;
    std::vector<std::shared_ptr<const std::string>> _reflectedOptionText;
    std::vector<LayerSlot>                    _layers;
    bool                                      _defaultModules = true;
};

[[nodiscard]] inline auto App(EngineRunConfig config = {}) -> Application {
    return Application(std::move(config));
}

} // namespace ZHLN
