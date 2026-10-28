// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure CPU verification of Zahlen's binary-decoupled immediate-mode GUI
// subsystem (`zahlen_gui`):
//   - Zero header or link coupling to Engine, ECS::Registry, Components,
//     PlatformHost, or RenderContext.
//   - FontAtlas treated strictly as an asset value fed into GUI::Context.
//   - Per-frame input supplied via explicit GUI::UIInput values.
//   - Immediate-mode widget emission gated behind the move-only GUI::Frame
//     capability token.

#include "TestsFramework.hpp"
#include <Zahlen/gui/FontLoader.hpp>
#include <Zahlen/gui/GUI.hpp>

#include <array>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(std::is_trivially_copyable_v<ZHLN::UIDrawData>);

static_assert(!std::is_copy_constructible_v<ZHLN::GUI::Context>);
static_assert(!std::is_copy_assignable_v<ZHLN::GUI::Context>);
static_assert(std::is_move_constructible_v<ZHLN::GUI::Context>);
static_assert(std::is_move_assignable_v<ZHLN::GUI::Context>);

static_assert(!std::is_default_constructible_v<ZHLN::GUI::Frame>);
static_assert(!std::is_copy_constructible_v<ZHLN::GUI::Frame>);
static_assert(!std::is_copy_assignable_v<ZHLN::GUI::Frame>);
static_assert(std::is_move_constructible_v<ZHLN::GUI::Frame>);
static_assert(!std::is_move_assignable_v<ZHLN::GUI::Frame>);

namespace ZHLN::Test {

auto test_gui_font_atlas_value_feeds_sdf_batches_without_ecs() -> std::expected<void, ZHLN::ErrorCode> {
    const ZHLN::GUI::BakedFontAsset& baked = ZHLN::GUI::GetDefaultBakedFont();
    const ZHLN::TextureHandle        tex {42};
    const ZHLN::FontAtlas            atlas = ZHLN::GUI::BuildFontAtlas(baked, tex);

    ExpectEq(atlas.texture, tex);
    ExpectGt(atlas.glyphCount, 0u);
    ExpectGt(atlas.fontSize, 0.0f);
    ExpectGt(atlas.GlyphFor('A').x1, atlas.GlyphFor('A').x0);

    // Without a FontAtlas, text falls back to non-SDF solid glyph quads on Invalid texture.
    ZHLN::GUI::Context plainCtx;
    {
        ZHLN::GUI::Frame frame = plainCtx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {640, 480}, .dt = 0.016f});
        frame.Text("Hello", 16.0f);
        const ZHLN::UIDrawData draw = std::move(frame).End();
        ExpectTrue(!draw.Empty());
        ExpectEq(draw.positions.size(), draw.surfaces.size());
        ExpectFalse(draw.batches[0].isSDF);
        ExpectEq(draw.batches[0].texture, ZHLN::TextureHandle::Invalid);
    }

    // Feeding the FontAtlas value directly into GUI::Context switches text to SDF batches.
    ZHLN::GUI::Context sdfCtx(atlas);
    ExpectEq(sdfCtx.GetFontAtlas().texture, tex);
    {
        ZHLN::GUI::Frame frame = sdfCtx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {640, 480}, .dt = 0.016f});
        frame.Text("Hello", 16.0f);
        const ZHLN::UIDrawData draw = std::move(frame).End();
        ExpectTrue(!draw.Empty());
        ExpectEq(draw.positions.size(), draw.surfaces.size());
        ExpectTrue(draw.batches[0].isSDF);
        ExpectEq(draw.batches[0].texture, tex);
    }

    return {};
}

auto test_gui_frame_capability_token_lifecycle_and_raii_cleanup() -> std::expected<void, ZHLN::ErrorCode> {
    ZHLN::GUI::Context ctx;
    ExpectFalse(ctx.HasActiveFrame());

    // Move-constructing the Frame token transfers ownership cleanly and keeps the frame open.
    {
        ZHLN::GUI::Frame first = ctx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {800, 600}, .dt = 0.016f});
        ExpectTrue(ctx.HasActiveFrame());

        ZHLN::GUI::Frame moved = std::move(first);
        ExpectTrue(ctx.HasActiveFrame());
        moved.Box("Root", ZHLN::GUI::BoxConfig {.width = {.fixed = 120.0f}, .height = {.fixed = 40.0f}}, [&]() -> void {
            moved.Text("Token");
        });

        const ZHLN::UIDrawData draw = std::move(moved).End();
        ExpectFalse(ctx.HasActiveFrame());
        ExpectTrue(!draw.Empty());
    }

    // Dropping an un-ended Frame token (early return) cleanly closes Clay's layout pass via RAII
    // so the subsequent frame opens and lays out without corruption.
    {
        ZHLN::GUI::Frame abandoned = ctx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {800, 600}, .dt = 0.016f});
        abandoned.BeginBox("UnclosedBox", ZHLN::GUI::BoxConfig {.width = {.fixed = 100.0f}, .height = {.fixed = 50.0f}});
        ExpectTrue(ctx.HasActiveFrame());
    }
    ExpectFalse(ctx.HasActiveFrame());

    {
        ZHLN::GUI::Frame next = ctx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {800, 600}, .dt = 0.016f});
        next.Box(
            "Recovered",
            ZHLN::GUI::BoxConfig {.width = {.fixed = 160.0f}, .height = {.fixed = 60.0f}, .color = {0.2f, 0.3f, 0.4f, 1.0f}},
            [&]() -> void {}
        );
        const ZHLN::UIDrawData draw = std::move(next).End();
        ExpectTrue(!draw.Empty());
    }

    const auto rect = ctx.GetLastFrameRect("Recovered");
    ExpectTrue(rect.has_value());
    if (rect.has_value()) {
        ExpectEq(rect->width, 160.0f);
        ExpectEq(rect->height, 60.0f);
    }

    return {};
}

auto test_gui_button_hover_single_fire_drag_and_mouse_capture() -> std::expected<void, ZHLN::ErrorCode> {
    ZHLN::GUI::Context ctx;
    uint32_t           clicksA = 0;
    uint32_t           clicksB = 0;
    bool               hoveredA = false;
    bool               activeA  = false;

    auto runFrame = [&](float mx, float my, bool lmb) -> void {
        ZHLN::GUI::Frame ui = ctx.BeginFrame(
            ZHLN::GUI::UIInput {
                .viewport  = {640, 480},
                .dt        = 0.016f,
                .mouseX    = mx,
                .mouseY    = my,
                .mouseDown = {lmb, false, false},
            }
        );

        // Full-viewport transparent workspace must NOT capture mouse when cursor is in empty space.
        ui.Box(
            "TransparentWorkspace",
            ZHLN::GUI::BoxConfig {
                .width     = {.grow = 1.0f},
                .height    = {.grow = 1.0f},
                .color     = {0.0f, 0.0f, 0.0f, 0.0f},
                .padding   = 20.0f,
                .gap       = 10.0f,
                .direction = ZHLN::GUI::Direction::Column,
            },
            [&]() -> void {
                if (ui.Button("ButtonA")) {
                    ++clicksA;
                }
                hoveredA = ui.IsItemHovered();
                activeA  = ui.IsItemActive();

                if (ui.Button("ButtonB")) {
                    ++clicksB;
                }
            }
        );

        const ZHLN::UIDrawData draw = std::move(ui).End();
        ExpectEq(draw.wantsCaptureMouse, ctx.WantsCaptureMouse());
        ExpectEq(draw.wantsCaptureKeyboard, ctx.WantsCaptureKeyboard());
    };

    // Frame 1: Seed layout with cursor in empty space (500, 400).
    runFrame(500.0f, 400.0f, false);
    ExpectFalse(ctx.WantsCaptureMouse());
    ExpectEq(clicksA, 0u);

    // Frame 2: Hover ButtonA at (45, 32).
    runFrame(45.0f, 32.0f, false);
    ExpectTrue(hoveredA);
    ExpectFalse(activeA);
    ExpectTrue(ctx.WantsCaptureMouse());
    ExpectEq(clicksA, 0u);

    // Frame 3: Press over ButtonA -> single-fire click.
    runFrame(45.0f, 32.0f, true);
    ExpectEq(clicksA, 1u);
    ExpectTrue(activeA);

    // Frame 4: Hold button down -> remains active, does NOT fire a second time.
    runFrame(45.0f, 32.0f, true);
    ExpectEq(clicksA, 1u);
    ExpectTrue(activeA);

    // Frame 5: Release mouse -> active clears.
    runFrame(45.0f, 32.0f, false);
    ExpectFalse(activeA);

    // Frame 6 & 7: Press in empty space and drag onto ButtonB -> must NOT click ButtonB.
    runFrame(500.0f, 400.0f, true);
    const auto rectB = ctx.GetLastFrameRect("ButtonB");
    ExpectTrue(rectB.has_value());
    const float bx = rectB ? (rectB->x + 10.0f) : 45.0f;
    const float by = rectB ? (rectB->y + 10.0f) : 75.0f;
    runFrame(bx, by, true);
    ExpectEq(clicksB, 0u);

    // Release and click cleanly over ButtonB -> fires once.
    runFrame(bx, by, false);
    runFrame(bx, by, true);
    ExpectEq(clicksB, 1u);

    return {};
}

auto test_gui_text_input_editing_clipboard_and_keyboard_capture() -> std::expected<void, ZHLN::ErrorCode> {
    ZHLN::GUI::Context ctx;
    std::string        value = "init";
    std::string        clipboardText;

    const ZHLN::GUI::TextEdit::ClipboardSink clipboard {
        .userdata = &clipboardText,
        .set      = [](void* ud, std::string_view text) -> void { *static_cast<std::string*>(ud) = std::string(text); },
        .get      = [](void* ud) -> std::string { return *static_cast<std::string*>(ud); },
    };

    auto step = [&](float mx, float my, bool lmb, ZHLN::KeyModifiers mods, std::span<const ZHLN::KeyCode> keys, std::span<const uint32_t> chars) -> bool {
        ZHLN::GUI::Frame ui = ctx.BeginFrame(
            ZHLN::GUI::UIInput {
                .viewport        = {640, 480},
                .dt              = 0.016f,
                .mouseX          = mx,
                .mouseY          = my,
                .mouseDown       = {lmb, false, false},
                .modifiers       = mods,
                .pressedKeys     = keys,
                .characterStream = chars,
                .clipboard       = clipboard,
            }
        );
        bool changed = false;
        ui.Box("Form", ZHLN::GUI::BoxConfig {.width = {.fixed = 320.0f}, .height = {.fixed = 120.0f}, .padding = 10.0f}, [&]() -> void {
            changed = ui.TextInput("Name", value);
        });
        const ZHLN::UIDrawData draw = std::move(ui).End();
        ExpectEq(draw.wantsCaptureMouse, ctx.WantsCaptureMouse());
        ExpectEq(draw.wantsCaptureKeyboard, ctx.WantsCaptureKeyboard());
        return changed;
    };

    // Frame 1: Establish layout so TextInput's field box exists.
    step(0.0f, 0.0f, false, ZHLN::KeyModifiers::None, {}, {});
    ExpectFalse(ctx.IsTextInputFocused());
    ExpectFalse(ctx.WantsCaptureKeyboard());

    const auto inputRect = ctx.GetLastFrameRect("Name");
    ExpectTrue(inputRect.has_value());
    const float fx = inputRect ? (inputRect->x + 12.0f) : 100.0f;
    const float fy = inputRect ? (inputRect->y + 10.0f) : 24.0f;

    // Frame 2: Click inside field -> focuses text input & captures keyboard.
    step(fx, fy, true, ZHLN::KeyModifiers::None, {}, {});
    ExpectTrue(ctx.IsTextInputFocused());
    ExpectTrue(ctx.WantsCaptureKeyboard());

    // Frame 3: Ctrl+A followed by typing "ok" replaces "init" -> "ok".
    const std::array<ZHLN::KeyCode, 1> selectAll {ZHLN::KeyCode::A};
    const std::array<uint32_t, 2>      typed {'o', 'k'};
    ExpectTrue(step(fx, fy, false, ZHLN::KeyModifiers::Ctrl, selectAll, typed));
    ExpectEq(std::string_view(value), std::string_view("ok"));

    // Frame 4: Ctrl+A + Ctrl+C copies "ok" into ClipboardSink.
    const std::array<ZHLN::KeyCode, 2> selectAndCopy {ZHLN::KeyCode::A, ZHLN::KeyCode::C};
    step(fx, fy, false, ZHLN::KeyModifiers::Ctrl, selectAndCopy, {});
    ExpectEq(std::string_view(clipboardText), std::string_view("ok"));

    // Frame 5: End + Ctrl+V appends pasted clipboard ("ok!").
    clipboardText = "!";
    const std::array<ZHLN::KeyCode, 1> moveEnd {ZHLN::KeyCode::End};
    step(fx, fy, false, ZHLN::KeyModifiers::None, moveEnd, {});
    const std::array<ZHLN::KeyCode, 1> paste {ZHLN::KeyCode::V};
    ExpectTrue(step(fx, fy, false, ZHLN::KeyModifiers::Ctrl, paste, {}));
    ExpectEq(std::string_view(value), std::string_view("ok!"));

    // Frame 6: Escape unfocuses the text field and releases keyboard capture.
    const std::array<ZHLN::KeyCode, 1> esc {ZHLN::KeyCode::Escape};
    step(fx, fy, false, ZHLN::KeyModifiers::None, esc, {});
    ExpectFalse(ctx.IsTextInputFocused());
    ExpectFalse(ctx.WantsCaptureKeyboard());

    return {};
}

auto test_gui_multiple_contexts_remain_isolated() -> std::expected<void, ZHLN::ErrorCode> {
    ZHLN::GUI::Context editorCtx;
    ZHLN::GUI::Context previewCtx;

    {
        ZHLN::GUI::Frame f1 = editorCtx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {1280, 720}, .dt = 0.016f});
        f1.Box("SharedId", ZHLN::GUI::BoxConfig {.width = {.fixed = 300.0f}, .height = {.fixed = 500.0f}}, [&]() -> void {});
        [[maybe_unused]] const auto d1 = std::move(f1).End();
    }
    {
        ZHLN::GUI::Frame f2 = previewCtx.BeginFrame(ZHLN::GUI::UIInput {.viewport = {800, 600}, .dt = 0.016f});
        f2.Box("SharedId", ZHLN::GUI::BoxConfig {.width = {.fixed = 120.0f}, .height = {.fixed = 90.0f}}, [&]() -> void {});
        [[maybe_unused]] const auto d2 = std::move(f2).End();
    }

    const auto r1 = editorCtx.GetLastFrameRect("SharedId");
    const auto r2 = previewCtx.GetLastFrameRect("SharedId");
    ExpectTrue(r1.has_value());
    ExpectTrue(r2.has_value());
    if (r1 && r2) {
        ExpectEq(r1->width, 300.0f);
        ExpectEq(r1->height, 500.0f);
        ExpectEq(r2->width, 120.0f);
        ExpectEq(r2->height, 90.0f);
    }

    return {};
}

auto test_custom_cursor_geometry_and_batch_invariants() -> std::expected<void, ZHLN::ErrorCode> {
    const ZHLN::CustomCursorStyle style {
        .innerRadius   = 3.5f,
        .outlineRadius = 5.5f,
        .innerColor    = {.data = 0xFAFFFFFFu},
        .outlineColor  = {.data = 0xE6140E0Eu},
    };
    const ZHLN::CustomCursor cursor(640.0f, 360.0f, style);
    const ZHLN::UIDrawData   draw = cursor.DrawData();

    ExpectFalse(draw.Empty());
    ExpectEq(draw.batches.size(), size_t {1});
    ExpectEq(draw.positions.size(), ZHLN::CustomCursor::kVertexCount);
    ExpectEq(draw.surfaces.size(), ZHLN::CustomCursor::kVertexCount);
    ExpectEq(draw.batches[0].texture, ZHLN::TextureHandle::Invalid);
    ExpectEq(draw.batches[0].vertexStart, 0u);
    ExpectEq(draw.batches[0].vertexCount, static_cast<uint32_t>(ZHLN::CustomCursor::kVertexCount));
    ExpectFalse(draw.batches[0].useScissor);
    ExpectFalse(draw.batches[0].isSDF);

    for (const auto& pos : draw.positions) {
        const float dx = pos.position[0] - 640.0f;
        const float dy = pos.position[1] - 360.0f;
        ExpectTrue(dx * dx + dy * dy <= style.outlineRadius * style.outlineRadius + 1e-3f);
        ExpectEq(pos.position[2], 0.0f);
    }

    return {};
}

} // namespace ZHLN::Test
