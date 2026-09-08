// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// app/UIEditor.cpp
//
// v0.1 UI-tree editor. A second composition-root binary next to `zahlen`:
// this one edits a GUI::UINode rather than a 3D world.
//
//   Left   Hierarchy  -- one row per node id; click selects a container
//   Center Canvas     -- RenderUITree(..., TreeMode::Design); G/S/R grab,
//                        scale, rotate the selection (pixel / 15° snap)
//   Right  Inspector  -- edits FindNodeById(tree, selectedId); px-snapped
//   Preview           -- spawns this binary with --preview (own OS window).
//                        A second in-process Engine cannot exist: Vulkan is
//                        single-instance.
//
// Chrome is immediate-mode Clay. The document being edited is the UINode
// tree; Design-mode hits and hierarchy clicks write the same selectedId
// string that FindNodeById / InsertChild / RemoveNodeById already speak.

#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/gui/UITree.hpp>
#if defined(ZHLN_HAS_UI_TOML)
#include <toml/UITOML.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
// libc's environ. Must not live in the anonymous namespace or the linker
// looks for (anonymous namespace)::environ instead of the process env.
extern "C" char** environ;
#endif

namespace {

namespace GUI = ZHLN::GUI;

constexpr float            kLeftPanelWidth  = 260.0f;
constexpr float            kRightPanelWidth = 320.0f;
constexpr std::string_view kDocumentPath    = "ui.toml";
constexpr std::string_view kPreviewPath     = "ui-preview.toml";
constexpr float            kPixelSnap       = 1.0f;
constexpr float            kGrowSnap        = 0.25f;
constexpr float            kColorSnap       = 0.05f;
constexpr float            kDegreeSnap      = 15.0f;
constexpr std::string_view kCanvasStageId   = "CanvasStage";

[[nodiscard]] auto Snap(float value, float step) -> float {
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

void SnapSlider(GUI::Context& gui, std::string_view label, float& value, float minVal, float maxVal, float step) {
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
    GUI::UINode                 tree;
    GUI::ActionRegistry         actions;
    GUI::PropertyStore          properties;
    std::string                 selectedId;
    int                         nextNodeIndex = 1;
    int                         addKindIndex  = 0;
    bool                        escWasDown    = false;
    bool                        saveWasDown   = false;
    bool                        gWasDown      = false;
    bool                        sWasDown      = false;
    bool                        rWasDown      = false;
    bool                        confirmWasDown = false;
    float                       dt            = 0.016f;
    bool                        openPreviewRequested = false;
    std::string                 executablePath;
#if defined(_WIN32)
    void*                       previewProcess = nullptr;
#else
    int                         previewPid     = -1;
#endif

    XformMode    xform        = XformMode::None;
    GUI::NodeBox xformBackup  {};
    float        startMouseX  = 0.0f;
    float        startMouseY  = 0.0f;
    float        startOffsetX = 0.0f;
    float        startOffsetY = 0.0f;
    float        startWidth   = 0.0f;
    float        startHeight  = 0.0f;
    float        startRotation = 0.0f;
    float        pivotX       = 0.0f;
    float        pivotY       = 0.0f;
    float        startDist    = 1.0f;
    float        startAngle   = 0.0f;
};

void BindHostActions(Session& session) {
    session.actions.Bind("editor.save_scene", []() { ZHLN::Log("[UIEditor] editor.save_scene"); });
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

void DrawHierarchyRow(GUI::Context& gui, Session& session, const GUI::UINode& node, std::string_view path, int depth) {
    const std::string id       = NodeId(node, path);
    const bool        selected = session.selectedId == id;

    std::array<char, 160> hierIdBuf {};
    std::array<char, 160> labelBuf {};
    const std::string_view hierId = ZHLN::FormatTo(hierIdBuf, "hier/{}", id);
    const std::string_view label  = ZHLN::FormatTo(labelBuf, "{}  {}", id, ZHLN::Reflect::EnumToString(node.kind));

    gui.Box(
        hierId,
        GUI::BoxConfig {
            .width   = {.grow = 1.0f},
            .color   = selected ? JPH::Vec4 {0.20f, 0.35f, 0.55f, 0.95f} : JPH::Vec4 {0.10f, 0.12f, 0.16f, 0.80f},
            .padding = 4.0f,
        },
        [&]() {
            if (depth > 0) {
                gui.BeginRow(0.0f, static_cast<float>(depth) * 10.0f);
            }
            gui.Text(label, 13.0f, {0.92f, 0.94f, 0.98f, 1.0f});
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

void DrawHierarchy(GUI::Context& gui, Session& session) {
    gui.Text("Hierarchy", 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    constexpr auto kKinds = ZHLN::Reflect::EnumNames<GUI::NodeKind>();
    gui.Dropdown("Add as", std::span<const std::string_view>(kKinds), session.addKindIndex);

    GUI::UINode* selected = GUI::FindNodeById(session.tree, session.selectedId);
    const bool   canAdd   = selected != nullptr && IsContainer(selected->kind);
    const bool   canDel   = selected != nullptr && NodeId(session.tree, RootPath(session.tree)) != session.selectedId;

    gui.BeginRow(4.0f);
    if (canAdd && gui.Button("Add Child", JPH::Vec4(0.16f, 0.30f, 0.20f, 0.95f))) {
        const auto kind = static_cast<GUI::NodeKind>(session.addKindIndex);
        if (GUI::InsertChild(session.tree, session.selectedId, MakeChild(session, kind))) {
            // Stay on the container so the next Add Child keeps landing here.
        }
    }
    if (canDel && gui.Button("Delete", JPH::Vec4(0.42f, 0.16f, 0.16f, 0.95f))) {
        if (GUI::RemoveNodeById(session.tree, session.selectedId)) {
            session.selectedId.clear();
        }
    }
    gui.EndRow();

    if (!canAdd && selected != nullptr) {
        gui.Text("Select a Box / Row / Column to add children.", 12.0f, {0.55f, 0.55f, 0.58f, 1.0f});
    }

    DrawHierarchyRow(gui, session, session.tree, RootPath(session.tree), 0);
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

void DrawFloat4(GUI::Context& gui, std::string_view prefix, JPH::Float4& value, float minVal, float maxVal, float step) {
    for (int axis = 0; axis < 4; ++axis) {
        std::array<char, 64> idBuf {};
        const std::string_view label = ZHLN::FormatTo(idBuf, "{} {}", prefix, "RGBA"[axis]);
        float& component = (&value.x)[axis];
        SnapSlider(gui, label, component, minVal, maxVal, step);
    }
}

void DrawInspector(GUI::Context& gui, Session& session) {
    gui.Text("Inspector", 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    GUI::UINode* node = GUI::FindNodeById(session.tree, session.selectedId);
    if (node == nullptr) {
        gui.Text("No selection", 12.0f, {0.5f, 0.5f, 0.5f, 1.0f});
        return;
    }

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
}

void CancelXform(Session& session) {
    if (session.xform == XformMode::None) {
        return;
    }
    if (GUI::UINode* node = GUI::FindNodeById(session.tree, session.selectedId); node != nullptr) {
        node->box = session.xformBackup;
    }
    session.xform = XformMode::None;
}

void ConfirmXform(Session& session) {
    session.xform = XformMode::None;
}

void BeginXform(GUI::Context& gui, Session& session, XformMode mode, float mx, float my) {
    GUI::UINode* node = GUI::FindNodeById(session.tree, session.selectedId);
    if (node == nullptr) {
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
    GUI::UINode* node = GUI::FindNodeById(session.tree, session.selectedId);
    if (node == nullptr) {
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

void UpdateCanvasXform(GUI::Context& gui, Session& session, const ZHLN::Components::InputStateComponent* state, bool uiOwnsKeyboard) {
    if (state == nullptr) {
        return;
    }

    const float mx = state->mouseX;
    const float my = state->mouseY;
    const bool controlDown =
        state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) ||
        state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl));
    const bool gDown = state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::G));
    const bool sDown = state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S));
    const bool rDown = state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::R));
    const bool confirmDown =
        state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Enter)) ||
        state->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool cancelDown = state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape));

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

void DrawPreview(ZHLN::Engine& engine, Session& session) {
    GUI::Context gui(engine);
    gui.BeginFrame(session.dt);
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
            (void) GUI::RenderUITree(gui, session.tree, session.actions, session.properties, GUI::TreeMode::Preview);
        }
    );
    gui.EndFrameAndRender(engine.GetRenderContext());
}

[[nodiscard]] auto PreviewIsRunning(const Session& session) -> bool {
#if defined(_WIN32)
    return session.previewProcess != nullptr;
#else
    return session.previewPid > 0;
#endif
}

void ReapPreview(Session& session) {
#if defined(_WIN32)
    if (session.previewProcess == nullptr) {
        return;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(session.previewProcess), &code) && code != STILL_ACTIVE) {
        CloseHandle(static_cast<HANDLE>(session.previewProcess));
        session.previewProcess = nullptr;
    }
#else
    if (session.previewPid <= 0) {
        return;
    }
    int status = 0;
    const pid_t waited = waitpid(static_cast<pid_t>(session.previewPid), &status, WNOHANG);
    if (waited == static_cast<pid_t>(session.previewPid)) {
        session.previewPid = -1;
    }
#endif
}

void StopPreview(Session& session) {
#if defined(_WIN32)
    if (session.previewProcess != nullptr) {
        TerminateProcess(static_cast<HANDLE>(session.previewProcess), 0);
        CloseHandle(static_cast<HANDLE>(session.previewProcess));
        session.previewProcess = nullptr;
    }
#else
    if (session.previewPid > 0) {
        kill(static_cast<pid_t>(session.previewPid), SIGTERM);
        waitpid(static_cast<pid_t>(session.previewPid), nullptr, 0);
        session.previewPid = -1;
    }
#endif
}

#if !defined(_WIN32)
extern char** environ;
#endif

void OpenPreview(Session& session) {
    if (session.executablePath.empty()) {
        ZHLN::Log("[UIEditor] Preview: no executable path to spawn");
        return;
    }

#if defined(ZHLN_HAS_UI_TOML)
    SaveTree(session.tree, kPreviewPath);
#else
    ZHLN::Log("[UIEditor] Preview needs extras/toml (rebuild with ZHLN_BUILD_EXTRAS)");
    return;
#endif

    if (PreviewIsRunning(session)) {
        ZHLN::Log("[UIEditor] Preview already open; wrote '{}'", kPreviewPath);
        return;
    }

#if defined(_WIN32)
    std::string cmd = "\"";
    cmd += session.executablePath;
    cmd += "\" --preview \"";
    cmd += kPreviewPath;
    cmd += "\"";
    STARTUPINFOA        si {};
    PROCESS_INFORMATION pi {};
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ZHLN::Log("[UIEditor] Preview CreateProcess failed ({})", static_cast<unsigned>(GetLastError()));
        return;
    }
    CloseHandle(pi.hThread);
    session.previewProcess = pi.hProcess;
#else
    char* args[] = {
        session.executablePath.data(),
        const_cast<char*>("--preview"),
        const_cast<char*>(kPreviewPath.data()),
        nullptr
    };
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, session.executablePath.c_str(), nullptr, nullptr, args, ::environ);
    if (rc != 0) {
        ZHLN::Log("[UIEditor] Preview spawn failed: {}", rc);
        return;
    }
    session.previewPid = static_cast<int>(pid);
#endif
    ZHLN::Log("[UIEditor] Preview window spawned");
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
    auto              parsed = ZHLN::ReflectTOML::TryParse<GUI::UINode>(text);
    if (!parsed) {
        ZHLN::Log("[UIEditor] '{}' did not parse as a UINode", path);
        return;
    }
    session.tree       = std::move(*parsed);
    session.selectedId = RootPath(session.tree);
    ZHLN::Log("[UIEditor] loaded '{}'", path);
}
#endif

void DrawFrame(ZHLN::Engine& engine, Session& session) {
    GUI::Context gui(engine);
    gui.SetClipboard(GUI::TextEdit::ClipboardSink {
        .userdata = &engine,
        .set      = [](void* ud, std::string_view text) -> void { static_cast<ZHLN::Engine*>(ud)->GetWindow().SetClipboardText(text); },
        .get      = [](void* ud) -> std::string { return static_cast<ZHLN::Engine*>(ud)->GetWindow().GetClipboardText(); },
    });

    auto* state = engine.GetRegistry().GetSingleton<ZHLN::Components::InputStateComponent>();
    const bool uiOwnsKeyboard = gui.IsTextInputFocused() || (state != nullptr && state->wantCaptureKeyboard);

    const bool xformWasActive = session.xform != XformMode::None;
    UpdateCanvasXform(gui, session, state, uiOwnsKeyboard);

    const bool escDown = state != nullptr && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape));
    if (escDown && !session.escWasDown && !uiOwnsKeyboard && !xformWasActive) {
        session.selectedId.clear();
    }
    session.escWasDown = escDown;

#if defined(ZHLN_HAS_UI_TOML)
    const bool controlDown = state != nullptr && (state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) ||
                                                  state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl)));
    const bool saveDown    = controlDown && state != nullptr && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S));
    if (saveDown && !session.saveWasDown && !uiOwnsKeyboard) {
        SaveTree(session.tree, kDocumentPath);
    }
    session.saveWasDown = saveDown;
#endif

    gui.BeginFrame(session.dt);
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
                            if (gui.Button("Preview", JPH::Vec4(0.16f, 0.24f, 0.36f, 0.95f))) {
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
                            if (gui.Button("Save", JPH::Vec4(0.16f, 0.30f, 0.20f, 0.95f))) {
                                SaveTree(session.tree, kDocumentPath);
                            }
                            if (gui.Button("Load", JPH::Vec4(0.16f, 0.24f, 0.36f, 0.95f))) {
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
    gui.EndFrameAndRender(engine.GetRenderContext());
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    bool        previewHost = false;
    std::string previewFile;
    std::vector<char*> cli;
    cli.reserve(static_cast<size_t>(argc));
    cli.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg {argv[i]};
        if (arg == "--preview") {
            previewHost = true;
            if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-') {
                previewFile = argv[++i];
            }
            continue;
        }
        cli.push_back(argv[i]);
    }

    auto optionsRes = ZHLN::HandleCommandLine(std::span(cli.data(), cli.size()));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();
    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128},
         .render =
             {.appName    = previewHost ? "UI Preview" : "Zahlen UI Editor",
              .width      = options.fullscreen ? 0u : (previewHost ? 800u : 1280u),
              .height     = options.fullscreen ? 0u : (previewHost ? 600u : 720u),
              .vsync      = options.vsync,
              .fullscreen = options.fullscreen,
              .validationMode = options.validationMode,
              .headless   = options.headless},
         .enableFallbackScene = false}
    );
    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    Session session;
    session.executablePath = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";
    session.tree           = MakeDemoTree();
    session.selectedId     = "panel";
    BindHostActions(session);
#if defined(ZHLN_HAS_UI_TOML)
    if (previewHost && !previewFile.empty()) {
        LoadTree(session, previewFile);
    }
#endif

    engine->SetGameState(&session);
    if (previewHost) {
        engine->SetUICallback([](ZHLN::Engine& eng) {
            auto* s = static_cast<Session*>(eng.GetGameState());
            if (s != nullptr) {
                DrawPreview(eng, *s);
            }
        });
    } else {
        engine->SetUICallback([](ZHLN::Engine& eng) {
            auto* s = static_cast<Session*>(eng.GetGameState());
            if (s != nullptr) {
                DrawFrame(eng, *s);
            }
        });
    }

    std::filesystem::file_time_type previewMTime {};
    bool                            havePreviewMTime = false;
#if defined(ZHLN_HAS_UI_TOML)
    if (previewHost && !previewFile.empty()) {
        std::error_code existsEc;
        if (std::filesystem::exists(previewFile, existsEc)) {
            std::error_code timeEc;
            previewMTime     = std::filesystem::last_write_time(previewFile, timeEc);
            havePreviewMTime = !timeEc;
        }
    }
#endif

    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        session.dt = clock.GetDeltaTime();
        engine->ProcessEvents();

        if (auto* st = engine->GetRegistry().GetSingleton<ZHLN::Components::InputStateComponent>(); st != nullptr && st->needsResize) {
            engine->GetRenderContext().SetResolution(st->newSize);
            st->needsResize = false;
            continue;
        }

        if (!previewHost) {
            ReapPreview(session);
            if (session.openPreviewRequested) {
                session.openPreviewRequested = false;
                OpenPreview(session);
            }
        }
#if defined(ZHLN_HAS_UI_TOML)
        else if (!previewFile.empty()) {
            std::error_code existsEc;
            if (std::filesystem::exists(previewFile, existsEc)) {
                std::error_code timeEc;
                const auto     written = std::filesystem::last_write_time(previewFile, timeEc);
                if (!timeEc && (!havePreviewMTime || written != previewMTime)) {
                    previewMTime     = written;
                    havePreviewMTime = true;
                    LoadTree(session, previewFile);
                }
            }
        }
#endif

        const auto status = engine->Tick(session.dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }
    }

    StopPreview(session);
    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
