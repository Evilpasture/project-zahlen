// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// ============================================================================
// ECS ↔ GraphicsSettings synchronisation.
//
// The ECS settings components (PostProcessSettingsComponent,
// ShadowSettingsComponent, AASettingsComponent) are the *editing surface* —
// what ImGui sliders, Lua scripts and quality presets write to. This helper
// is the single collector that folds them into the canonical GraphicsSettings
// model and pushes it into the RenderContext once per frame
// (RenderSystem::RenderMain). The renderer never queries the components
// itself, and nothing else calls the loose legacy setters.
// ============================================================================

#include <Zahlen/Common.h>
#include <Zahlen/GraphicsSettings.hpp>

namespace ZHLN {

class Engine;

/// Folds the ECS settings components into a GraphicsSettings snapshot.
/// Missing components fall back to the model's defaults; the RT
/// enableReflections mirror is kept in sync with the post.enableRTR ABI
/// toggle here (single writer).
[[nodiscard]] ZHLN_API GraphicsSettings CollectGraphicsSettings(Engine& engine);

/// Collects and applies the settings to the engine's RenderContext
/// (delta-detected — e.g. reacts to shadow-resolution changes by resizing the
/// GPU cascade targets). Returns the applied snapshot for uniform assembly.
ZHLN_API GraphicsSettings SyncGraphicsSettings(Engine& engine);

/// Writes a quality preset back into the ECS editing surface (the fields of
/// GraphicsSettings::QualitySignature). The next SyncGraphicsSettings applies
/// it to the renderer. Returns false when there was no component to write
/// (and none could be created) or the preset was Custom.
ZHLN_API bool ApplyQualityPreset(Engine& engine, QualityLevel preset);

} // namespace ZHLN
