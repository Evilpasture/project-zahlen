// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// The widgets a panel is built from.
//
// GUI::Context is the substrate: boxes, rows, text, buttons. Everything above it
// is a pattern -- "the newest N of a rolling series, drawn as bars", "measure
// this text against the box it goes in", "a card that appears when the pointer
// is over something" -- and until now every panel that wanted one wrote it
// again, slightly differently, and got a slightly different answer:
//
//   * the frame-time chart projected its ring from the wrong end and froze once
//     the window filled, because the projection was four lines inside a panel;
//   * a sparkline whose samples all sat at the noise floor filled every bar to
//     the top, because "scale to the row's own peak" has to say what happens
//     when the peak is nothing;
//   * text fitted to its box still wrapped and left its ".." on a line of its
//     own, because the advance is an average and a line of digits is wider.
//
// None of those are GUI bugs, and none of them were in GUI::Context. They are
// bugs in a pattern, and a pattern is exactly the thing that should exist once.
// This extension is where each one lives now: small, documented, tested on its
// own, and used by the panels that were carrying their own copies.
//
// The widgets:
//
//   * RollingWindow<T, N> (RollingWindow.hpp) -- the ring behind anything that
//     keeps "the last N samples": push a sample, copy the newest or the oldest
//     into the strip you are going to draw. It owns the index arithmetic that
//     froze the chart.
//   * BarStrip (BarStrip.hpp) -- a span of samples drawn as bars: the shared
//     scale of a frame-time chart, the own-peak-with-a-floor of a sparkline, the
//     per-bar colour of a load cell, the dim prefix of a series that has not
//     filled its window yet.
//   * HoverCard (HoverCard.hpp) -- the card a line, a row or a cell opens when
//     the pointer is over it: collected while the panel is drawn, drawn last and
//     beside the panel, placed against the element's own rectangle.
//   * TextFit (TextFit.hpp) -- the per-character advance of the font in use,
//     whether a line fits, and the cut that keeps it from spilling.
//
// What they are not: a layout engine, a retained-mode UI, or a second way to
// draw. Each one is a sequence of GUI::Context calls plus the arithmetic a
// caller should not have to repeat, and a caller that wants something else can
// still call the context directly -- the widgets do not own the panel.
//
// Lives in extensions/Widget/ (zahlen_widgets) rather than in core: core keeps
// src/gui/, the immediate-mode substrate (the Clay wrapper, TextEdit, the font
// rasterizer), and this layer builds on GUI::Context through its public API.
// The namespace is ZHLN::GUI::Widget, so a widget reads as part of the GUI
// family the way <UI/UITree.hpp> does.
//
// Nothing here names a platform, a device or an engine subsystem: a widget draws
// through the context it is handed and reads the rectangles that context
// reports. That is what lets a test drive one headlessly.
// ===========================================================================

#include <Widget/BarStrip.hpp>
#include <Widget/HoverCard.hpp>
#include <Widget/RollingWindow.hpp>
#include <Widget/TextFit.hpp>
