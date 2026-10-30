// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The application-layer umbrella: one include for everything an application,
// host binary, editor or sample tool compiles against.
//
// It belongs to the *application* layer, not the engine: it lives in
// app/include/, so only targets that carry that include root can see it, and
// Core targets still cannot reach an application header. The engine keeps its
// per-subsystem includes and has no engine-wide header of its own
// (ARCHITECTURE.md §1.3).
//
// Jolt is deliberately not here. A unit that names a JPH:: type includes
// <Zahlen/Physics/Jolt.hpp> itself, so the JPH_* ABI macros spread only as far
// as the code that actually speaks Jolt.

// --- foundation -------------------------------------------------------------
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/String.hpp>

// --- math -------------------------------------------------------------------
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Math3D.hpp>

// --- threading --------------------------------------------------------------
#include <Zahlen/Threading/TaskSystem.hpp>

// --- ECS --------------------------------------------------------------------
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/ECS/EventBus.hpp>

// --- simulation -------------------------------------------------------------
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Audio/Audio.hpp>

// --- presentation -----------------------------------------------------------
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/RenderGraphDump.hpp>
#include <Zahlen/Render/View.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/Window/WindowInput.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/GUI/GUI.hpp>

// --- engine composition -----------------------------------------------------
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Engine/Kernel.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/Scene.hpp>

// --- application ------------------------------------------------------------
#include <Zahlen/App.hpp>
#include <Zahlen/AppOutcome.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/KernelApp.hpp>
