// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/toml/UITOML.hpp
//
// Include this (not toml/TOML.hpp alone) to round-trip a GUI::UINode as a
// document. The tree itself is format-free -- Zahlen/gui/UITree.hpp knows
// nothing about TOML -- and the colours on NodeBox are JPH::Float4, so they
// need the same `[r, g, b, a]` bindings SceneTOML.hpp already provides for
// scene files:
//
//     #include <Zahlen/gui/UITree.hpp>
//     #include <toml/UITOML.hpp>
//
//     const auto tree = ReflectTOML::TryParse<ZHLN::GUI::UINode>(text);
//     const auto text = ReflectTOML::SerializeTOML(tree);
//
// Kind, Direction and Alignment come out as quoted enumerator names, children
// as [[children]] tables, and a missing key keeps the field default -- the
// same contract as a scene document.

#include <Zahlen/gui/UITree.hpp>
#include <toml/SceneTOML.hpp>
