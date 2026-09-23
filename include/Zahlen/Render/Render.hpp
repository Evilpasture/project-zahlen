// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/Render.hpp
//
// The renderer's public surface. This is the header to include when you want to
// draw something; the siblings are here so a caller that needs only one piece --
// the frame vocabulary, a descriptor struct, the counter block -- can take that
// piece without the renderer's whole dispatch surface coming with it.
//
// Following the convention of the tree's other subsystem directories
// (ecs/ECS.hpp, gui/GUI.hpp, physics/Physics.hpp): the facade lives inside the
// directory, and there is no alias at include/Zahlen/Render.hpp.
#pragma once
#include <Zahlen/Render/FrameResult.hpp>   // FrameOutcome, FrameSkipped, PresentSuboptimal, FrameResult
#include <Zahlen/Render/GpuEnums.hpp>      // LightType, ParticleAlignment
#include <Zahlen/Render/GpuLayout.hpp>     // the generated GPU structs: FrameUniforms, Light, the emitter params
#include <Zahlen/Render/Info.hpp>          // RenderInfo, PresentationMode, PhysicalDeviceType, RenderResult
#include <Zahlen/Render/PipelineStats.hpp> // GpuPipelineCounters
#include <Zahlen/Render/RenderContext.hpp> // RenderContext, PipelineStatsCapture
#include <Zahlen/Render/Types.hpp>         // MaterialDesc, DrawParams, CSGDrawParams, DecalParams
#include <Zahlen/Render/View.hpp>          // SceneView, UIView
