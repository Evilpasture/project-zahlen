// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Application facade: unlike Core's Engine.hpp, this host-facing header owns
// its CLI contract. zahlen_application publishes zahlen_engine_cli to callers.
#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdlib>
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

// A layer may implement either or both phases. Calls are nonvirtual, in the
// order added, with the scene already built for Start and input already pumped
// for Tick. Layers are owned by the Application, including move-only objects.
template <typename T>
concept AppLayer = requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.Start(engine);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine, float dt) {
    layer.Tick(engine, dt);
};

// Bounds for integral CLI configuration fields. The existing
// ZHLN_ANNOTATION annotation syntax attaches this to a reflected member.
template <auto Min, auto Max>
struct CliRange {
    static_assert(std::integral<decltype(Min)> && std::integral<decltype(Max)>, "CliRange bounds must be integral");
    static_assert(std::cmp_less_equal(Min, Max), "CliRange minimum must not exceed maximum");
};

// Optional single-letter short alias for a reflected field (e.g. -c).
template <char Key>
struct CliShort {
    static_assert((Key >= 'a' && Key <= 'z') || (Key >= 'A' && Key <= 'Z'), "CliShort requires a letter");
};

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

    // Bind integral fields of an annotated config to --member-name. Reflected
    // keys and short aliases are owned here (unlike hand-written handlers).
    // The config itself is borrowed and must outlive the synchronous Run.
    template <typename ConfigStruct>
        requires(!std::is_const_v<ConfigStruct> && std::is_class_v<ConfigStruct>)
    auto WithConfig(ConfigStruct& target) -> Application& {
        Reflect::ForEachDataMember<ConfigStruct>([&]<auto member>() {
            using Field = std::remove_cvref_t<decltype(Reflect::MemberValue<member>(target))>;
            static_assert(std::integral<Field> && !std::same_as<Field, bool>, "Reflected CLI fields must be integral (not bool)");
            static_assert(Reflect::AnnotationCountOf<CliRange, member>() <= 1, "Only one CliRange annotation is allowed per field");
            static_assert(Reflect::AnnotationCountOf<CliShort, member>() <= 1, "Only one CliShort annotation is allowed per field");
            constexpr std::string_view name = Reflect::MemberName<member>();
            static_assert(!name.empty(), "Reflected CLI fields must be named");
            constexpr std::string_view description = Reflect::GetDescriptionText<member>();

            auto key = std::make_shared<const std::string>(std::string("--") + std::string(name));
            std::shared_ptr<const std::string> shortKey;
            if constexpr (Reflect::AnnotationCountOf<CliShort, member>() > 0) {
                constexpr char letter = Reflect::AnnotationTemplateArgument<CliShort, member, 0, char>();
                shortKey = std::make_shared<const std::string>(std::string("-") + letter);
            }
            AddCliOption(CommandHandler {
                .key         = *key,
                .shortKey    = shortKey ? std::string_view(*shortKey) : std::string_view {},
                .placeholder = "<integer>",
                .description = description.empty() ? std::string_view(*key).substr(2) : description,
                .action      = [&target](CommandLineOptions&, std::string_view value) -> std::expected<void, ErrorCode> {
                    if (value.empty()) {
                        return std::unexpected(CommandLineError::MissingValue);
                    }
                    Field parsed {};
                    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                    if (error != std::errc {} || end != value.data() + value.size()) {
                        return std::unexpected(CommandLineError::InvalidValue);
                    }
                    if constexpr (Reflect::AnnotationCountOf<CliRange, member>() > 0) {
                        constexpr long double minimum = Reflect::AnnotationTemplateArgument<CliRange, member, 0, long double>();
                        constexpr long double maximum = Reflect::AnnotationTemplateArgument<CliRange, member, 1, long double>();
                        const long double number = static_cast<long double>(parsed);
                        if (number < minimum || number > maximum) {
                            return std::unexpected(CommandLineError::InvalidValue);
                        }
                    }
                    Reflect::MemberValue<member>(target) = parsed; // commit only after validation
                    return {};
                },
            });
            _reflectedOptionNames.push_back(std::move(key));
            if (shortKey) {
                _reflectedOptionNames.push_back(std::move(shortKey));
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
        _layers.push_back(std::move(slot));
        return *this;
    }

    // Application lifecycle, not launch configuration. Called once after the
    // initial scene is built (e.g. to spawn persistent entities).
    auto OnStart(std::function<void(Engine&)> start) -> Application& {
        _start = std::move(start);
        return *this;
    }

    auto OnUI(Engine::UICallback callback) -> Application& {
        _ui = std::move(callback);
        return *this;
    }

    // System-driven apps need no per-frame callable. With no CLI parse there is
    // no CommandLineOptions service; a span-based Run publishes parsed options.
    auto Run() const -> int { return RunConfigured(_config, {}, nullptr); }
    auto Run(std::span<char* const> args) const -> int { return RunFromCLI(_config, args, {}); }

    // Adapter for main(int argc, char* argv[]). Reject malformed C arguments
    // instead of silently turning them into a valid empty command line.
    auto Run(int argc, char* argv[]) const -> int {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return EXIT_FAILURE;
        }
        return Run(MakeSpan(argc, argv));
    }

    // Gameplay runs once per frame after events/resize and before Engine::Tick.
    // Both (Engine&) and (Engine&, float dt) work. The callable is borrowed
    // synchronously until Run returns; even move-only state can be captured.
    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(GameLogic&& logic) const -> int {
        return RunConfigured(_config, BindLogic(logic), nullptr);
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(std::span<char* const> args, GameLogic&& logic) const -> int {
        return RunFromCLI(_config, args, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(int argc, char* argv[], GameLogic&& logic) const -> int {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return EXIT_FAILURE;
        }
        return Run(MakeSpan(argc, argv), std::forward<GameLogic>(logic));
    }

  private:
    struct LayerSlot {
        // Shared ownership keeps the existing copyable Application API and
        // leaves no virtual dispatch in the per-frame Start/Tick call paths.
        std::shared_ptr<void> state;
        void (*start)(void*, Engine&)        = nullptr;
        void (*tick)(void*, Engine&, float)  = nullptr;
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

    auto RunConfigured(const EngineRunConfig& config, const Update& update, const CommandLineOptions* options) const -> int;
    auto RunFromCLI(EngineRunConfig config, std::span<char* const> args, const Update& update) const -> int;

    EngineRunConfig                           _config;
    Engine::UICallback                        _ui;
    std::function<void(Engine&)>              _start;
    std::vector<std::function<void(Engine&)>> _installers;
    std::vector<CommandHandler>               _cliHandlers;
    std::vector<std::shared_ptr<const std::string>> _reflectedOptionNames;
    std::vector<LayerSlot>                    _layers;
    bool                                      _defaultModules = true;
};

[[nodiscard]] inline auto App(EngineRunConfig config = {}) -> Application {
    return Application(std::move(config));
}

} // namespace ZHLN
