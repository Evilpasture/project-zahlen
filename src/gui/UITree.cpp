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

[[nodiscard]] auto ChildPath(std::string_view parent, size_t index) -> std::string {
    std::string out;
    out.reserve(parent.size() + 8);
    out.append(parent);
    out.push_back('/');
    out.append(std::to_string(index));
    return out;
}

[[nodiscard]] auto BoxForKind(const UINode& node) -> BoxConfig {
    BoxConfig cfg = node.box;
    if (node.kind == NodeKind::Row) {
        cfg.direction = Direction::Row;
    } else if (node.kind == NodeKind::Column) {
        cfg.direction = Direction::Column;
    }
    return cfg;
}

void RenderNode(
    Context&              gui,
    const UINode&         node,
    const ActionRegistry& actions,
    PropertyStore&        properties,
    TreeMode              mode,
    std::string_view      path,
    RenderUITreeResult&   result
) {
    const std::string id    = NodeId(node, path);
    const std::string label = WidgetLabel(node, path);

    auto renderChildren = [&]() -> void {
        for (size_t i = 0; i < node.children.size(); ++i) {
            RenderNode(gui, node.children[i], actions, properties, mode, ChildPath(id, i), result);
        }
    };

    switch (node.kind) {
        case NodeKind::Box:
        case NodeKind::Row:
        case NodeKind::Column:
            gui.Box(id, BoxForKind(node), renderChildren);
            break;

        case NodeKind::Text:
            gui.Text(label, node.fontSize, node.textColor);
            break;

        case NodeKind::Button:
            if (gui.Button(label)) {
                result.clickedId = id;
                if (mode == TreeMode::Preview && !node.onClickAction.empty() && actions.Contains(node.onClickAction)) {
                    actions.Invoke(node.onClickAction);
                    result.actionInvoked = true;
                }
            }
            break;

        case NodeKind::Checkbox: {
            bool value = properties.GetBool(node.bindProperty);
            if (gui.Checkbox(label, value)) {
                result.clickedId = id;
                if (mode == TreeMode::Preview && !node.bindProperty.empty()) {
                    properties.SetBool(node.bindProperty, value);
                }
            }
            break;
        }

        case NodeKind::Slider: {
            float value = properties.GetFloat(node.bindProperty);
            if (gui.Slider(label, value, node.minVal, node.maxVal) && mode == TreeMode::Preview && !node.bindProperty.empty()) {
                properties.SetFloat(node.bindProperty, value);
            }
            break;
        }

        case NodeKind::TextInput: {
            std::string value = properties.GetString(node.bindProperty);
            if (gui.TextInput(label, value) && mode == TreeMode::Preview && !node.bindProperty.empty()) {
                properties.SetString(node.bindProperty, value);
            }
            break;
        }
    }
}

} // namespace

auto RenderUITree(Context& gui, const UINode& root, const ActionRegistry& actions, PropertyStore& properties, TreeMode mode)
    -> RenderUITreeResult {
    RenderUITreeResult result;
    const std::string  path = root.id.empty() ? "root" : root.id;
    RenderNode(gui, root, actions, properties, mode, path, result);
    return result;
}

} // namespace ZHLN::GUI
