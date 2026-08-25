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
//     creation continues against the deepest live parent (an error is logged),
//     and a later sweep still reclaims the whole tree.

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
using ZHLN::NullEntity;
using ZHLN::ECS::Registry;
using ZHLN::GUI::GUIError;
namespace GUI  = ZHLN::GUI;
namespace Comp = ZHLN::Components;

[[nodiscard]] size_t CountUIRects(Registry& reg) {
    return reg.GetEntitiesWith<Comp::UIRectComponent>().size();
}

[[nodiscard]] size_t CountTotalCacheRecords(Registry& reg) {
    size_t total = 0;
    for (Entity e: reg.GetEntitiesWith<Comp::UIChildCacheComponent>()) {
        if (const auto* cache = reg.Get<Comp::UIChildCacheComponent>(e)) {
            total += cache->children.Size();
        }
    }
    return total;
}

[[nodiscard]] size_t CountCacheRecordsOn(Registry& reg, Entity e) {
    if (const auto* cache = reg.Get<Comp::UIChildCacheComponent>(e)) {
        return cache->children.Size();
    }
    return 0;
}

} // namespace

struct GUIContextTestSuite {
    struct Tests {

        // ------------------------------------------------------------------
        // Rebuilding the identical tree next frame must reuse entities, never
        // respawn them (respawning per frame is what makes UIs flicker).
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> widget_entities_are_reused_across_frames() {
            Registry reg;

            Entity root1 = NullEntity, boxA1 = NullEntity, boxB1 = NullEntity;
            {
                GUI::Context gui(reg, 1);
                root1 = gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    boxA1 = gui.BeginBox("a", GUI::BoxConfig {}, []() -> void {});
                    boxB1 = gui.BeginBox("b", GUI::BoxConfig {}, []() -> void {});
                });
                (void)gui.SweepStaleChildren(NullEntity);
            }

            Entity root2 = NullEntity, boxA2 = NullEntity, boxB2 = NullEntity;
            bool   buildSucceeded = false;
            {
                GUI::Context gui(reg, 2);
                root2 = gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    boxA2 = gui.BeginBox("a", GUI::BoxConfig {}, []() -> void {});
                    boxB2 = gui.BeginBox("b", GUI::BoxConfig {}, []() -> void {});
                });
                (void)gui.SweepStaleChildren(NullEntity);
                buildSucceeded = gui.Status().has_value(); // clean build: no structural error recorded
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
        // own sweep - immediately when BeginPanel returns, not "sometime".
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> stale_child_is_destroyed_when_parent_rebuilds_without_it() {
            Registry reg;

            Entity boxB = NullEntity;
            {
                GUI::Context gui(reg, 1);
                gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    gui.BeginBox("a", GUI::BoxConfig {}, []() -> void {});
                    boxB = gui.BeginBox("b", GUI::BoxConfig {}, []() -> void {});
                });
            }

            Entity root = NullEntity;
            {
                GUI::Context gui(reg, 2);
                root = gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    gui.BeginBox("a", GUI::BoxConfig {}, []() -> void {});
                    // "b" is no longer rebuilt -> swept before BeginPanel returns.
                });
            }

            ZHLN::Test::ExpectTrue(boxB != NullEntity);
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
        std::expected<void, ZHLN::Error> dynamic_widget_keys_do_not_leak_cache_records() {
            Registry reg;

            constexpr uint64_t kFrames = 50;
            Entity             root    = NullEntity;

            for (uint64_t frame = 1; frame <= kFrames; ++frame) {
                GUI::Context gui(reg, frame);
                root = gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    gui.BeginBox("stable", GUI::BoxConfig {}, []() -> void {});
                    gui.BeginBox("dynamic_" + std::to_string(frame), GUI::BoxConfig {}, []() -> void {});
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
        std::expected<void, ZHLN::Error> root_sweep_removes_unvisited_panels_and_purges_records() {
            Registry reg;

            Entity panel2 = NullEntity;
            {
                GUI::Context gui(reg, 1);
                gui.BeginPanel("p1", GUI::PanelConfig {}, []() -> void {});
                panel2 = gui.BeginPanel("p2", GUI::PanelConfig {}, []() -> void {});
                (void)gui.SweepStaleChildren(NullEntity);
            }

            Entity rootCache = NullEntity;
            bool   reportOk = false, reportCountsRight = false;
            {
                GUI::Context gui(reg, 2);
                rootCache = gui.GetRootCacheEntity();
                gui.BeginPanel("p1", GUI::PanelConfig {}, []() -> void {});

                // The sweep's result is data, not a log line: exactly one stale
                // subtree destroyed ("p2"), nothing orphaned.
                const auto report = gui.SweepStaleChildren(NullEntity);
                reportOk          = report.has_value();
                reportCountsRight = report.has_value() && report->destroyedSubtrees == 1u && report->purgedRecords == 0u;
            }

            ZHLN::Test::ExpectTrue(panel2 != NullEntity);
            ZHLN::Test::ExpectFalse(reg.IsAlive(panel2));
            ZHLN::Test::ExpectEq(CountCacheRecordsOn(reg, rootCache), 1u); // only "p1"
            ZHLN::Test::ExpectTrue(reportOk);
            ZHLN::Test::ExpectTrue(reportCountsRight);

            return {};
        }

        // ------------------------------------------------------------------
        // Sweeping a nested child panel must destroy its whole subtree, not
        // leave its labels/boxes orphaned in the registry.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> nested_sweep_destroys_child_panel_and_its_contents() {
            Registry reg;

            Entity childPanel = NullEntity, childLabel = NullEntity, childBox = NullEntity;
            {
                GUI::Context gui(reg, 1);
                gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    childPanel = gui.BeginPanel("nested", GUI::PanelConfig {}, [&]() -> void {
                        childLabel = gui.Label("hello");
                        childBox   = gui.BeginBox("inner", GUI::BoxConfig {}, []() -> void {});
                    });
                });
            }

            {
                GUI::Context gui(reg, 2);
                gui.BeginPanel("root", GUI::PanelConfig {}, []() -> void {}); // "nested" no longer rebuilt
            }

            ZHLN::Test::ExpectFalse(reg.IsAlive(childPanel));
            ZHLN::Test::ExpectFalse(reg.IsAlive(childLabel));
            ZHLN::Test::ExpectFalse(reg.IsAlive(childBox));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 1u); // root alone
            ZHLN::Test::ExpectEq(CountTotalCacheRecords(reg), 1u); // root's record at the root cache

            return {};
        }

        // ------------------------------------------------------------------
        // DestroyUIEntity removes an entire subtree on demand; the (now dead)
        // record the root cache still holds is purged by the next sweep.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> destroy_ui_entity_removes_whole_subtree() {
            Registry reg;
            Entity   root = NullEntity, boxB = NullEntity, label = NullEntity, boxC = NullEntity;

            GUI::Context gui(reg, 1);
            Entity   rootCache = gui.GetRootCacheEntity();
            root               = gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                boxB = gui.BeginBox("b", GUI::BoxConfig {}, [&]() -> void { label = gui.Label("inside"); });
                boxC = gui.BeginBox("c", GUI::BoxConfig {}, []() -> void {});
            });

            const auto destroyResult = gui.DestroyUIEntity(root);
            ZHLN::Test::ExpectTrue(destroyResult.has_value()); // success: silent, expected-void

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
            const auto report = gui.SweepStaleChildren(NullEntity);
            ZHLN::Test::ExpectTrue(report.has_value());
            if (report.has_value()) {
                ZHLN::Test::ExpectEq(report->destroyedSubtrees, 0u);
                ZHLN::Test::ExpectEq(report->purgedRecords, 1u);
            }
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
        std::expected<void, ZHLN::Error> externally_destroyed_widget_respawns_next_frame() {
            Registry reg;

            Entity boxA1 = NullEntity, root = NullEntity;
            {
                GUI::Context gui(reg, 1);
                root  = gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    boxA1 = gui.BeginBox("a", GUI::BoxConfig {}, []() -> void {});
                });
            }
            reg.Destroy(boxA1); // the external-mistake path

            Entity boxA2 = NullEntity;
            {
                GUI::Context gui(reg, 2);
                gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void {
                    boxA2 = gui.BeginBox("a", GUI::BoxConfig {}, []() -> void {});
                });
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
        std::expected<void, ZHLN::Error> hierarchy_deeper_than_stack_cap_degrades_but_survives() {
            Registry reg;

            constexpr size_t                 kDepth = GUI::MAX_UI_STACK_DEPTH + 8;
            std::array<Entity, kDepth + 1>   boxes {}; // 1 index-based for readability (boxes[1] == outermost)

            bool statusIsError = false, statusCodeIsRight = false;
            {
                GUI::Context gui(reg, 1);
                std::function<void(size_t)> build = [&](size_t level) -> void {
                    if (level > kDepth) {
                        return;
                    }
                    boxes[level] = gui.BeginBox(GUI::BoxConfig {.height = 4.0f}, [&]() -> void { build(level + 1); });
                };
                build(1);

                // Overflowing the scope stack is a typed, queryable failure -
                // not a log line. The build itself still completed above.
                const auto status   = gui.Status();
                statusIsError       = !status.has_value();
                statusCodeIsRight   = (!status.has_value()) && status.error().Is(GUIError::HierarchyTooDeep);
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
            // (the last widget whose PushParent succeeded).
            ZHLN::Test::ExpectEq(rectPastCap1->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH].Pack());
            ZHLN::Test::ExpectEq(rectPastCap2->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH].Pack());
            ZHLN::Test::ExpectEq(rectDeepest->parentEntity.Pack(), boxes[GUI::MAX_UI_STACK_DEPTH].Pack());
            // Depth stops growing past the cap (no unbounded counter).
            ZHLN::Test::ExpectEq(rectDeepest->hierarchyDepth, rectPastCap1->hierarchyDepth);

            // Whole tree - including the misattached tail - is reclaimed by a
            // single root sweep: exactly one destroyed subtree, no orphans.
            bool sweepOk = false, sweepCountsRight = false;
            {
                GUI::Context gui(reg, 2);
                const auto report = gui.SweepStaleChildren(NullEntity);
                sweepOk           = report.has_value();
                sweepCountsRight  = report.has_value() && report->destroyedSubtrees == 1u && report->purgedRecords == 0u;
            }
            ZHLN::Test::ExpectTrue(sweepOk);
            ZHLN::Test::ExpectTrue(sweepCountsRight);
            ZHLN::Test::ExpectEq(CountUIRects(reg), 0u);
            ZHLN::Test::ExpectEq(CountTotalCacheRecords(reg), 0u);

            return {};
        }

        // ------------------------------------------------------------------
        // Labels are keyed by their text by design: changing the text swaps the
        // entity. Document the behaviour so the sweep keeps cleanup predictable
        // for dynamic text (and so nobody "fixes" it by accident).
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> label_text_change_replaces_entity_and_sweeps_old_one() {
            Registry reg;

            Entity label1 = NullEntity;
            {
                GUI::Context gui(reg, 1);
                gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void { label1 = gui.Label("count 1"); });
            }

            Entity label2 = NullEntity;
            {
                GUI::Context gui(reg, 2);
                gui.BeginPanel("root", GUI::PanelConfig {}, [&]() -> void { label2 = gui.Label("count 2"); });
            }

            ZHLN::Test::ExpectFalse(reg.IsAlive(label1));
            ZHLN::Test::ExpectTrue(reg.IsAlive(label2));
            ZHLN::Test::ExpectEq(CountUIRects(reg), 2u);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<GUIContextTestSuite>();
}
