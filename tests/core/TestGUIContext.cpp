// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/TestGUIContext.cpp
//
// Stress & regression tests for the immediate-mode native GUI (Zahlen/GUI.hpp):
//
//   - Entity cache reuse across frames (widgets must NOT respawn per frame).
//   - Mark-and-sweep GC: stale widgets are destroyed AND their cache records
//     erased. Without the erase, every dynamically-keyed widget (changing
//     labels, tree rows, ...) leaks one record per frame and every sweep walks
//     all of them - the classic "deleting lags the runtime" pattern.
//   - Recursive subtree destruction via DestroyUIEntity.
//   - Externally-destroyed (orphaned) widget entities are respawned cleanly
//     instead of leaving dead references behind.
//   - Hierarchies deeper than GUI::MAX_UI_STACK_DEPTH degrade gracefully:
//     creation continues against the deepest live parent (the typed error is
//     latched into Status(), not logged), and a later sweep still reclaims
//     the whole tree.
//   - Hybrid RAII + closure API: Panel()/Box() return [[nodiscard]] UIScope
//     guards whose destruction pops AND sweeps the container; the closure
//     overloads are sugar on top. Push/pop are private, the root-cache
//     accessor is private (tests derive it from UISettingsComponent), and
//     ~Context() itself sweeps the root cache at frame end.

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

namespace {

using ZHLN::Entity;
using ZHLN::ECS::Registry;
using ZHLN::GUI::GUIError;
namespace GUI = ZHLN::GUI;
using Comp    = ZHLN::Components; // NB: nested types of a struct, not a namespace

[[nodiscard]] auto CountUIRects(Registry& reg) -> size_t {
    return reg.GetEntitiesWith<Comp::UIRectComponent>().size();
}

[[nodiscard]] auto CountTotalCacheRecords(Registry& reg) -> size_t {
    size_t total = 0;
    for (Entity e: reg.GetEntitiesWith<Comp::UIChildCacheComponent>()) {
        if (const auto* cache = reg.Get<Comp::UIChildCacheComponent>(e)) {
            total += cache->children.Size();
        }
    }
    return total;
}

[[nodiscard]] auto CountCacheRecordsOn(Registry& reg, Entity e) -> size_t {
    if (const auto* cache = reg.Get<Comp::UIChildCacheComponent>(e)) {
        return cache->children.Size();
    }
    return 0;
}

// GUI::Context keeps the cache root private by design; the tests derive it as
// "the UISettings entity", which the builder's fallback creates on first use.
// NOTE: on a pristine registry this only resolves AFTER the first build.
[[nodiscard]] auto RootCacheEntity(Registry& reg) -> Entity {
    const auto settings = reg.GetEntitiesWith<Comp::UISettingsComponent>();
    return settings.empty() ? Entity::Null() : settings[0];
}

} // namespace

struct GUIContextTestSuite {
    struct Tests {
        // ------------------------------------------------------------------
        // Rebuilding the identical tree next frame must reuse entities, never
        // respawn them (respawning per frame is what makes UIs flicker).
        // ------------------------------------------------------------------
        auto widget_entities_are_reused_across_frames() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            Entity root1 = Entity::Null();
            Entity boxA1 = Entity::Null();
            Entity boxB1 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                root1 = gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                    boxA1 = gui.Box("a", GUI::BoxConfig {}, []() -> void {});
                    boxB1 = gui.Box("b", GUI::BoxConfig {}, []() -> void {});
                });
                gui.SweepStaleChildren(Entity::Null()); // frame-1 no-op sweep
            }

            Entity root2          = Entity::Null();
            Entity boxA2          = Entity::Null();
            Entity boxB2          = Entity::Null();
            bool   buildSucceeded = false;
            {
                GUI::Context gui(reg, 2);
                root2 = gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                    boxA2 = gui.Box("a", GUI::BoxConfig {}, []() -> void {});
                    boxB2 = gui.Box("b", GUI::BoxConfig {}, []() -> void {});
                });
                gui.SweepStaleChildren(Entity::Null());    // clean: nothing is stale on an identical rebuild
                buildSucceeded = gui.Status().has_value(); // clean build: engaged expected, not an error code
            }

            ZHLN::Test::ExpectEq(root1.Pack(), root2.Pack());
            ZHLN::Test::ExpectEq(boxA1.Pack(), boxA2.Pack());
            ZHLN::Test::ExpectEq(boxB1.Pack(), boxB2.Pack());
            ZHLN::Test::ExpectEq(CountUIRects(reg), 3u);
            ZHLN::Test::ExpectTrue(buildSucceeded);

            return {};
        }

        // ------------------------------------------------------------------
        // A widget the parent stops rebuilding is destroyed during the parent's
        // own sweep - immediately when the Panel scope closes, not "sometime".
        // ------------------------------------------------------------------
        auto stale_child_is_destroyed_when_parent_rebuilds_without_it() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            Entity boxB = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                    gui.Box("a", GUI::BoxConfig {}, []() -> void {});
                    boxB = gui.Box("b", GUI::BoxConfig {}, []() -> void {});
                });
            }

            Entity root = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                root = gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                    gui.Box("a", GUI::BoxConfig {}, []() -> void {});
                    // "b" is no longer rebuilt -> swept before the Panel closure returns.
                });
            }

            ZHLN::Test::ExpectTrue(boxB != Entity::Null());
            ZHLN::Test::ExpectFalse(reg.IsAlive(boxB));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 2u);
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, root), 1u); // only "a" survives - no stale record for "b"

            return {};
        }

        // ------------------------------------------------------------------
        // THE lag repro: a parent that keeps creating uniquely-keyed children
        // (dynamic rows / changing labels). Every frame one child dies and one
        // is born. If swept records are not erased from the HashMap, the cache
        // grows by 1/frame and every future sweep walks all of them.
        // ------------------------------------------------------------------
        auto dynamic_widget_keys_do_not_leak_cache_records() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            constexpr uint64_t kFrames = 50;
            Entity             root    = Entity::Null();

            for (uint64_t frame = 1; frame <= kFrames; ++frame) {
                GUI::Context gui(reg, frame);
                root = gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                    gui.Box("stable", GUI::BoxConfig {}, []() -> void {});
                    gui.Box("dynamic_" + std::to_string(frame), GUI::BoxConfig {}, []() -> void {});
                });
            }

            // Final frame holds: root panel + "stable" + this frame's dynamic row.
            ZHLN::Test::ExpectEq(CountUIRects(reg), 3u);
            // Cache must contain exactly the two live children - NOT 1+50 records
            // (the leak) and not 49 stale tombstones from previous frames.
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, root), 2u);
            ZHLN::Test::ExpectEq(CountTotalCacheRecords(reg), 3u); // two child records + root-panel record at the root cache

            return {};
        }

        // ------------------------------------------------------------------
        // Root-level sweep: a panel never rebuilt gets destroyed and its record
        // purged from the root cache entity.
        // ------------------------------------------------------------------
        auto root_sweep_removes_unvisited_panels_and_purges_records() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            Entity panel2 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("p1", GUI::PanelConfig {}, []() -> void {});
                panel2 = gui.Panel("p2", GUI::PanelConfig {}, []() -> void {});
                gui.SweepStaleChildren(Entity::Null()); // frame-1 no-op sweep
            }

            Entity rootCache = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                rootCache = RootCacheEntity(reg); // created during the frame-1 build
                gui.Panel("p1", GUI::PanelConfig {}, []() -> void {});

                // "p2" was not rebuilt this frame: the sweep collects it, and
                // WHAT it did is verified through registry state below.
                gui.SweepStaleChildren(Entity::Null());
            }

            ZHLN::Test::ExpectTrue(panel2 != Entity::Null());
            ZHLN::Test::ExpectFalse(reg.IsAlive(panel2));
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, rootCache), 1u); // only "p1" remains recorded

            return {};
        }

        // ------------------------------------------------------------------
        // Sweeping a nested child panel must destroy its whole subtree, not
        // leave its labels/boxes orphaned in the registry.
        // ------------------------------------------------------------------
        auto nested_sweep_destroys_child_panel_and_its_contents() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            Entity childPanel = Entity::Null();
            Entity childLabel = Entity::Null();
            Entity childBox   = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                    childPanel = gui.Panel("nested", GUI::PanelConfig {}, [&]() -> void {
                        childLabel = gui.Label("hello");
                        childBox   = gui.Box("inner", GUI::BoxConfig {}, []() -> void {});
                    });
                });
            }

            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {}, []() -> void {}); // "nested" no longer rebuilt
            }

            ZHLN::Test::ExpectFalse(reg.IsAlive(childPanel));
            ZHLN::Test::ExpectFalse(reg.IsAlive(childLabel));
            ZHLN::Test::ExpectFalse(reg.IsAlive(childBox));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 1u);           // root alone
            ZHLN::Test::ExpectEq(CountTotalCacheRecords(reg), 1u); // root's record at the root cache

            return {};
        }

        // ------------------------------------------------------------------
        // DestroyUIEntity removes an entire subtree on demand; the (now dead)
        // record the root cache still holds is purged by the next sweep.
        // ------------------------------------------------------------------
        auto destroy_ui_entity_removes_whole_subtree() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   root  = Entity::Null();
            Entity   boxB  = Entity::Null();
            Entity   label = Entity::Null();
            Entity   boxC  = Entity::Null();

            GUI::Context gui(reg, 1);
            root = gui.Panel("root", GUI::PanelConfig {}, [&]() -> void {
                boxB = gui.Box("b", GUI::BoxConfig {}, [&]() -> void { label = gui.Label("inside"); });
                boxC = gui.Box("c", GUI::BoxConfig {}, []() -> void {});
            });
            // Derived after the build above: the first widget creates the root cache.
            const Entity rootCache = RootCacheEntity(reg);

            const auto destroyResult = gui.DestroyUIEntity(root);
            ZHLN::Test::ExpectTrue(destroyResult.has_value()); // success: engaged expected, silent

            ZHLN::Test::ExpectFalse(reg.IsAlive(root));
            ZHLN::Test::ExpectFalse(reg.IsAlive(boxB));
            ZHLN::Test::ExpectFalse(reg.IsAlive(label));
            ZHLN::Test::ExpectFalse(reg.IsAlive(boxC));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 0u);

            // Destroying the same entity again is a typed failure, not a log.
            const auto destroyAgain = gui.DestroyUIEntity(root);
            ZHLN::Test::ExpectFalse(destroyAgain.has_value());
            if (!destroyAgain.has_value()) {
                ZHLN::Test::ExpectTrue(destroyAgain.error().Is(GUIError::EntityNotAlive));
            }

            // Root's own record lives in the root cache entity and points at a
            // destroyed entity now: the sweep must purge it, not keep it around
            // (this is the orphaned-record path). Exactly one purge, no destroys.
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, rootCache), 1u);
            gui.SweepStaleChildren(Entity::Null()); // purges the dead record
            // The orphaned record is gone afterwards.
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, rootCache), 0u);
            ZHLN::Test::ExpectEq(CountTotalCacheRecords(reg), 0u);

            return {};
        }

        // ------------------------------------------------------------------
        // A widget destroyed externally (plain Registry::Destroy, NOT
        // DestroyUIEntity) gets respawned on the next rebuild instead of
        // leaving a dead cached reference behind. The cache must not duplicate
        // the record either.
        // ------------------------------------------------------------------
        auto externally_destroyed_widget_respawns_next_frame() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            Entity boxA1 = Entity::Null();
            Entity root  = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                root = gui.Panel("root", GUI::PanelConfig {}, [&]() -> void { boxA1 = gui.Box("a", GUI::BoxConfig {}, []() -> void {}); });
            }
            reg.Destroy(boxA1); // the external-mistake path

            Entity boxA2 = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {}, [&]() -> void { boxA2 = gui.Box("a", GUI::BoxConfig {}, []() -> void {}); });
            }

            ZHLN::Test::ExpectFalse(reg.IsAlive(boxA1));
            ZHLN::Test::ExpectTrue(reg.IsAlive(boxA2));
            ZHLN::Test::ExpectNe(boxA1.Pack(), boxA2.Pack()); // fresh entity (new generation), same key
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, root), 1u);
            ZHLN::Test::ExpectEq(CountUIRects(reg), 2u);

            return {};
        }

        // ------------------------------------------------------------------
        // Deeper than MAX_UI_STACK_DEPTH: creation must not crash or spin;
        // widgets past the cap attach to the deepest LIVE parent (documented
        // degradation), and a root sweep still reclaims the entire tree.
        // ------------------------------------------------------------------
        auto hierarchy_deeper_than_stack_cap_degrades_but_survives() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            constexpr size_t               kDepth = GUI::MAX_UI_STACK_DEPTH + 8;
            std::array<Entity, kDepth + 1> boxes {}; // 1 index-based for readability (boxes[1] == outermost)

            bool statusIsError     = false;
            bool statusCodeIsRight = false;
            {
                GUI::Context                gui(reg, 1);
                std::function<void(size_t)> build = [&](size_t level) -> void {
                    if (level > kDepth) {
                        return;
                    }
                    boxes[level] = gui.Box(GUI::BoxConfig {.height = 4.0f}, [&]() -> void { build(level + 1); });
                };
                build(1);

                // Overflowing the scope stack is a typed, queryable failure -
                // not a log line. The build itself still completed above.
                const auto status = gui.Status();
                statusIsError     = !status.has_value();
                statusCodeIsRight = (!status.has_value()) && status.error().Is(GUIError::HierarchyTooDeep);
            }

            ZHLN::Test::ExpectTrue(statusIsError);
            ZHLN::Test::ExpectTrue(statusCodeIsRight);

            // Everything was actually created: no loss, no infinite loop.
            ZHLN::Test::ExpectEq(CountUIRects(reg), kDepth);

            const auto* rectAtCap    = reg.Get<Comp::UIRectComponent>(boxes[GUI::MAX_UI_STACK_DEPTH]);
            const auto* rectPastCap1 = reg.Get<Comp::UIRectComponent>(boxes[GUI::MAX_UI_STACK_DEPTH + 1]);
            const auto* rectPastCap2 = reg.Get<Comp::UIRectComponent>(boxes[GUI::MAX_UI_STACK_DEPTH + 2]);
            const auto* rectDeepest  = reg.Get<Comp::UIRectComponent>(boxes[kDepth]);

            ZHLN::Test::ExpectTrue(rectAtCap != nullptr && rectPastCap1 != nullptr && rectPastCap2 != nullptr && rectDeepest != nullptr);
            if (rectAtCap == nullptr || rectPastCap1 == nullptr || rectPastCap2 == nullptr || rectDeepest == nullptr) {
                return {};
            }

            // Widget exactly at the cap still parents normally...
            ZHLN::Test::ExpectEq(rectAtCap->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH - 1].Pack());
            // ...everything below it flattens onto the deepest LIVE parent
            // (the last widget whose scope push succeeded).
            ZHLN::Test::ExpectEq(rectPastCap1->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH].Pack());
            ZHLN::Test::ExpectEq(rectPastCap2->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH].Pack());
            ZHLN::Test::ExpectEq(rectDeepest->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH].Pack());
            // Depth stops growing past the cap (no unbounded counter).
            ZHLN::Test::ExpectEq(rectDeepest->hierarchyDepth, rectPastCap1->hierarchyDepth);

            // Whole tree - including the misattached tail - is reclaimed by a
            // single root sweep: exactly one destroyed subtree, no orphans.
            {
                GUI::Context gui(reg, 2);
                gui.SweepStaleChildren(Entity::Null());
            }
            ZHLN::Test::ExpectEq(CountUIRects(reg), 0u);
            ZHLN::Test::ExpectEq(CountTotalCacheRecords(reg), 0u);

            return {};
        }

        // ------------------------------------------------------------------
        // Labels are keyed by their text by design: changing the text swaps the
        // entity. Document the behaviour so the sweep keeps cleanup predictable
        // for dynamic text (and so nobody "fixes" it by accident).
        // ------------------------------------------------------------------
        auto label_text_change_replaces_entity_and_sweeps_old_one() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            Entity label1 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {}, [&]() -> void { label1 = gui.Label("count 1"); });
            }

            Entity label2 = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {}, [&]() -> void { label2 = gui.Label("count 2"); });
            }

            ZHLN::Test::ExpectFalse(reg.IsAlive(label1));
            ZHLN::Test::ExpectTrue(reg.IsAlive(label2));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 2u);

            return {};
        }

        // ------------------------------------------------------------------
        // CHECKBOX: entity reuse across frames, boolean out-param reflects
        // the ECS value, and inner visual children (box + mark + label) are
        // cached rather than respawned.
        // ------------------------------------------------------------------
        auto checkbox_reuses_entity_and_its_visual_children() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            bool value = false;
            Entity cb1 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                cb1 = gui.Checkbox("vsync", "Vsync", value);
            }
            size_t rectsAfter1 = CountUIRects(reg);
            ZHLN::Test::ExpectTrue(rectsAfter1 >= 4u); // root + box + mark + label at minimum

            value = true; // flip out-param externally before frame 2
            Entity cb2 = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                cb2 = gui.Checkbox("vsync", "Vsync", value);
            }

            ZHLN::Test::ExpectEq(cb1.Pack(), cb2.Pack());
            // External value change is accepted into the component
            const auto* cbComp = reg.Get<Comp::UICheckboxComponent>(cb2);
            ZHLN::Test::ExpectTrue(cbComp != nullptr);
            if (cbComp != nullptr) {
                ZHLN::Test::ExpectTrue(cbComp->checked);
            }
            // No child entity respawning (stable count)
            ZHLN::Test::ExpectEq(CountUIRects(reg), rectsAfter1);

            return {};
        }

        // ------------------------------------------------------------------
        // SLIDER: entity reuse, float out-param clamped into [min,max], and
        // visual children (track + knob + optional label + value) are cached.
        // ------------------------------------------------------------------
        auto slider_reuses_entity_and_clamps_value() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            float value = 1.5f; // above max — must clamp on first frame
            Entity sl1 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                sl1 = gui.Slider("gamma", "Gamma", value, 0.0f, 1.0f, 0.01f);
            }
            ZHLN::Test::ExpectTrue(value <= 1.0f + 1e-5f); // clamped out

            value = 0.3f; // external programmatic change
            Entity sl2 = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                sl2 = gui.Slider("gamma", "Gamma", value, 0.0f, 1.0f, 0.01f);
            }
            ZHLN::Test::ExpectEq(sl1.Pack(), sl2.Pack());
            const auto* slComp = reg.Get<Comp::UISliderComponent>(sl2);
            ZHLN::Test::ExpectTrue(slComp != nullptr);
            if (slComp != nullptr) {
                ZHLN::Test::ExpectTrue(std::abs(slComp->value - 0.3f) < 1e-5f);
                ZHLN::Test::ExpectTrue(std::abs(slComp->minValue - 0.0f) < 1e-5f);
                ZHLN::Test::ExpectTrue(std::abs(slComp->maxValue - 1.0f) < 1e-5f);
                ZHLN::Test::ExpectTrue(std::abs(slComp->step - 0.01f) < 1e-5f);
                ZHLN::Test::ExpectFalse(slComp->isDragging); // idle
            }

            return {};
        }

        // ------------------------------------------------------------------
        // TEXTINPUT: entity reuse, string out-param syncs from the ECS
        // component, and the editable text lives on a LEAF child (no children
        // of its own) so Yoga never has to attach a measure function to a
        // node that already has children (this was the original Yoga crash).
        // Regression guard for "Nodes with measure functions cannot have
        // children".
        // ------------------------------------------------------------------
        auto textinput_has_no_text_on_root_and_leaf_text_child() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            std::string value = "hello";
            Entity ti1 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                ti1 = gui.TextInput("name", "Name", value);
            }
            // Root must NOT carry the TextComponent (that was the Yoga crash
            // trigger: measure func + children), but must still hold the
            // UITextInputComponent for the engine's key handler.
            ZHLN::Test::ExpectTrue(reg.Get<Comp::UITextInputComponent>(ti1) != nullptr);
            // The editable text leaf child must exist and be a leaf.
            Entity textLeaf = Entity::Null();
            if (const auto* cache = reg.Get<Comp::UIChildCacheComponent>(ti1)) {
                cache->children.ForEach([&](uint64_t, const Comp::UIChildCacheComponent::ChildRecord& rec) -> void {
                    Entity c = rec.entity;
                    if (!reg.IsAlive(c)) return;
                    if (const auto* nm = reg.Get<Comp::NameComponent>(c)) {
                        if (std::string_view(nm->name) == "_ti_text") textLeaf = c;
                    }
                });
            }
            ZHLN::Test::ExpectTrue(textLeaf != Entity::Null());
            if (textLeaf != Entity::Null()) {
                // Text leaf has TextComponent, no children, UITextInputComponent is NOT here
                ZHLN::Test::ExpectTrue(reg.Get<Comp::TextComponent>(textLeaf) != nullptr);
                ZHLN::Test::ExpectTrue(reg.Get<Comp::UITextInputComponent>(textLeaf) == nullptr);
                ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, textLeaf), 0u); // leaf
            }

            // Label path: when both id and label are supplied, a _ti_label
            // sibling is created; rebuild reuses entities.
            std::string v2 = "world";
            Entity ti2 = Entity::Null();
            size_t rectsBefore2 = CountUIRects(reg);
            {
                GUI::Context gui(reg, 2);
                ti2 = gui.TextInput("name", "Name", v2);
            }
            ZHLN::Test::ExpectEq(ti1.Pack(), ti2.Pack());
            ZHLN::Test::ExpectEq(CountUIRects(reg), rectsBefore2); // no respawn

            return {};
        }

        // ------------------------------------------------------------------
        // DROPDOWN: entity reuse, selected index out-param sync, option list
        // stored in the component, expansion starts collapsed, and removing
        // the dropdown sweeps its menu box + option items.
        // ------------------------------------------------------------------
        auto dropdown_stores_options_and_tracks_selected_index() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            std::array<std::string_view, 3> opts = {"Low", "Medium", "High"};
            int selected = 1;
            Entity dd1 = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                dd1 = gui.Dropdown("qual", "Quality", selected, opts);
            }
            const auto* ddComp = reg.Get<Comp::UIDropdownComponent>(dd1);
            ZHLN::Test::ExpectTrue(ddComp != nullptr);
            if (ddComp != nullptr) {
                ZHLN::Test::ExpectEq(static_cast<int>(ddComp->options.size()), 3);
                ZHLN::Test::ExpectEq(ddComp->selectedIdx, 1);
                ZHLN::Test::ExpectFalse(ddComp->expanded); // collapsed by default
            }
            // Header children exist: display text + arrow
            ZHLN::Test::ExpectTrue(CountCacheRecordsOn(reg, dd1) >= 2u);
            // Menu box should NOT exist while collapsed (we only create it when
            // expanded). That also means option items don't clutter the
            // registry when closed.
            size_t rectsWhileCollapsed = CountUIRects(reg);

            // Rebuild identical dropdown — stable entities, no leak.
            selected = 2;
            Entity dd2 = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                dd2 = gui.Dropdown("qual", "Quality", selected, opts);
            }
            ZHLN::Test::ExpectEq(dd1.Pack(), dd2.Pack());
            ZHLN::Test::ExpectEq(CountUIRects(reg), rectsWhileCollapsed);

            // Removing the dropdown from the tree sweeps all its children.
            {
                GUI::Context gui(reg, 3);
                gui.SweepStaleChildren(Entity::Null()); // "qual" not rebuilt this frame
            }
            ZHLN::Test::ExpectFalse(reg.IsAlive(dd1));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 0u);

            return {};
        }

        // ------------------------------------------------------------------
        // COLLAPSINGHEADER: RAII scope — content entities are only visited
        // (and thus exist) while the header is open. defaultOpen is a
        // create-time default, not a per-frame force. Toggle is driven by
        // the isOpen field on the component (which UIInteractionSystem sets
        // on click); flipping it closed causes the content subtree to be
        // swept on the next frame.
        // ------------------------------------------------------------------
        auto collapsing_header_content_exists_only_while_open() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            // Frame 1: defaultOpen = false, content closure is NOT invoked
            {
                GUI::Context gui(reg, 1);
                bool invoked = false;
                auto scope = gui.CollapsingHeader("adv", "Advanced", false, [&]() -> void {
                    invoked = true;
                });
                (void)scope;
                ZHLN::Test::ExpectFalse(invoked);
            }

            // Frame 2: create a header defaulted open. Content is built.
            Entity hdrOpen = Entity::Null();
            Entity lbl     = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                auto scope = gui.CollapsingHeader("adv_open", "Advanced", true, [&]() -> void {
                    lbl = gui.Label("inner-label");
                });
                hdrOpen = scope.GetEntity();
                (void)scope;
            }
            ZHLN::Test::ExpectTrue(hdrOpen != Entity::Null());
            ZHLN::Test::ExpectTrue(reg.IsAlive(lbl));
            const auto* hdrComp = reg.Get<Comp::UICollapsingHeaderComponent>(hdrOpen);
            ZHLN::Test::ExpectTrue(hdrComp != nullptr);
            if (hdrComp == nullptr) return {};
            ZHLN::Test::ExpectTrue(hdrComp->isOpen);

            // Frame 3: programmatically close the header (simulates click
            // handling, which flips isOpen). Next rebuild must NOT invoke
            // the content closure and the previous label must be swept.
            {
                GUI::Context gui(reg, 3);
                // Mutate the ECS directly, as ConsumeClick would on a title click.
                if (auto* mutHdr = reg.Get<Comp::UICollapsingHeaderComponent>(hdrOpen)) {
                    mutHdr->isOpen = false;
                }
                bool invoked = false;
                auto scope = gui.CollapsingHeader("adv_open", "Advanced", true, [&]() -> void {
                    invoked = true;
                    lbl = gui.Label("inner-label");
                });
                (void)scope;
                ZHLN::Test::ExpectFalse(invoked);
            }
            // The label created in frame 2 must be gone; header remains.
            ZHLN::Test::ExpectFalse(reg.IsAlive(lbl));
            ZHLN::Test::ExpectTrue(reg.IsAlive(hdrOpen));
            hdrComp = reg.Get<Comp::UICollapsingHeaderComponent>(hdrOpen);
            ZHLN::Test::ExpectTrue(hdrComp != nullptr && !hdrComp->isOpen);

            return {};
        }

        // ------------------------------------------------------------------
        // COLUMNS/SPLITTER: entity reuse, ratio is persisted in the
        // UISplitterComponent, left + right + handle children are created
        // and re-parented correctly, and removing the splitter sweeps its
        // subtree.
        // ------------------------------------------------------------------
        auto columns_creates_three_children_and_persists_ratio() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            float ratio = 0.3f;
            Entity sp1  = Entity::Null();
            Entity lLbl  = Entity::Null();
            Entity rLbl  = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                sp1 = gui.Columns("split", GUI::SplitDirection::Horizontal, ratio,
                    [&]() -> void { lLbl = gui.Label("L"); },
                    [&]() -> void { rLbl = gui.Label("R"); });
            }
            ZHLN::Test::ExpectTrue(sp1 != Entity::Null());
            ZHLN::Test::ExpectTrue(reg.IsAlive(lLbl));
            ZHLN::Test::ExpectTrue(reg.IsAlive(rLbl));
            const auto* spComp = reg.Get<Comp::UISplitterComponent>(sp1);
            ZHLN::Test::ExpectTrue(spComp != nullptr);
            if (spComp != nullptr) {
                ZHLN::Test::ExpectTrue(std::abs(spComp->ratio - 0.3f) < 1e-5f);
                ZHLN::Test::ExpectTrue(spComp->direction == Comp::UISplitterComponent::Horizontal);
                ZHLN::Test::ExpectFalse(spComp->isDragging);
            }
            // Expect 3 immediate children: left, handle, right
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, sp1), 3u);

            // Re-build with a different ratio (external mutation); entity
            // reuse still holds, ratio updates.
            Entity lLbl2 = Entity::Null(), rLbl2 = Entity::Null(), sp2 = Entity::Null();
            ratio = 0.6f;
            {
                GUI::Context gui(reg, 2);
                sp2 = gui.Columns("split", GUI::SplitDirection::Horizontal, ratio,
                    [&]() -> void { lLbl2 = gui.Label("L"); },
                    [&]() -> void { rLbl2 = gui.Label("R"); });
            }
            ZHLN::Test::ExpectEq(sp1.Pack(), sp2.Pack());
            ZHLN::Test::ExpectEq(lLbl.Pack(), lLbl2.Pack());
            ZHLN::Test::ExpectEq(rLbl.Pack(), rLbl2.Pack());
            spComp = reg.Get<Comp::UISplitterComponent>(sp2);
            ZHLN::Test::ExpectTrue(spComp != nullptr);
            if (spComp != nullptr) {
                ZHLN::Test::ExpectTrue(std::abs(spComp->ratio - 0.6f) < 1e-5f);
            }

            // Ratio must be clamped back to [0.05, 0.95]
            ratio = -0.5f;
            {
                GUI::Context gui(reg, 3);
                (void)gui.Columns("split", GUI::SplitDirection::Horizontal, ratio,
                    [&]() -> void {}, [&]() -> void {});
            }
            ZHLN::Test::ExpectTrue(ratio >= 0.05f - 1e-5f);

            return {};
        }

        // ------------------------------------------------------------------
        // Compound-widget container roots never carry a TextComponent (which
        // would give the Yoga node a measure function) while ALSO having
        // children attached. This is the universal form of the Yoga crash
        // "Cannot add child: Nodes with measure functions cannot have
        // children." Every compound widget root is validated here.
        // ------------------------------------------------------------------
        auto compound_widget_roots_never_pair_text_with_children() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            bool cb = false;
            float sl = 0.5f;
            std::string ti = "x";
            std::array<std::string_view, 2> ddOpts = {"a", "b"};
            int ddSel = 0;
            float splitterRatio = 0.5f;

            Entity cbEnt, slEnt, tiEnt, ddEnt, chEnt, spEnt;
            {
                GUI::Context gui(reg, 1);
                gui.Panel("r", GUI::PanelConfig {}, [&]() -> void {
                    cbEnt = gui.Checkbox("cb", "CB", cb);
                    slEnt = gui.Slider("sl", "SL", sl, 0.0f, 1.0f);
                    tiEnt = gui.TextInput("ti", "TI", ti);
                    ddEnt = gui.Dropdown("dd", "DD", ddSel, ddOpts);
                    auto ch = gui.CollapsingHeader("ch", "CH", true, [&]() -> void {
                        gui.Label("inside-ch");
                    });
                    chEnt = ch.GetEntity();
                    spEnt = gui.Columns("sp", GUI::SplitDirection::Horizontal, splitterRatio,
                        [&]() -> void { gui.Label("L"); },
                        [&]() -> void { gui.Label("R"); });
                });
            }

            // For each compound root: if it has children, it must NOT have a
            // TextComponent (which is what triggers Yoga's measure-func path).
            const auto checkRoot = [&](Entity e, const char* name) -> void {
                if (e == Entity::Null() || !reg.IsAlive(e)) return;
                const bool hasText     = reg.Get<Comp::TextComponent>(e) != nullptr;
                const size_t kids      = CountCacheRecordsOn(reg, e);
                if (kids > 0 && hasText) {
                    // Surface the violation as an Expect failure rather than
                    // a crash so we know which widget regressed.
                    ZHLN::Test::ExpectTrue(false && name);
                }
            };
            checkRoot(cbEnt, "checkbox");
            checkRoot(slEnt, "slider");
            checkRoot(tiEnt, "textinput");
            checkRoot(ddEnt, "dropdown");
            checkRoot(chEnt, "collapsingheader");
            checkRoot(spEnt, "splitter");

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunGUIContextSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GUIContextTestSuite>();
}

