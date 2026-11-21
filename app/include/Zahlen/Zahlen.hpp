// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
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
// Public umbrella header for COM application hosts; Jolt stays private to the engine.

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

#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Math3D.hpp>

#include <Zahlen/Threading/TaskSystem.hpp>

// --- fixed-layout engine vocabulary and COM capabilities ---------------------
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/GUI/UIData.hpp>

// --- host policy (not linked into the engine) ---------------------------------
#include <Zahlen/App.hpp>
#include <Zahlen/AppOutcome.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/HostRunner.hpp>
