// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/gui/UITree.hpp>
#include <cstddef>
#include <utility>

namespace ZHLN::GUI {

void ActionRegistry::Bind(std::string_view id, Handler handler) {
    if (id.empty() || !handler) {
        return;
    }
    for (auto& entry: _handlers) {
        if (entry.id == id) {
            entry.handler = std::move(handler);
            return;
        }
    }
    _handlers.push_back(Entry {.id = std::string {id}, .handler = std::move(handler)});
}

void ActionRegistry::Unbind(std::string_view id) {
    for (size_t i = 0; i < _handlers.size(); ++i) {
        if (_handlers[i].id == id) {
            _handlers.erase(_handlers.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void ActionRegistry::Invoke(std::string_view id) const {
    if (id.empty()) {
        return;
    }
    for (const auto& entry: _handlers) {
        if (entry.id == id && entry.handler) {
            entry.handler();
            return;
        }
    }
}

auto ActionRegistry::Contains(std::string_view id) const -> bool {
    if (id.empty()) {
        return false;
    }
    for (const auto& entry: _handlers) {
        if (entry.id == id) {
            return true;
        }
    }
    return false;
}

void ActionRegistry::Clear() {
    _handlers.clear();
}

void PropertyStore::SetBool(std::string_view path, bool value) {
    if (path.empty()) {
        return;
    }
    for (auto& existing: _values) {
        if (existing.path == path) {
            existing = Value {.path = existing.path, .kind = Kind::Bool, .b = value};
            return;
        }
    }
    _values.push_back(Value {.path = std::string {path}, .kind = Kind::Bool, .b = value});
}

void PropertyStore::SetFloat(std::string_view path, float value) {
    if (path.empty()) {
        return;
    }
    for (auto& existing: _values) {
        if (existing.path == path) {
            existing = Value {.path = existing.path, .kind = Kind::Float, .f = value};
            return;
        }
    }
    _values.push_back(Value {.path = std::string {path}, .kind = Kind::Float, .f = value});
}

void PropertyStore::SetString(std::string_view path, std::string_view value) {
    if (path.empty()) {
        return;
    }
    for (auto& existing: _values) {
        if (existing.path == path) {
            existing = Value {.path = existing.path, .kind = Kind::String, .s = std::string {value}};
            return;
        }
    }
    _values.push_back(Value {.path = std::string {path}, .kind = Kind::String, .s = std::string {value}});
}

auto PropertyStore::GetBool(std::string_view path, bool fallback) const -> bool {
    for (const auto& existing: _values) {
        if (existing.path == path) {
            return existing.kind == Kind::Bool ? existing.b : fallback;
        }
    }
    return fallback;
}

auto PropertyStore::GetFloat(std::string_view path, float fallback) const -> float {
    for (const auto& existing: _values) {
        if (existing.path == path) {
            return existing.kind == Kind::Float ? existing.f : fallback;
        }
    }
    return fallback;
}

auto PropertyStore::GetString(std::string_view path, std::string_view fallback) const -> std::string {
    for (const auto& existing: _values) {
        if (existing.path == path) {
            return existing.kind == Kind::String ? existing.s : std::string {fallback};
        }
    }
    return std::string {fallback};
}

auto PropertyStore::Contains(std::string_view path) const -> bool {
    for (const auto& existing: _values) {
        if (existing.path == path) {
            return true;
        }
    }
    return false;
}

void PropertyStore::Clear() {
    _values.clear();
}

namespace {

[[nodiscard]] auto NodeId(const UINode& node, std::string_view path) -> std::string {
    if (!node.id.empty()) {
        return node.id;
    }
    if (!path.empty()) {
        return std::string {path};
    }
    return "node";
}

[[nodiscard]] auto WidgetLabel(const UINode& node, std::string_view path) -> std::string {
    if (!node.label.empty()) {
        return node.label;
    }
    return NodeId(node, path);
}

/// Clay keys the hover/press state of Button/Slider by this string. The
/// display label is not unique (toolbar Save vs document Save), so widgets
/// use the node id. The Design wrap box keeps the bare id for hit-testing.
[[nodiscard]] auto WidgetKey(std::string_view id) -> std::string {
    std::string out;
    out.reserve(id.size() + 2);
    out.append(id);
    out.append("/w");
    return out;
}

[[nodiscard]] auto ChildPath(std::string_view parent, size_t index) -> std::string {
    std::string out;
    out.reserve(parent.size() + 8);
    out.append(parent);
    out.push_back('/');
    out.append(std::to_string(index));
    return out;
}

[[nodiscard]] auto RootPath(const UINode& root) -> std::string {
    return root.id.empty() ? std::string {"root"} : root.id;
}

[[nodiscard]] auto ToVec4(const JPH::Float4& c) -> JPH::Vec4 {
    return {c.x, c.y, c.z, c.w};
}

[[nodiscard]] auto ToBoxConfig(const NodeBox& box) -> BoxConfig {
    BoxConfig cfg;
    cfg.width        = box.width;
    cfg.height       = box.height;
    cfg.color        = ToVec4(box.color);
    cfg.cornerRadius = ToVec4(box.cornerRadius);
    cfg.padding      = box.padding;
    cfg.gap          = box.gap;
    cfg.direction    = box.direction;
    cfg.alignMain    = box.alignMain;
    cfg.alignCross   = box.alignCross;
    cfg.offsetX      = box.offsetX;
    cfg.offsetY      = box.offsetY;
    return cfg;
}

[[nodiscard]] auto BoxForKind(const UINode& node) -> BoxConfig {
    BoxConfig cfg = ToBoxConfig(node.box);
    if (node.kind == NodeKind::Row) {
        cfg.direction = Direction::Row;
    } else if (node.kind == NodeKind::Column) {
        cfg.direction = Direction::Column;
    }
    return cfg;
}

[[nodiscard]] auto IsSelected(std::string_view id, std::string_view selectedId) -> bool {
    return !selectedId.empty() && id == selectedId;
}

void ApplySelectionTint(BoxConfig& cfg) {
    cfg.color = cfg.color + JPH::Vec4(0.15f, 0.25f, 0.40f, 0.25f);
}

void NoteDesignClick(Context& gui, std::string_view id, TreeMode mode, RenderUITreeResult& result) {
    if (mode != TreeMode::Design || !result.clickedId.empty()) {
        return;
    }
    if (gui.IsPointerOver(id) && gui.IsPointerPressedThisFrame()) {
        result.clickedId = std::string {id};
    }
}

template <typename Node>
auto FindImpl(Node& node, std::string_view targetId, std::string_view path) -> Node* {
    if (targetId.empty()) {
        return nullptr;
    }
    const std::string id = NodeId(node, path);
    if (id == targetId) {
        return &node;
    }
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (auto* found = FindImpl(node.children[i], targetId, ChildPath(id, i))) {
            return found;
        }
    }
    return nullptr;
}

auto RemoveImpl(UINode& node, std::string_view targetId, std::string_view path) -> bool {
    const std::string id = NodeId(node, path);
    for (size_t i = 0; i < node.children.size(); ++i) {
        const std::string childPath = ChildPath(id, i);
        if (NodeId(node.children[i], childPath) == targetId) {
            node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        if (RemoveImpl(node.children[i], targetId, childPath)) {
            return true;
        }
    }
    return false;
}

void RenderNode(
    Context&              gui,
    const UINode&         node,
    const ActionRegistry& actions,
    PropertyStore&        properties,
    TreeMode              mode,
    std::string_view      path,
    std::string_view      selectedId,
    RenderUITreeResult&   result
) {
    const std::string id    = NodeId(node, path);
    const std::string label = WidgetLabel(node, path);
    const bool        selected = mode == TreeMode::Design && IsSelected(id, selectedId);

    auto renderChildren = [&]() -> void {
        for (size_t i = 0; i < node.children.size(); ++i) {
            RenderNode(gui, node.children[i], actions, properties, mode, ChildPath(id, i), selectedId, result);
        }
    };

    auto wrapLeaf = [&](auto&& draw) -> void {
        if (mode != TreeMode::Design) {
            draw();
            return;
        }
        BoxConfig hit;
        if (selected) {
            hit.color   = {0.15f, 0.25f, 0.40f, 0.35f};
            hit.padding = 2.0f;
        }
        gui.Box(id, hit, draw);
    };

    switch (node.kind) {
        case NodeKind::Box:
        case NodeKind::Row:
        case NodeKind::Column: {
            BoxConfig cfg = BoxForKind(node);
            if (selected) {
                ApplySelectionTint(cfg);
            }
            gui.Box(id, cfg, renderChildren);
            NoteDesignClick(gui, id, mode, result);
            break;
        }

        case NodeKind::Text:
            wrapLeaf([&]() { gui.Text(label, node.fontSize, ToVec4(node.textColor)); });
            NoteDesignClick(gui, id, mode, result);
            break;

        case NodeKind::Button:
            wrapLeaf([&]() {
                if (gui.Button(label, {0.16f, 0.24f, 0.36f, 0.95f}, {}, WidgetKey(id))) {
                    result.clickedId = id;
                    if (mode == TreeMode::Preview && !node.onClickAction.empty() && actions.Contains(node.onClickAction)) {
                        actions.Invoke(node.onClickAction);
                        result.actionInvoked = true;
                    }
                }
            });
            NoteDesignClick(gui, id, mode, result);
            break;

        case NodeKind::Checkbox: {
            bool value = properties.GetBool(node.bindProperty);
            wrapLeaf([&]() {
                if (gui.Checkbox(label, value, WidgetKey(id))) {
                    result.clickedId = id;
                    if (mode == TreeMode::Preview && !node.bindProperty.empty()) {
                        properties.SetBool(node.bindProperty, value);
                    }
                }
            });
            NoteDesignClick(gui, id, mode, result);
            break;
        }

        case NodeKind::Slider: {
            float value = properties.GetFloat(node.bindProperty);
            wrapLeaf([&]() {
                if (gui.Slider(label, value, node.minVal, node.maxVal, WidgetKey(id)) && mode == TreeMode::Preview && !node.bindProperty.empty()) {
                    properties.SetFloat(node.bindProperty, value);
                }
            });
            NoteDesignClick(gui, id, mode, result);
            break;
        }

        case NodeKind::TextInput: {
            std::string value = properties.GetString(node.bindProperty);
            wrapLeaf([&]() {
                if (gui.TextInput(label, value, {}, WidgetKey(id)) && mode == TreeMode::Preview && !node.bindProperty.empty()) {
                    properties.SetString(node.bindProperty, value);
                }
            });
            NoteDesignClick(gui, id, mode, result);
            break;
        }
    }
}

} // namespace

auto FindNodeById(UINode& root, std::string_view targetId) -> UINode* {
    return FindImpl(root, targetId, RootPath(root));
}

auto FindNodeById(const UINode& root, std::string_view targetId) -> const UINode* {
    return FindImpl(root, targetId, RootPath(root));
}

auto InsertChild(UINode& root, std::string_view parentId, UINode child) -> bool {
    UINode* parent = FindNodeById(root, parentId);
    if (parent == nullptr) {
        return false;
    }
    parent->children.push_back(std::move(child));
    return true;
}

auto RemoveNodeById(UINode& root, std::string_view targetId) -> bool {
    if (targetId.empty() || NodeId(root, RootPath(root)) == targetId) {
        return false;
    }
    return RemoveImpl(root, targetId, RootPath(root));
}

auto RenderUITree(
    Context&              gui,
    const UINode&         root,
    const ActionRegistry& actions,
    PropertyStore&        properties,
    TreeMode              mode,
    std::string_view      selectedId
) -> RenderUITreeResult {
    RenderUITreeResult result;
    RenderNode(gui, root, actions, properties, mode, RootPath(root), selectedId, result);
    return result;
}

} // namespace ZHLN::GUI
