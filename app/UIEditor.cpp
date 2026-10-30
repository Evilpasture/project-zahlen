// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// app/UIEditor.cpp
//
// v0.1 UI-tree editor. A second composition-root binary next to `zahlen`:
// this one edits a GUI::UINode rather than a 3D world.
//
// It runs on a Kernel alone -- windows, GPU, audio, assets -- with explicit
// GUI::Context + GUI::UIInput state per window. No World, no ECS::Registry, no
// physics, no camera, no frame scheduler: the document is a UINode tree and
// the chrome is immediate-mode Clay, so nothing here needs a simulation.
//
//   Left   Hierarchy  -- one row per node id; click selects a container
//   Center Canvas     -- RenderUITree(..., TreeMode::Design); G/S/R grab,
//                        scale, rotate the selection (pixel / 15° snap)
//   Right  Inspector  -- edits FindNodeById(tree, selectedId); px-snapped
//   Preview           -- second OS window owned by the Kernel (AddWindow).
//                        DrawPreview renders TreeMode::Preview straight into
//                        that window's acquired image with RenderUI; both
//                        windows are presented by the one EndFrame. Same
//                        device, same UI pass; no second graph, no scene.
//
// Chrome is immediate-mode Clay. The document being edited is the UINode
// tree; Design-mode hits and hierarchy clicks write the same selectedId
// string that FindNodeById / InsertChild / RemoveNodeById already speak.

#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Engine/Kernel.hpp>
#include <Zahlen/KernelApp.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/View.hpp>
#include <Widget/TextFit.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/ECS/EventBus.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <UI/UITree.hpp>
#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif
#if defined(ZHLN_HAS_UI_TOML)
#include <toml/UITOML.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace GUI = ZHLN::GUI;

constexpr float            kLeftPanelWidth  = 260.0f;
constexpr float            kRightPanelWidth = 320.0f;
constexpr std::string_view kDocumentPath    = "ui.toml";
constexpr float            kPixelSnap       = 1.0f;
constexpr float            kGrowSnap        = 0.25f;
constexpr float            kColorSnap       = 0.05f;
constexpr float            kDegreeSnap      = 15.0f;
constexpr std::string_view kCanvasStageId   = "CanvasStage";

// The hierarchy's geometry, as the layout above builds it: a 260 px panel with
// 10 px of padding, a row box with 4 px of its own, and 10 px of indent per
// level. A row's label is cut to what is left of that width -- an element wider
// than its box is not clipped, Clay draws past the edge, so a node several levels
// down would otherwise draw its path over the panel's border.
constexpr float kHierarchyRowTextWidth = kLeftPanelWidth - (10.0f * 2.0f) - (4.0f * 2.0f);
constexpr float kHierarchyIndent       = 10.0f;
constexpr float kHierarchyFontSize     = 13.0f;

[[nodiscard]] auto Snap(float value, float step) -> float {
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

void SnapSlider(GUI::Frame& gui, std::string_view label, float& value, float minVal, float maxVal, float step) {
    gui.Slider(label, value, minVal, maxVal);
    value = std::clamp(Snap(value, step), minVal, maxVal);
}

[[nodiscard]] auto NodeId(const GUI::UINode& node, std::string_view path) -> std::string {
    if (!node.id.empty()) {
        return node.id;
    }
    if (!path.empty()) {
        return std::string {path};
    }
    return "node";
}

[[nodiscard]] auto ChildPath(std::string_view parent, size_t index) -> std::string {
    std::string out;
    out.reserve(parent.size() + 8);
    out.append(parent);
    out.push_back('/');
    out.append(std::to_string(index));
    return out;
}

[[nodiscard]] auto RootPath(const GUI::UINode& root) -> std::string {
    return root.id.empty() ? std::string {"root"} : root.id;
}

[[nodiscard]] auto IsContainer(GUI::NodeKind kind) -> bool {
    return kind == GUI::NodeKind::Box || kind == GUI::NodeKind::Row || kind == GUI::NodeKind::Column;
}

[[nodiscard]] auto MakeDemoTree() -> GUI::UINode {
    GUI::UINode root;
    root.id          = "panel";
    root.kind        = GUI::NodeKind::Column;
    root.label       = "Root";
    root.box.padding = 12.0f;
    root.box.gap     = 8.0f;
    root.box.color   = {0.10f, 0.10f, 0.12f, 1.0f};
    root.box.width   = {.grow = 1.0f};
    root.box.height  = {.grow = 1.0f};

    GUI::UINode title;
    title.id    = "title";
    title.kind  = GUI::NodeKind::Text;
    title.label = "Hello";

    GUI::UINode save;
    save.id            = "save";
    save.kind          = GUI::NodeKind::Button;
    save.label         = "Save";
    save.onClickAction = "editor.save_scene";

    GUI::UINode speed;
    speed.id           = "speed";
    speed.kind         = GUI::NodeKind::Slider;
    speed.label        = "Speed";
    speed.bindProperty = "camera.speed";
    speed.minVal       = 0.0f;
    speed.maxVal       = 20.0f;

    root.children.push_back(std::move(title));
    root.children.push_back(std::move(save));
    root.children.push_back(std::move(speed));
    return root;
}

enum class XformMode : uint8_t { None = 0, Grab, Scale, Rotate };

struct WindowUIInputCollector {
    static constexpr size_t kQueueCap = 64;

    ZHLN::Components::InputStateComponent state {};
    std::array<ZHLN::KeyCode, kQueueCap>  pressedKeys {};
    std::array<uint32_t, kQueueCap>       charStream {};
    std::array<ZHLN::KeyCode, kQueueCap>  latchedKeys {};
    std::array<uint32_t, kQueueCap>       latchedChars {};
    size_t                                pressedCount = 0;
    size_t                                charCount    = 0;
    float                                 scrollDelta  = 0.0f;

    [[nodiscard]] auto MakeReceiver() noexcept -> ZHLN::WindowInputReceiver {
        return ZHLN::WindowInputReceiver {
            .userdata = this,
            .onKey =
                [](void* ud, ZHLN::KeyCode key, bool pressed) -> void {
                    auto* self = static_cast<WindowUIInputCollector*>(ud);
                    self->state.SetKey(static_cast<uint8_t>(key), pressed);
                    if (pressed && self->pressedCount < kQueueCap) {
                        self->pressedKeys[self->pressedCount++] = key;
                    }
                },
            .onMouseMove =
                [](void* ud, float x, float y) -> void { static_cast<WindowUIInputCollector*>(ud)->state.ApplyLocalMotion(x, y); },
            .onMouseRelative =
                [](void* ud, float x, float y, float dx, float dy) -> void {
                    static_cast<WindowUIInputCollector*>(ud)->state.ApplyRelativeMotion(x, y, dx, dy);
                },
            .onMouseScroll =
                [](void* ud, float delta) -> void {
                    auto* self = static_cast<WindowUIInputCollector*>(ud);
                    self->state.ApplyWheel(delta);
                    self->scrollDelta += delta;
                },
            .onChar =
                [](void* ud, unsigned int codepoint) -> void {
                    auto* self = static_cast<WindowUIInputCollector*>(ud);
                    if (self->charCount < kQueueCap) {
                        self->charStream[self->charCount++] = codepoint;
                    }
                },
        };
    }

    [[nodiscard]] auto Consume(ZHLN::Extent2D viewport, float dt, GUI::TextEdit::ClipboardSink clipboard = {}) noexcept -> GUI::UIInput {
        const size_t nKeys  = pressedCount;
        const size_t nChars = charCount;
        std::copy_n(pressedKeys.data(), nKeys, latchedKeys.data());
        std::copy_n(charStream.data(), nChars, latchedChars.data());
        pressedCount = 0;
        charCount    = 0;

        ZHLN::KeyModifiers mods = ZHLN::KeyModifiers::None;
        if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) ||
            state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl))) {
            mods = mods | ZHLN::KeyModifiers::Ctrl;
        }
        if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LShift)) ||
            state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RShift))) {
            mods = mods | ZHLN::KeyModifiers::Shift;
        }
        if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LAlt)) ||
            state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RAlt))) {
            mods = mods | ZHLN::KeyModifiers::Alt;
        }

        const float scroll = scrollDelta;
        scrollDelta        = 0.0f;

        return GUI::UIInput {
            .viewport    = viewport,
            .dt          = dt,
            .mouseX      = state.mouseX,
            .mouseY      = state.mouseY,
            .mouseScroll = scroll,
            .mouseDown   = {
                state.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton)),
                state.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton)),
                state.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::MButton)),
            },
            .modifiers       = mods,
            .pressedKeys     = std::span<const ZHLN::KeyCode>(latchedKeys.data(), nKeys),
            .characterStream = std::span<const uint32_t>(latchedChars.data(), nChars),
            .clipboard       = clipboard,
        };
    }
};

struct Session {
    GUI::UINode                   tree;
    ZHLN::ECS::EventBus           events;
    GUI::ActionRegistry           actions;
    GUI::PropertyStore            properties;
    std::string                   selectedId;
    int                           nextNodeIndex  = 1;
    int                           addKindIndex   = 0;
    bool                          escWasDown     = false;
    bool                          saveWasDown    = false;
    bool                          gWasDown       = false;
    bool                          sWasDown       = false;
    bool                          rWasDown       = false;
    bool                          confirmWasDown = false;
    float                         dt                   = 0.016f;
    bool                          openPreviewRequested = false;
    ZHLN::Optional<ZHLN::Window&> previewWindow; // kernel-owned; see Kernel::AddWindow
    GUI::Context                  editorGui;
    GUI::Context                  previewGui;
    WindowUIInputCollector        editorInput;
    WindowUIInputCollector        previewInput;
    GUI::PropertyStore            previewProperties; // runtime instance; Design keeps `properties`

    // The font the chrome draws with, for measuring a row against its panel
    // (see DrawHierarchyRow): empty until the atlas is built, and the measuring
    // falls back to the documented ratio while it is.
    ZHLN::FontAtlas fontAtlas {};

    XformMode    xform         = XformMode::None;
    GUI::NodeBox xformBackup   {};
    float        startMouseX   = 0.0f;
    float        startMouseY   = 0.0f;
    float        startOffsetX  = 0.0f;
    float        startOffsetY  = 0.0f;
    float        startWidth    = 0.0f;
    float        startHeight   = 0.0f;
    float        startRotation = 0.0f;
    float        pivotX        = 0.0f;
    float        pivotY        = 0.0f;
    float        startDist     = 1.0f;
    float        startAngle    = 0.0f;
};

void BindHostActions(Session& session) {
    session.actions.SetEventBus(session.events);
    session.actions.Bind("editor.save_scene", GUI::UiActionEvent {.id = "editor.save_scene"});
    session.properties.SetFloat("camera.speed", 4.5f);
    session.properties.SetBool("post.enableSSR", false);
    session.properties.SetString("scene.name", "Untitled");
}

[[nodiscard]] auto MakeChild(Session& session, GUI::NodeKind kind) -> GUI::UINode {
    GUI::UINode child;
    std::array<char, 32> idBuf {};
    child.id    = std::string {ZHLN::FormatTo(idBuf, "node_{}", session.nextNodeIndex++)};
    child.kind  = kind;
    child.label = child.id;
    if (IsContainer(kind)) {
        child.box.padding = 8.0f;
        child.box.gap     = 4.0f;
        child.box.color   = {0.16f, 0.18f, 0.22f, 1.0f};
    }
    return child;
}

void DrawHierarchyRow(GUI::Frame& gui, Session& session, const GUI::UINode& node, std::string_view path, int depth) {
    const std::string id       = NodeId(node, path);
    const bool        selected = session.selectedId == id;

    std::array<char, 160> hierIdBuf {};
    std::array<char, 160> labelBuf {};
    const std::string_view hierId = ZHLN::FormatTo(hierIdBuf, "hier/{}", id);

    // The label is cut to the row it goes in rather than drawn past it (see
    // <Widget/TextFit.hpp>): the indent is inside the row, the measure is the
    // font the chrome is drawing with, and the cut goes into the buffer the
    // label was formatted into, so a row costs no allocation a frame.
    const ZHLN::FontAtlas* atlasPtr  = session.fontAtlas.glyphCount > 0 ? &session.fontAtlas : nullptr;
    const float            indent    = static_cast<float>(depth) * kHierarchyIndent;
    const float            advance   = GUI::Widget::AdvancePerChar(atlasPtr, kHierarchyFontSize);
    const float            textWidth = std::max(kHierarchyRowTextWidth - indent, 0.0f);
    const std::string_view label = GUI::Widget::FitToWidthInto(
        ZHLN::FormatTo(labelBuf, "{}  {}", id, ZHLN::Reflect::EnumToString(node.kind)), textWidth, advance, labelBuf
    );

    gui.Box(
        hierId,
        GUI::BoxConfig {
            .width   = {.grow = 1.0f},
            .color   = selected ? JPH::Vec4 {0.20f, 0.35f, 0.55f, 0.95f} : JPH::Vec4 {0.10f, 0.12f, 0.16f, 0.80f},
            .padding = 4.0f,
        },
        [&]() {
            if (depth > 0) {
                gui.BeginRow(0.0f, indent);
            }
            gui.Text(label, kHierarchyFontSize, {0.92f, 0.94f, 0.98f, 1.0f});
            if (depth > 0) {
                gui.EndRow();
            }
        }
    );
    if (gui.IsPointerOver(hierId) && gui.IsPointerPressedThisFrame()) {
        session.selectedId = id;
    }

    for (size_t i = 0; i < node.children.size(); ++i) {
        DrawHierarchyRow(gui, session, node.children[i], ChildPath(id, i), depth + 1);
    }
}

void DrawHierarchy(GUI::Frame& gui, Session& session) {
    gui.Text("Hierarchy", 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    constexpr auto kKinds = ZHLN::Reflect::EnumNames<GUI::NodeKind>();
    gui.Dropdown("Add as", std::span<const std::string_view>(kKinds), session.addKindIndex);

    auto       selected = GUI::FindNodeById(session.tree, session.selectedId);
    const bool canAdd   = selected && IsContainer(selected->kind);
    const bool canDel   = selected && NodeId(session.tree, RootPath(session.tree)) != session.selectedId;

    gui.BeginRow(4.0f);
    if (canAdd && gui.Button("Add Child", {0.16f, 0.30f, 0.20f, 0.95f})) {
        const auto kind = static_cast<GUI::NodeKind>(session.addKindIndex);
        if (GUI::InsertChild(session.tree, session.selectedId, MakeChild(session, kind))) {
            // Stay on the container so the next Add Child keeps landing here.
        }
    }
    if (canDel && gui.Button("Delete", {0.42f, 0.16f, 0.16f, 0.95f})) {
        if (GUI::RemoveNodeById(session.tree, session.selectedId)) {
            session.selectedId.clear();
        }
    }
    gui.EndRow();

    if (!canAdd && selected) {
        gui.Text("Select a Box / Row / Column to add children.", 12.0f, {0.55f, 0.55f, 0.58f, 1.0f});
    }

    gui.Box(
        "HierarchyList",
        GUI::BoxConfig {
            .width        = {.grow = 1.0f},
            .height       = {.grow = 1.0f},
            .gap          = 4.0f,
            .direction    = GUI::Direction::Column,
            .clipVertical = true,
        },
        [&]() { DrawHierarchyRow(gui, session, session.tree, RootPath(session.tree), 0); }
    );
}

[[nodiscard]] auto ParentIdOf(const GUI::UINode& node, std::string_view targetId, std::string_view path) -> std::string {
    const std::string id = NodeId(node, path);
    for (size_t i = 0; i < node.children.size(); ++i) {
        const std::string childPath = ChildPath(id, i);
        if (NodeId(node.children[i], childPath) == targetId) {
            return id;
        }
        if (const std::string found = ParentIdOf(node.children[i], targetId, childPath); !found.empty()) {
            return found;
        }
    }
    return {};
}

void DrawFloat4(GUI::Frame& gui, std::string_view prefix, JPH::Float4& value, float minVal, float maxVal, float step) {
    for (int axis = 0; axis < 4; ++axis) {
        std::array<char, 64> idBuf {};
        const std::string_view label = ZHLN::FormatTo(idBuf, "{} {}", prefix, "RGBA"[axis]);
        float& component = (&value.x)[axis];
        SnapSlider(gui, label, component, minVal, maxVal, step);
    }
}

void DrawInspector(GUI::Frame& gui, Session& session) {
    gui.Text("Inspector", 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    auto node = GUI::FindNodeById(session.tree, session.selectedId);
    if (!node) {
        gui.Text("No selection", 12.0f, {0.5f, 0.5f, 0.5f, 1.0f});
        return;
    }

    gui.BeginBox(
        "InspectorList",
        GUI::BoxConfig {
            .width        = {.grow = 1.0f},
            .height       = {.grow = 1.0f},
            .gap          = 6.0f,
            .direction    = GUI::Direction::Column,
            .clipVertical = true,
        }
    );

    if (gui.BeginCollapsingHeader("Identity", true)) {
        gui.TextInput("id", node->id);
        // Keep selectedId in lockstep with the live node so a renamed id does
        // not leave the inspector pointing at a name that no longer exists.
        if (!node->id.empty()) {
            session.selectedId = node->id;
        }

        constexpr auto kKinds = ZHLN::Reflect::EnumNames<GUI::NodeKind>();
        int            kind   = static_cast<int>(node->kind);
        if (gui.Dropdown("kind", std::span<const std::string_view>(kKinds), kind)) {
            node->kind = static_cast<GUI::NodeKind>(kind);
        }
        gui.TextInput("label", node->label);
        gui.EndCollapsingHeader();
    }

    if (gui.BeginCollapsingHeader("Box", true)) {
        SnapSlider(gui, "padding", node->box.padding, 0.0f, 64.0f, kPixelSnap);
        SnapSlider(gui, "gap", node->box.gap, 0.0f, 64.0f, kPixelSnap);
        SnapSlider(gui, "x", node->box.offsetX, -800.0f, 800.0f, kPixelSnap);
        SnapSlider(gui, "y", node->box.offsetY, -800.0f, 800.0f, kPixelSnap);
        SnapSlider(gui, "rotation", node->box.rotation, -180.0f, 180.0f, kDegreeSnap);
        SnapSlider(gui, "width.fixed", node->box.width.fixed, 0.0f, 800.0f, kPixelSnap);
        SnapSlider(gui, "width.grow", node->box.width.grow, 0.0f, 4.0f, kGrowSnap);
        gui.Checkbox("width.fit", node->box.width.fit);
        SnapSlider(gui, "height.fixed", node->box.height.fixed, 0.0f, 800.0f, kPixelSnap);
        SnapSlider(gui, "height.grow", node->box.height.grow, 0.0f, 4.0f, kGrowSnap);
        gui.Checkbox("height.fit", node->box.height.fit);
        DrawFloat4(gui, "color", node->box.color, 0.0f, 1.0f, kColorSnap);
        DrawFloat4(gui, "radius", node->box.cornerRadius, 0.0f, 32.0f, kPixelSnap);

        constexpr auto kDir  = ZHLN::Reflect::EnumNames<GUI::Direction>();
        constexpr auto kAlign = ZHLN::Reflect::EnumNames<GUI::Alignment>();
        int            dir    = static_cast<int>(node->box.direction);
        int            mainA  = static_cast<int>(node->box.alignMain);
        int            crossA = static_cast<int>(node->box.alignCross);
        if (gui.Dropdown("direction", std::span<const std::string_view>(kDir), dir)) {
            node->box.direction = static_cast<GUI::Direction>(dir);
        }
        if (gui.Dropdown("alignMain", std::span<const std::string_view>(kAlign), mainA)) {
            node->box.alignMain = static_cast<GUI::Alignment>(mainA);
        }
        if (gui.Dropdown("alignCross", std::span<const std::string_view>(kAlign), crossA)) {
            node->box.alignCross = static_cast<GUI::Alignment>(crossA);
        }
        gui.EndCollapsingHeader();
    }

    if (gui.BeginCollapsingHeader("Widget", true)) {
        gui.TextInput("onClickAction", node->onClickAction);
        gui.TextInput("bindProperty", node->bindProperty);
        SnapSlider(gui, "fontSize", node->fontSize, 8.0f, 48.0f, kPixelSnap);
        DrawFloat4(gui, "text", node->textColor, 0.0f, 1.0f, kColorSnap);
        SnapSlider(gui, "minVal", node->minVal, -100.0f, 100.0f, 0.5f);
        SnapSlider(gui, "maxVal", node->maxVal, -100.0f, 100.0f, 0.5f);
        gui.EndCollapsingHeader();
    }

    gui.EndBox();
}

void CancelXform(Session& session) {
    if (session.xform == XformMode::None) {
        return;
    }
    if (auto node = GUI::FindNodeById(session.tree, session.selectedId)) {
        node->box = session.xformBackup;
    }
    session.xform = XformMode::None;
}

void ConfirmXform(Session& session) {
    session.xform = XformMode::None;
}

void BeginXform(const GUI::Context& gui, Session& session, XformMode mode, float mx, float my) {
    auto node = GUI::FindNodeById(session.tree, session.selectedId);
    if (!node) {
        return;
    }

    session.xform       = mode;
    session.xformBackup = node->box;
    session.startMouseX = mx;
    session.startMouseY = my;

    const auto selfRect = gui.GetLastFrameRect(session.selectedId);
    const std::string parentId = ParentIdOf(session.tree, session.selectedId, RootPath(session.tree));
    const auto parentRect = gui.GetLastFrameRect(parentId.empty() ? kCanvasStageId : std::string_view {parentId});

    if (selfRect) {
        session.pivotX = selfRect->x + selfRect->width * 0.5f;
        session.pivotY = selfRect->y + selfRect->height * 0.5f;
    } else {
        session.pivotX = mx;
        session.pivotY = my;
    }

    if (mode == XformMode::Grab) {
        if (selfRect && parentRect) {
            node->box.offsetX = Snap(selfRect->x - parentRect->x, kPixelSnap);
            node->box.offsetY = Snap(selfRect->y - parentRect->y, kPixelSnap);
        }
        session.startOffsetX = node->box.offsetX;
        session.startOffsetY = node->box.offsetY;
    } else if (mode == XformMode::Scale) {
        if (selfRect) {
            if (node->box.width.fixed <= 0.0f) {
                node->box.width.fixed = Snap(selfRect->width, kPixelSnap);
                node->box.width.grow  = 0.0f;
                node->box.width.fit   = false;
            }
            if (node->box.height.fixed <= 0.0f) {
                node->box.height.fixed = Snap(selfRect->height, kPixelSnap);
                node->box.height.grow  = 0.0f;
                node->box.height.fit   = false;
            }
        }
        session.startWidth  = node->box.width.fixed;
        session.startHeight = node->box.height.fixed;
        session.startDist   = std::hypot(mx - session.pivotX, my - session.pivotY);
        if (session.startDist < 1.0f) {
            session.startDist = 1.0f;
        }
    } else if (mode == XformMode::Rotate) {
        session.startRotation = node->box.rotation;
        session.startAngle    = std::atan2(my - session.pivotY, mx - session.pivotX);
    }
}

void ApplyXform(Session& session, float mx, float my) {
    auto node = GUI::FindNodeById(session.tree, session.selectedId);
    if (!node) {
        session.xform = XformMode::None;
        return;
    }

    switch (session.xform) {
        case XformMode::Grab:
            node->box.offsetX = Snap(session.startOffsetX + (mx - session.startMouseX), kPixelSnap);
            node->box.offsetY = Snap(session.startOffsetY + (my - session.startMouseY), kPixelSnap);
            break;
        case XformMode::Scale: {
            const float dist   = std::hypot(mx - session.pivotX, my - session.pivotY);
            const float factor = dist / session.startDist;
            node->box.width.fixed  = std::max(0.0f, Snap(session.startWidth * factor, kPixelSnap));
            node->box.height.fixed = std::max(0.0f, Snap(session.startHeight * factor, kPixelSnap));
            break;
        }
        case XformMode::Rotate: {
            const float angle = std::atan2(my - session.pivotY, mx - session.pivotX);
            const float deg   = (angle - session.startAngle) * (180.0f / 3.14159265f);
            node->box.rotation = Snap(session.startRotation + deg, kDegreeSnap);
            break;
        }
        case XformMode::None:
            break;
    }
}

void UpdateCanvasXform(const GUI::Context& gui, Session& session, const ZHLN::Components::InputStateComponent& state, bool uiOwnsKeyboard) {
    const float mx = state.mouseX;
    const float my = state.mouseY;
    const bool controlDown =
        state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) ||
        state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl));
    const bool gDown = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::G));
    const bool sDown = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S));
    const bool rDown = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::R));
    const bool confirmDown =
        state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Enter)) ||
        state.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool cancelDown = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape));

    if (session.xform != XformMode::None) {
        ApplyXform(session, mx, my);
        if (cancelDown && !session.escWasDown) {
            CancelXform(session);
        } else if (confirmDown && !session.confirmWasDown) {
            ConfirmXform(session);
        }
        session.gWasDown       = gDown;
        session.sWasDown       = sDown;
        session.rWasDown       = rDown;
        session.confirmWasDown = confirmDown;
        return;
    }

    if (!uiOwnsKeyboard && !session.selectedId.empty()) {
        if (gDown && !session.gWasDown) {
            BeginXform(gui, session, XformMode::Grab, mx, my);
        } else if (sDown && !session.sWasDown && !controlDown) {
            BeginXform(gui, session, XformMode::Scale, mx, my);
        } else if (rDown && !session.rWasDown) {
            BeginXform(gui, session, XformMode::Rotate, mx, my);
        }
    }

    session.gWasDown       = gDown;
    session.sWasDown       = sDown;
    session.rWasDown       = rDown;
    session.confirmWasDown = confirmDown;
}

#if defined(ZHLN_HAS_UI_TOML)
void SaveTree(const GUI::UINode& tree, std::string_view path);
void LoadTree(Session& session, std::string_view path);
#endif

[[nodiscard]] auto MakeHostClipboard(ZHLN::PlatformHost& host) noexcept -> GUI::TextEdit::ClipboardSink {
    return GUI::TextEdit::ClipboardSink {
        .userdata = &host,
        .set      = [](void* ud, std::string_view text) -> void { static_cast<ZHLN::PlatformHost*>(ud)->SetClipboardText(text); },
        .get      = [](void* ud) -> std::string { return static_cast<ZHLN::PlatformHost*>(ud)->GetClipboardText(); },
    };
}

void DrawPreview(ZHLN::Kernel& kernel, Session& session, ZHLN::FrameScope& gpuFrame) {
    if (!session.previewWindow) {
        return;
    }
    const ZHLN::Extent2D previewSize = session.previewWindow->GetSize();
    if (previewSize.width == 0 || previewSize.height == 0) {
        return;
    }
    session.previewGui.SetFontAtlas(session.fontAtlas);
    GUI::Frame gui = session.previewGui.BeginFrame(
        session.previewInput.Consume(previewSize, session.dt, MakeHostClipboard(kernel.GetPlatformHost()))
    );
    gui.Box(
        "PreviewRoot",
        GUI::BoxConfig {
            .width     = {.grow = 1.0f},
            .height    = {.grow = 1.0f},
            .color     = {0.04f, 0.05f, 0.07f, 1.0f},
            .padding   = 8.0f,
            .direction = GUI::Direction::Column,
        },
        [&]() {
            (void) GUI::RenderUITree(gui, session.tree, session.actions, session.previewProperties, GUI::TreeMode::Preview);
        }
    );
    auto&                  rc               = kernel.GetRenderContext();
    const ZHLN::UIDrawData uiData           = std::move(gui).End();
    const bool             drawCustomCursor = session.previewWindow->UsesCustomCursor();
    if (!uiData.Empty() || drawCustomCursor) {
        const auto target = kernel.AcquireTarget(*session.previewWindow);
        if (!target) {
            ZHLN::Log("[UIEditor] Preview window acquisition failed: {}", target.error());
            return;
        }
        if (!target->has_value()) {
            return;
        }
        if (!uiData.Empty()) {
            if (const auto drawn = gpuFrame.RenderUI(
                    ZHLN::UIView {
                        .viewport   = {.x = 0, .y = 0, .width = previewSize.width, .height = previewSize.height},
                        .target     = **target,
                        .frameIndex = rc.GetFrameIndex(),
                    },
                    uiData
                );
                !drawn) {
                ZHLN::Log("[UIEditor] Preview UI draw failed: {}", drawn.error());
            }
        }
        if (drawCustomCursor) {
            const float cursorX = previewSize.width > 0
                                      ? std::clamp(session.previewInput.state.mouseX, 0.0f, static_cast<float>(previewSize.width - 1))
                                      : session.previewInput.state.mouseX;
            const float cursorY = previewSize.height > 0
                                      ? std::clamp(session.previewInput.state.mouseY, 0.0f, static_cast<float>(previewSize.height - 1))
                                      : session.previewInput.state.mouseY;
            const ZHLN::CustomCursor cursor(cursorX, cursorY);
            if (const auto drawn = gpuFrame.RenderUI(
                    ZHLN::UIView {
                        .viewport   = {.x = 0, .y = 0, .width = previewSize.width, .height = previewSize.height},
                        .target     = **target,
                        .frameIndex = rc.GetFrameIndex(),
                    },
                    cursor.DrawData()
                );
                !drawn) {
                ZHLN::Log("[UIEditor] Preview custom cursor draw failed: {}", drawn.error());
            }
        }
    }
}

[[nodiscard]] auto PreviewIsRunning(const Session& session) -> bool {
    return session.previewWindow && session.previewWindow->IsRunning();
}

void StopPreview(ZHLN::Kernel& kernel, Session& session) {
    if (!session.previewWindow) {
        return;
    }
    kernel.RemoveWindow(*session.previewWindow);
    session.previewWindow.reset();
}

void OpenPreview(ZHLN::Kernel& kernel, Session& session) {
    if (PreviewIsRunning(session)) {
        ZHLN::Log("[UIEditor] Preview window already open");
        return;
    }
    StopPreview(kernel, session);
    session.previewProperties = session.properties;
    session.previewInput      = {};
    session.previewGui        = GUI::Context(session.fontAtlas);

    // The preview window is an ordinary destination: the editor draws its 2D
    // tree into (and only into) it, so nothing about the window has to declare
    // what kind of content it accepts.
    session.previewWindow = kernel.AddWindow("UI Preview", 800, 600, false, session.previewInput.MakeReceiver());
    if (!session.previewWindow) {
        ZHLN::Log("[UIEditor] Preview AddWindow failed");
        return;
    }
    ZHLN::Log("[UIEditor] Preview window open");
}

#if defined(ZHLN_HAS_UI_TOML)
void SaveTree(const GUI::UINode& tree, std::string_view path) {
    const std::string text = ZHLN::ReflectTOML::SerializeTOML(tree);
    std::ofstream     out {std::string {path}, std::ios::binary | std::ios::trunc};
    if (!out) {
        ZHLN::Log("[UIEditor] could not open '{}' for writing", path);
        return;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (out.fail()) {
        ZHLN::Log("[UIEditor] writing '{}' failed", path);
        return;
    }
    ZHLN::Log("[UIEditor] wrote '{}'", path);
}

void LoadTree(Session& session, std::string_view path) {
    std::ifstream in {std::string {path}, std::ios::binary};
    if (!in) {
        ZHLN::Log("[UIEditor] '{}' is not readable", path);
        return;
    }
    const std::string text {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto              parsed = ZHLN::ReflectTOML::Parse<GUI::UINode>(text);
    if (!parsed) {
        ZHLN::Log("[UIEditor] '{}' did not parse as a UINode", path);
        return;
    }
    session.tree       = std::move(*parsed);
    session.selectedId = RootPath(session.tree);
    ZHLN::Log("[UIEditor] loaded '{}'", path);
}
#endif

void DrawFrame(ZHLN::Kernel& kernel, Session& session, ZHLN::FrameScope& gpuFrame) {
    auto&      state          = session.editorInput.state;
    const bool uiOwnsKeyboard = session.editorGui.IsTextInputFocused() || state.wantCaptureKeyboard;

    const bool xformWasActive = session.xform != XformMode::None;
    UpdateCanvasXform(session.editorGui, session, state, uiOwnsKeyboard);

    const bool escDown = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape));
    if (escDown && !session.escWasDown && !uiOwnsKeyboard && !xformWasActive) {
        session.selectedId.clear();
    }
    session.escWasDown = escDown;

#if defined(ZHLN_HAS_UI_TOML)
    const bool controlDown = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) ||
                             state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl));
    const bool saveDown = controlDown && state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S));
    if (saveDown && !session.saveWasDown && !uiOwnsKeyboard) {
        SaveTree(session.tree, kDocumentPath);
    }
    session.saveWasDown = saveDown;
#endif

    const ZHLN::Extent2D size = kernel.GetPlatformHost().GetSize();
    session.editorGui.SetFontAtlas(session.fontAtlas);
    GUI::Frame gui = session.editorGui.BeginFrame(
        session.editorInput.Consume(size, session.dt, MakeHostClipboard(kernel.GetPlatformHost()))
    );
    gui.Box(
        "UIEditorWorkspace",
        GUI::BoxConfig {
            .width     = {.grow = 1.0f},
            .height    = {.grow = 1.0f},
            .color     = {0.05f, 0.06f, 0.08f, 1.0f},
            .direction = GUI::Direction::Row,
        },
        [&]() {
            gui.Box(
                "HierarchyPanel",
                GUI::BoxConfig {
                    .width     = {.fixed = kLeftPanelWidth},
                    .height    = {.grow = 1.0f},
                    .color     = {0.07f, 0.09f, 0.13f, 0.98f},
                    .padding   = 10.0f,
                    .gap       = 6.0f,
                    .direction = GUI::Direction::Column,
                },
                [&]() { DrawHierarchy(gui, session); }
            );

            gui.Box(
                "CanvasPanel",
                GUI::BoxConfig {
                    .width     = {.grow = 1.0f},
                    .height    = {.grow = 1.0f},
                    .color     = {0.09f, 0.10f, 0.13f, 1.0f},
                    .padding   = 10.0f,
                    .gap       = 8.0f,
                    .direction = GUI::Direction::Column,
                },
                [&]() {
                    gui.Box(
                        "CanvasToolbar",
                        GUI::BoxConfig {
                            .width        = {.grow = 1.0f},
                            .color        = {0.08f, 0.10f, 0.14f, 0.95f},
                            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
                            .padding      = 8.0f,
                            .gap          = 12.0f,
                            .direction    = GUI::Direction::Row,
                            .alignCross   = GUI::Alignment::Center,
                        },
                        [&]() {
                            gui.Text("Canvas", 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});
                            if (gui.Button("Preview", {0.16f, 0.24f, 0.36f, 0.95f})) {
                                session.openPreviewRequested = true;
                            }
                            if (PreviewIsRunning(session)) {
                                gui.Text("Preview window open", 12.0f, {0.55f, 0.75f, 0.55f, 1.0f});
                            }
                            if (session.xform == XformMode::Grab) {
                                gui.Text("Grab  (click / Enter confirm, Esc cancel)", 12.0f, {0.9f, 0.85f, 0.4f, 1.0f});
                            } else if (session.xform == XformMode::Scale) {
                                gui.Text("Scale  (click / Enter confirm, Esc cancel)", 12.0f, {0.9f, 0.85f, 0.4f, 1.0f});
                            } else if (session.xform == XformMode::Rotate) {
                                gui.Text("Rotate 15°  (click / Enter confirm, Esc cancel)", 12.0f, {0.9f, 0.85f, 0.4f, 1.0f});
                            } else {
                                gui.Text("G grab   S scale   R rotate", 12.0f, {0.5f, 0.55f, 0.6f, 1.0f});
                            }
#if defined(ZHLN_HAS_UI_TOML)
                            if (gui.Button("Save", {0.16f, 0.30f, 0.20f, 0.95f})) {
                                SaveTree(session.tree, kDocumentPath);
                            }
                            if (gui.Button("Load", {0.16f, 0.24f, 0.36f, 0.95f})) {
                                LoadTree(session, kDocumentPath);
                            }
#endif
                        }
                    );

                    gui.Box(
                        "CanvasStage",
                        GUI::BoxConfig {
                            .width     = {.grow = 1.0f},
                            .height    = {.grow = 1.0f},
                            .color     = {0.04f, 0.05f, 0.07f, 1.0f},
                            .padding   = 8.0f,
                            .direction = GUI::Direction::Column,
                        },
                        [&]() {
                            const auto result = GUI::RenderUITree(
                                gui, session.tree, session.actions, session.properties, GUI::TreeMode::Design, session.selectedId
                            );
                            if (session.xform == XformMode::None && !result.clickedId.empty()) {
                                session.selectedId = result.clickedId;
                            }
                        }
                    );
                }
            );

            gui.Box(
                "InspectorPanel",
                GUI::BoxConfig {
                    .width     = {.fixed = kRightPanelWidth},
                    .height    = {.grow = 1.0f},
                    .color     = {0.07f, 0.09f, 0.13f, 0.98f},
                    .padding   = 10.0f,
                    .gap       = 6.0f,
                    .direction = GUI::Direction::Column,
                },
                [&]() { DrawInspector(gui, session); }
            );
        }
    );
    auto&                  rc     = kernel.GetRenderContext();
    const ZHLN::UIDrawData uiData = std::move(gui).End();
    session.editorInput.state.wantCaptureMouse    = uiData.wantsCaptureMouse;
    session.editorInput.state.wantCaptureKeyboard = uiData.wantsCaptureKeyboard;
    const bool drawCustomCursor                   = kernel.GetPlatformHost().UsesCustomCursor();
    if (uiData.Empty() && !drawCustomCursor) {
        return;
    }
    // Pure 2D frame: the editor draws into this frame's acquired window target.
    const auto target = kernel.AcquireTarget();
    if (!target) {
        ZHLN::Log("[UIEditor] Window acquisition failed: {}", target.error());
        return;
    }
    if (!target->has_value()) {
        return;
    }
    if (!uiData.Empty()) {
        if (const auto drawn = gpuFrame.RenderUI(
                ZHLN::UIView {
                    .viewport   = {.x = 0, .y = 0, .width = size.width, .height = size.height},
                    .target     = **target,
                    .frameIndex = rc.GetFrameIndex(),
                },
                uiData
            );
            !drawn) {
            ZHLN::Log("[UIEditor] Window UI draw failed: {}", drawn.error());
        }
    }

    if (drawCustomCursor) {
        const float cursorX =
            size.width > 0 ? std::clamp(session.editorInput.state.mouseX, 0.0f, static_cast<float>(size.width - 1)) : session.editorInput.state.mouseX;
        const float cursorY =
            size.height > 0 ? std::clamp(session.editorInput.state.mouseY, 0.0f, static_cast<float>(size.height - 1)) : session.editorInput.state.mouseY;
        const ZHLN::CustomCursor cursor(cursorX, cursorY);
        if (const auto drawn = gpuFrame.RenderUI(
                ZHLN::UIView {
                    .viewport   = {.x = 0, .y = 0, .width = size.width, .height = size.height},
                    .target     = **target,
                    .frameIndex = rc.GetFrameIndex(),
                },
                cursor.DrawData()
            );
            !drawn) {
            ZHLN::Log("[UIEditor] Custom cursor draw failed: {}", drawn.error());
        }
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    Session session;
    session.tree       = MakeDemoTree();
    session.selectedId = "panel";
    BindHostActions(session);

    ZHLN::AssetID activeFontAssetID = ZHLN::GUI::kDefaultFontAssetID;
    auto rebuildFonts = [&](ZHLN::Kernel& kernel) {
        // The restored renderer is a new device; never reuse its predecessor's
        // FontAtlas handles, and do not hold a RenderContext reference across
        // Kernel::HandleDeviceLost.
        session.fontAtlas = ZHLN::PrefabFactory::CreateFontAtlas(
            kernel.GetRenderContext(), kernel.GetAssetManager(), activeFontAssetID
        );
        session.editorGui.SetFontAtlas(session.fontAtlas);
        session.previewGui.SetFontAtlas(session.fontAtlas);
    };

    const auto result = ZHLN::KernelApp()
        .Named("Zahlen UI Editor")
        .Input(session.editorInput.MakeReceiver())
        .OnStart([&](ZHLN::Kernel& kernel) {
            // Fonts are assets, not a renderer-owned outline parser. Prime the
            // baked fallback before creating the application's two GUI atlases.
#if defined(ZHLN_HAS_FONTS)
            if (auto fontAssetID = ZHLN::Fonts::LoadFontAsset(kernel.GetAssetManager(), ZHLN::Fonts::VendoredDefaultFontSource())) {
                activeFontAssetID = *fontAssetID;
            } else {
                ZHLN::LogWarning("Font asset failed to load ({}), falling back to embedded default.", fontAssetID.error());
            }
#endif
            ZHLN::PrefabFactory::PrimeDefaultBakedFont(kernel.GetAssetManager());
            rebuildFonts(kernel);
        })
        .OnInputFrame([&](ZHLN::Kernel&, float) {
            session.previewInput.state.BeginFrame();
            session.editorInput.state.BeginFrame();
        })
        .OnUpdate([&](ZHLN::Kernel& kernel, float dt) {
            session.dt = dt;
            if (session.previewWindow && !session.previewWindow->IsRunning()) {
                StopPreview(kernel, session);
            }
            if (session.openPreviewRequested) {
                session.openPreviewRequested = false;
                OpenPreview(kernel, session);
            }
        })
        .OnRender([&](ZHLN::Kernel& kernel, ZHLN::FrameScope& gpuFrame) {
            DrawFrame(kernel, session, gpuFrame);
            if (session.previewWindow) {
                DrawPreview(kernel, session, gpuFrame);
            }
        })
        .OnDeviceRestored([&](ZHLN::Kernel& kernel) { rebuildFonts(kernel); })
        .OnFrameEnd([&](ZHLN::Kernel&) {
            session.events.Drain<GUI::UiActionEvent>([](const GUI::UiActionEvent& event) {
                if (event.id == "editor.save_scene") {
                    ZHLN::Log("[UIEditor] editor.save_scene");
                }
            });
        })
        .OnShutdown([&](ZHLN::Kernel& kernel) { StopPreview(kernel, session); })
        .Run(argc, argv);
    return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
