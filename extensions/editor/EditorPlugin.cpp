// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the Editor extension. Wraps an
// application-owned ZHLN::Editor::State so a host can run the Hierarchy and
// Inspector panels through the standard plugin ABI (CreateAppLayer + the
// EntryPointWitness seal).
//
// The editor is UI-dependent: its panels need a GUI::Frame that only the host
// can provide (see app/UIEditor.cpp), so this plugin target is optional and
// only built when ZHLN::UI is available. The layer owns Editor::State and
// gates per-frame work through PluginConfig; the host drives the actual panel
// drawing.

#include <editor/GUIEditor.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace ZHLN::Editor {

// Plugin-boundary configuration for the editor layer. Kept out of the public
// extension headers so the extension itself knows nothing about the COM plugin
// ABI.
struct PluginConfig {
    bool enabled = true;  // when false, OnTick is a no-op
    bool showUI  = true;  // when false, the host skips drawing editor panels
};

} // namespace ZHLN::Editor

namespace {

using namespace ZHLN;

class EditorPlugin final : public COM::Implements<EditorPlugin, COM::IAppLayer> {
  public:
    explicit EditorPlugin(Editor::PluginConfig config) noexcept
        : _config(std::move(config)) {}

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        // Editor::State is default-constructed as a member. The host obtains
        // the state (e.g. through its own editor context) and draws the
        // Hierarchy/Inspector panels against it each frame.
        return {};
    }

    auto OnTick(COM::IEngine* engine, float) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (!_config.enabled || !_config.showUI) {
            return {};
        }
        // Panel drawing needs a GUI::Frame, which the host supplies during its
        // UI pass (UIEditor.cpp calls DrawHierarchyPanel / DrawInspectorPanel
        // directly against the editor state). The layer gates on the config
        // flags here so a host can disable the editor without unloading it.
        (void)_state.selectedEntity;
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine*) noexcept override {
        _state = Editor::State {};
    }

  private:
    Editor::State      _state {};
    Editor::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayer(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Editor::PluginConfig parsed {};
    const auto               checked = DispatchInChain<ZHLN::Editor::PluginConfig>(
        config, [&](const ZHLN::Editor::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) EditorPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayer>::Seal() noexcept {}
