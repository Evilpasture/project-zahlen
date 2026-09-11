// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// Data-driven UI trees.
//
// GUI::Context is immediate-mode: a C++ host names widgets and passes
// callbacks and mutable references. A document cannot store a function pointer
// or a C++ reference, so a tree that can be loaded (and later authored in a
// builder) talks to the widgets through two tables the host owns:
//
//   * ActionRegistry  -- "editor.save_scene" -> a typed event on an ECS bus
//   * PropertyStore   -- "camera.speed"      -> the float a Slider edits
//
// UINode is the description. It is format-free, the same way Scene::Scene is:
// extras/toml can walk it later without this header knowing a parser exists.
// RenderUITree is the walk that turns one description into one frame of
// Context calls. FindNodeById / InsertChild / RemoveNodeById are the mutations
// a builder needs once Design mode has handed it a clickedId.
// ===========================================================================

#include <Jolt/Jolt.h>
#include <Jolt/Math/Float4.h>
#include <Zahlen/Common.h>
#include <Zahlen/ecs/EventBus.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::GUI {

enum class NodeKind : uint8_t { Box = 0, Row, Column, Text, Button, Checkbox, Slider, TextInput };

/// Preview runs actions and writes bound properties. Design intercepts clicks
/// so a builder can select a node instead of firing "Save".
enum class TreeMode : uint8_t { Preview = 0, Design };

/// Layout knobs for a UINode. Same fields as BoxConfig, but colours are Jolt
/// storage vectors so a document can say `color = [r, g, b, a]` rather than a
/// table of SIMD lanes.
struct NodeBox {
    Sizing      width        = {};
    Sizing      height       = {};
    JPH::Float4 color        = {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4 cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f};
    float       padding      = 0.0f;
    float       gap          = 0.0f;
    Direction   direction    = Direction::Column;
    Alignment   alignMain    = Alignment::Start;
    Alignment   alignCross   = Alignment::Start;
    /// Pixel offset from the parent box. The UI editor's G grab writes these.
    float       offsetX      = 0.0f;
    float       offsetY      = 0.0f;
    /// Clockwise degrees around the box centre. The UI editor's R rotate writes
    /// this (15° snap). Clay has no rotation, so it is document state for now.
    float       rotation     = 0.0f;
};

/// One widget in a layout tree. Defaults match BoxConfig / widget defaults so
/// a document can name only what differs.
struct UINode {
    std::string id;
    NodeKind    kind  = NodeKind::Box;
    std::string label;
    /// ActionRegistry key whose bound event is pushed when this Button is
    /// clicked in Preview.
    std::string onClickAction;
    /// PropertyStore path a Checkbox / Slider / TextInput reads and writes.
    std::string bindProperty;

    NodeBox     box {};
    float       fontSize  = 16.0f;
    JPH::Float4 textColor = {1.0f, 1.0f, 1.0f, 1.0f};
    float       minVal    = 0.0f;
    float       maxVal    = 1.0f;

    std::vector<UINode> children;
};

/// Pushed when Bind(id) has no typed payload. Hosts Drain this and switch on
/// `id`, or Bind a strongly-typed event instead:
///
///   actions.Bind("inventory.use_potion", UseItemEvent { .itemId = 42, .target = player });
///   bus.Drain<UseItemEvent>([](const UseItemEvent& e) { ... });
struct UiActionEvent {
    std::string id;
};

/// Name -> event prototype. The document stores the identifier; Bind copies a
/// typed event that Invoke pushes onto an ECS::EventBus. Missing ids are a
/// no-op, not an error: a document can name an action this host has not
/// installed. No std::function -- gameplay reacts by draining the bus.
class ZHLN_API ActionRegistry {
  public:
    ActionRegistry() = default;
    explicit ActionRegistry(ECS::EventBus& bus) noexcept: _bus(&bus) {}
    ~ActionRegistry();

    ActionRegistry(const ActionRegistry&)                    = delete;
    auto operator=(const ActionRegistry&) -> ActionRegistry& = delete;
    ActionRegistry(ActionRegistry&& other) noexcept;
    auto operator=(ActionRegistry&& other) noexcept -> ActionRegistry&;

    void SetEventBus(ECS::EventBus& bus) noexcept {
        _bus = &bus;
    }

    /// Invoke pushes UiActionEvent { id }.
    void Bind(std::string_view id);

    /// Invoke copies @p event onto the bus.
    template <typename T>
    void Bind(std::string_view id, T event) {
        BindErased(
            id,
            new T(std::move(event)),
            [](ECS::EventBus& bus, const void* p) -> void { bus.Push(*static_cast<const T*>(p)); },
            [](void* p) -> void { delete static_cast<T*>(p); }
        );
    }

    void Unbind(std::string_view id);
    /// True when a bound event was pushed. False for missing ids or no bus.
    [[nodiscard]] auto Invoke(std::string_view id) const -> bool;
    [[nodiscard]] auto Contains(std::string_view id) const -> bool;
    void               Clear();

  private:
    struct Entry {
        std::string id;
        void*       payload = nullptr;
        void (*emit)(ECS::EventBus&, const void*) = nullptr;
        void (*destroy)(void*)                    = nullptr;
    };

    void BindErased(std::string_view id, void* payload, void (*emit)(ECS::EventBus&, const void*), void (*destroy)(void*));
    void DestroyEntry(Entry& entry) noexcept;

    ECS::EventBus*     _bus = nullptr;
    std::vector<Entry> _entries;
};

/// Host value table for widgets that take a mutable reference.
class ZHLN_API PropertyStore {
  public:
    void SetBool(std::string_view path, bool value);
    void SetFloat(std::string_view path, float value);
    void SetString(std::string_view path, std::string_view value);

    [[nodiscard]] auto GetBool(std::string_view path, bool fallback = false) const -> bool;
    [[nodiscard]] auto GetFloat(std::string_view path, float fallback = 0.0f) const -> float;
    [[nodiscard]] auto GetString(std::string_view path, std::string_view fallback = {}) const -> std::string;

    [[nodiscard]] auto Contains(std::string_view path) const -> bool;
    void               Clear();

  private:
    enum class Kind : uint8_t { None = 0, Bool, Float, String };
    struct Value {
        std::string path;
        Kind        kind = Kind::None;
        bool        b    = false;
        float       f    = 0.0f;
        std::string s;
    };
    std::vector<Value> _values;
};

struct RenderUITreeResult {
    /// Node id (or generated path) that was clicked this frame, if any.
    std::string clickedId;
    /// True when Preview mode pushed a bound action onto the event bus.
    bool actionInvoked = false;
};

/// Resolves a Design-mode clickedId (a node.id, or a generated path like
/// "panel/0/1" when the node has no id) to the live node. Null when missing.
[[nodiscard]] ZHLN_API auto FindNodeById(UINode& root, std::string_view targetId) -> UINode*;
[[nodiscard]] ZHLN_API auto FindNodeById(const UINode& root, std::string_view targetId) -> const UINode*;

/// Appends @p child under the node named by @p parentId. False if the parent
/// is not in the tree.
[[nodiscard]] ZHLN_API auto InsertChild(UINode& root, std::string_view parentId, UINode child) -> bool;

/// Removes the named node and its subtree. The root itself cannot be removed.
[[nodiscard]] ZHLN_API auto RemoveNodeById(UINode& root, std::string_view targetId) -> bool;

/// Walks @p root once, issuing Context calls. Must run between BeginFrame and
/// EndFrame / EndFrameAndRender. @p properties is non-const because bound
/// widgets write back; pass a dummy store when the tree has no bindings.
/// @p selectedId is tinted in Design mode so a builder can see the selection.
[[nodiscard]] ZHLN_API auto RenderUITree(
    Context&              gui,
    const UINode&         root,
    const ActionRegistry& actions,
    PropertyStore&        properties,
    TreeMode              mode       = TreeMode::Preview,
    std::string_view      selectedId = {}
) -> RenderUITreeResult;

} // namespace ZHLN::GUI
