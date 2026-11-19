// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/GUI/Font.hpp>
#include <Zahlen/GUI/UIData.hpp>
#include <type_traits>

namespace ZHLN::Test {

static_assert(std::is_standard_layout_v<UIBatch> && std::is_trivially_copyable_v<UIBatch>);
static_assert(std::is_standard_layout_v<GlyphMetric> && std::is_trivially_copyable_v<GlyphMetric>);
static_assert(std::is_standard_layout_v<CookedFontHeader> && std::is_trivially_copyable_v<CookedFontHeader>);
static_assert(std::is_standard_layout_v<CookedFontGlyph> && std::is_trivially_copyable_v<CookedFontGlyph>);

void test_ui_data_pod_and_custom_cursor_geometry() {
    constexpr UIDrawData empty {};
    ExpectTrue(empty.Empty());

    const CustomCursorStyle style {
        .innerRadius   = 3.0f,
        .outlineRadius = 5.0f,
        .innerColor    = {.data = 0xFFFFFFFFu},
        .outlineColor  = {.data = 0xFF000000u},
    };
    const CustomCursor cursor(640.0f, 360.0f, style);
    const UIDrawData   draw = cursor.DrawData();
    ExpectFalse(draw.Empty());
    ExpectEq(draw.batches.size(), size_t {1});
    ExpectEq(draw.positions.size(), CustomCursor::kVertexCount);
    ExpectEq(draw.surfaces.size(), CustomCursor::kVertexCount);
    ExpectEq(draw.batches[0].vertexCount, static_cast<uint32_t>(CustomCursor::kVertexCount));
    ExpectFalse(draw.batches[0].isSDF);
}

} // namespace ZHLN::Test
