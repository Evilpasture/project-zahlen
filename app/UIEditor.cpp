#include <Zahlen/HostUI.hpp>
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// app/UIEditor.cpp
//
// v0.1 UI-tree editor. A second composition-root binary next to `zahlen`:
// this one edits a GUI::UINode rather than a 3D world.
//
// The host owns GUI::Context + GUI::UIInput state per window. It drives the
// COM engine without accessing a World, registry, renderer or native window.
// The document is a UINode tree; both windows submit ordinary copied UI data.
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
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Widget/TextFit.hpp>
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

using ZHLN::FVec4;

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
    // Only the id is held. The Window is engine-owned and looked up per use
    // (Engine::WindowFor): a host must not keep a Window& across a frame, and a
    // preview window that was closed underneath it would leave a dangling one.
    ZHLN::WindowID                previewID = ZHLN::WindowID::Invalid;
    GUI::Context                  editorGui;
    GUI::Context                  previewGui;
    GUI::UIInput                 editorInput {};
    bool editorCaptureKeyboard = false;
    ZHLN::HostUI* hostUI = nullptr; // borrowed from Application, not engine-owned
    GUI::UIInput                 previewInput {};
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
            .color   = selected ? FVec4 {0.20f, 0.35f, 0.55f, 0.95f} : FVec4 {0.10f, 0.12f, 0.16f, 0.80f},
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

void DrawFloat4(GUI::Frame& gui, std::string_view prefix, ZHLN::FVec4& value, float minVal, float maxVal, float step) {
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

void UpdateCanvasXform(const GUI::Context& gui, Session& session, const ZHLN::COM::HostInputInfo& state, bool uiOwnsKeyboard) {
    const float mx          = state.mouseX;
    const float my          = state.mouseY;
    const bool  controlDown = state.IsKeyDown(ZHLN::KeyCode::LControl) || state.IsKeyDown(ZHLN::KeyCode::RControl);
    const bool  gDown       = state.IsKeyDown(ZHLN::KeyCode::G);
    const bool  sDown       = state.IsKeyDown(ZHLN::KeyCode::S);
    const bool  rDown       = state.IsKeyDown(ZHLN::KeyCode::R);
    const bool  confirmDown = state.IsKeyDown(ZHLN::KeyCode::Enter) || state.IsMouseButtonDown(ZHLN::MouseButton::Left);
    const bool  cancelDown  = state.IsKeyDown(ZHLN::KeyCode::Escape);

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

// Explicit host composition: both windows submit copied geometry before Tick.
auto DrawPreview(ZHLN::COM::IEngine& engine, Session& session) -> ZHLN::COM::Result<void> {
    const auto previewSize = engine.WindowExtent(session.previewID);
    if (!previewSize.width || !previewSize.height) return {};
    session.previewGui.SetFontAtlas(session.fontAtlas);
    GUI::Frame gui = session.previewGui.BeginFrame(
        session.previewInput = session.hostUI->ConsumeInput(session.dt, session.previewID)
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
    const ZHLN::UIDrawData uiData = std::move(gui).End();
    return session.hostUI->Submit(uiData, session.previewID);
}

[[nodiscard]] auto PreviewIsRunning(ZHLN::COM::IEngine& engine, const Session& session) -> bool {
    if (session.previewID == ZHLN::WindowID::Invalid) {
        return false;
    }
    return engine.IsWindowRunning(session.previewID);
}

void StopPreview(ZHLN::COM::IEngine& engine, Session& session) {
    if (session.previewID != ZHLN::WindowID::Invalid) {
        engine.RemoveWindow(session.previewID);
        session.previewID = ZHLN::WindowID::Invalid;
    }
}

void StopPreview(ZHLN::COM::IEngine& engine, Session& session);
void OpenPreview(ZHLN::COM::IEngine& engine, Session& session);

// Once per frame, before the engine's Tick: the editor's clock, the preview
// window's life, and actions queued by the previous host UI composition.
void UpdateEditor(ZHLN::COM::IEngine& engine, Session& session, float dt) {
    session.dt = dt;
    if (session.previewID != ZHLN::WindowID::Invalid && !PreviewIsRunning(engine, session)) {
        StopPreview(engine, session);
    }
    if (session.openPreviewRequested) {
        session.openPreviewRequested = false;
        OpenPreview(engine, session);
    }
    session.events.Drain<GUI::UiActionEvent>([](const GUI::UiActionEvent& event) {
        if (event.id == "editor.save_scene") {
            ZHLN::Log("[UIEditor] editor.save_scene");
        }
    });
}

void OpenPreview(ZHLN::COM::IEngine& engine, Session& session) {
    if (PreviewIsRunning(engine, session)) {
        ZHLN::Log("[UIEditor] Preview window already open");
        return;
    }
    StopPreview(engine, session);
    session.previewProperties = session.properties;
    session.previewInput      = {};
    session.previewGui        = GUI::Context(session.fontAtlas);

    // The preview window is an ordinary destination: the editor draws its 2D
    // tree into (and only into) it, so nothing about the window has to declare
    // what kind of content it accepts.
    const auto previewWindow = engine.AddWindow(ZHLN::COM::WindowCreateInfo {.title = "UI Preview", .width = 800, .height = 600});
    if (!previewWindow.HasValue()) {
        ZHLN::Log("[UIEditor] Preview AddWindow failed");
        return;
    }
    session.previewID = previewWindow.value;

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

// Builds this window's UI and publishes it. The render step draws whatever is
// published for a window into that window's acquired target, which is why this
// does not touch a FrameScope: it runs in the UI phase, before any frame is
// acquired.
auto SubmitEditorUI(ZHLN::COM::IEngine& engine, Session& session) -> ZHLN::COM::Result<void> {
    ZHLN::COM::HostInputInfo state;
    (void)engine.ReadHostInput(engine.PrimaryWindow(), ZHLN::COM::OutChain(state), false);
    const bool uiOwnsKeyboard = session.editorGui.IsTextInputFocused() || session.editorCaptureKeyboard;

    const bool xformWasActive = session.xform != XformMode::None;
    UpdateCanvasXform(session.editorGui, session, state, uiOwnsKeyboard);

    const bool escDown = state.IsKeyDown(ZHLN::KeyCode::Escape);
    if (escDown && !session.escWasDown && !uiOwnsKeyboard && !xformWasActive) {
        session.selectedId.clear();
    }
    session.escWasDown = escDown;

#if defined(ZHLN_HAS_UI_TOML)
    const bool controlDown = state.IsKeyDown(ZHLN::KeyCode::LControl) || state.IsKeyDown(ZHLN::KeyCode::RControl);
    const bool saveDown    = controlDown && state.IsKeyDown(ZHLN::KeyCode::S);
    if (saveDown && !session.saveWasDown && !uiOwnsKeyboard) {
        SaveTree(session.tree, kDocumentPath);
    }
    session.saveWasDown = saveDown;
#endif

    const ZHLN::Extent2D size = engine.WindowExtent(engine.PrimaryWindow());
    session.editorGui.SetFontAtlas(session.fontAtlas);
    GUI::Frame gui = session.editorGui.BeginFrame(
        session.editorInput = session.hostUI->ConsumeInput(session.dt)
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
                            if (PreviewIsRunning(engine, session)) {
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
    ZHLN::UIDrawData uiData = std::move(gui).End();
    session.editorCaptureKeyboard = uiData.wantsCaptureKeyboard;
    if (uiData.Empty()) {
        return {};
    }
    return session.hostUI->Submit(std::move(uiData));

}

} // namespace

auto main(int argc, char* argv[]) -> int {
    Session session;
    session.tree       = MakeDemoTree();
    session.selectedId = "panel";
    BindHostActions(session);

    struct EditorOptions { uint32_t frames = 0; } options;
    struct EditorLayer {
        Session* session;
        uint32_t* frameLimit;
        void Start(ZHLN::COM::IEngine& engine, ZHLN::HostUI& ui) {
            session->hostUI = &ui;
#if defined(ZHLN_HAS_FONTS)
            auto baked = ZHLN::Fonts::LoadBakedFont(engine.Assets(), ZHLN::Fonts::VendoredDefaultFontSource());
            if (baked) ui.SetBakedFont(std::move(*baked));
            else ZHLN::LogWarning("Font load failed ({}); using embedded default.", baked.error());
#endif
        }
        void Tick(ZHLN::COM::IEngine& engine, float dt) {
            UpdateEditor(engine, *session, dt);
            if (*frameLimit && engine.CurrentFrame() >= *frameLimit) engine.RequestClose();
        }
        auto UI(ZHLN::COM::IEngine& engine, ZHLN::HostUI& ui) -> std::expected<void, ZHLN::ErrorCode> {
            // The host retains the CPU atlas and rebuilds its texture whenever
            // the device generation changes; no engine callback captures Session.
            session->fontAtlas = ui.GetFontAtlas();
            session->editorGui.SetFontAtlas(session->fontAtlas);
            session->previewGui.SetFontAtlas(session->fontAtlas);
            const auto submitted = SubmitEditorUI(engine, *session);
            if (!submitted.HasValue()) return std::unexpected(submitted.error);
            if (PreviewIsRunning(engine, *session)) {
                const auto preview = DrawPreview(engine, *session);
                if (!preview.HasValue()) return std::unexpected(preview.error);
            }
            return {};
        }
        void Stop(ZHLN::COM::IEngine& engine) { StopPreview(engine, *session); session->hostUI = nullptr; }
    };
    ZHLN::EngineConfig editorConfig {};
    editorConfig.appName = "Zahlen UI Editor";
    const auto result = ZHLN::Application(editorConfig).WithConfig(options).AddLayer(EditorLayer {&session, &options.frames}).Run(argc, argv);
    return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
