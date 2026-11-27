// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Scripting/ScriptECSBridge.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <array>
#include <optional>
struct ConsoleLeaf {
    float value = 1;
};
struct ConsoleBranch {
    ConsoleLeaf leaf;
};
struct ConsolePOD {
    ZHLN::String64                            label {"initial"};
    ConsoleBranch                             branch;
    std::array<ConsoleBranch, 2>              branches {};
    std::array<std::array<ConsoleLeaf, 2>, 2> grid {};
};
struct ConsolePODManifest {
    using Root   = ConsolePOD;
    using Branch = ConsoleBranch;
    using Leaf   = ConsoleLeaf;
};
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
struct COMScriptLifetimeSuite {
    struct Tests {
        std::expected<void, ErrorCode> nested_and_indexed_boxes_re_resolve_after_storage_moves_and_removal() {
            auto created = COM::CreateLogicWorld();
            if (!created)
                return std::unexpected(created.error());
            auto            world    = std::move(*created);
            auto&           entities = *world->Entities();
            ScriptECSBridge bridge(entities);
            bridge.RegisterComponentManifest<ConsolePODManifest>();
            auto victim = entities.CreateEntity();
            entities.Attach(victim, ConsolePOD {});
            auto entity = entities.CreateEntity();
            entities.Attach(entity, ConsolePOD {});
            auto branch = bridge.GetProperty(entity, "ConsolePOD", "branch");
            if (!branch)
                return std::unexpected(branch.error());
            auto leaf = bridge.GetPropertyOf(*branch, "leaf");
            if (!leaf)
                return std::unexpected(leaf.error());
            auto grid = bridge.GetProperty(entity, "ConsolePOD", "grid");
            if (!grid)
                return std::unexpected(grid.error());
            auto row = bridge.GetArrayElement(*grid, 1);
            if (!row)
                return std::unexpected(row.error());
            auto cell = bridge.GetArrayElement(*row, 1);
            if (!cell)
                return std::unexpected(cell.error());
            ExpectTrue(std::get<BoxedObject>(*cell).rawPtr == nullptr);
            for (int i = 0; i < 150; ++i) {
                auto e = entities.CreateEntity();
                entities.Attach(e, ConsolePOD {});
            }
            ExpectTrue(entities.Detach<ConsolePOD>(victim));
            ExpectTrue(bridge.SetPropertyOf(*leaf, "value", ScriptVal {4.0}).has_value());
            ExpectTrue(bridge.SetPropertyOf(*cell, "value", ScriptVal {9.0}).has_value());
            auto* live = entities.Get<ConsolePOD>(entity);
            if (!ExpectTrue(live != nullptr))
                return {};
            ExpectEq(live->branch.leaf.value, 4.f);
            ExpectEq(live->grid[1][1].value, 9.f);
            ExpectTrue(entities.Detach<ConsolePOD>(entity));
            ExpectTrue(!bridge.GetPropertyOf(*leaf, "value"));
            ExpectTrue(!bridge.GetPropertyOf(*cell, "value"));
            entities.Attach(entity, ConsolePOD {});
            auto refreshed = bridge.GetPropertyOf(*leaf, "value");
            ExpectTrue(refreshed.has_value());
            if (refreshed)
                ExpectEq(std::get<double>(*refreshed), 1.0);
            entities.Despawn(entity);
            world->Flush();
            ExpectTrue(!bridge.GetPropertyOf(*leaf, "value"));
            return {};
        }
        std::expected<void, ErrorCode> array_box_assignment_resolves_rhs_and_rejects_cross_world_identity() {
            auto a = COM::CreateLogicWorld();
            auto b = COM::CreateLogicWorld();
            if (!a)
                return std::unexpected(a.error());
            if (!b)
                return std::unexpected(b.error());
            ScriptECSBridge left(*(*a)->Entities()), right(*(*b)->Entities());
            left.RegisterComponentManifest<ConsolePODManifest>();
            right.RegisterComponentManifest<ConsolePODManifest>();
            auto       e = (*a)->Entities()->CreateEntity();
            auto       f = (*b)->Entities()->CreateEntity();
            ConsolePOD initial;
            initial.branches[1].leaf.value = 7;
            (*a)->Entities()->Attach(e, initial);
            (*b)->Entities()->Attach(f, ConsolePOD {});
            auto values = left.GetProperty(e, "ConsolePOD", "branches");
            if (!values)
                return std::unexpected(values.error());
            ExpectTrue(left.SetProperty(e, "ConsolePOD", "branches", *values).has_value());
            auto element = left.GetPropertyElementAt(e, "ConsolePOD", "branches", 1);
            if (!element)
                return std::unexpected(element.error());
            ExpectTrue(left.SetPropertyElementAt(e, "ConsolePOD", "branches", 0, *element).has_value());
            ExpectEq((*a)->Entities()->Get<ConsolePOD>(e)->branches[0].leaf.value, 7.f);
            ExpectTrue(!right.GetPropertyOf(*element, "leaf"));
            ExpectTrue(!right.SetPropertyElementAt(f, "ConsolePOD", "branches", 0, *element));
            ExpectTrue(!left.GetPropertyElementAt(e, "ConsolePOD", "branches", 2));
            return {};
        }
        std::expected<void, ErrorCode> boxed_values_retain_world_after_bridge_and_factory_owner_leave_scope() {
            std::optional<ScriptVal> held;
            COM::IEntities*          borrowed = nullptr;
            {
                auto world = COM::CreateLogicWorld();
                if (!world)
                    return std::unexpected(world.error());
                borrowed = (*world)->Entities();
                ScriptECSBridge bridge(*borrowed);
                bridge.RegisterComponentManifest<ConsolePODManifest>();
                auto e = borrowed->CreateEntity();
                borrowed->Attach(e, ConsolePOD {});
                auto box = bridge.GetProperty(e, "ConsolePOD", "branch");
                if (!box)
                    return std::unexpected(box.error());
                held = std::move(*box);
            }
            ScriptECSBridge later(*borrowed);
            later.RegisterComponentManifest<ConsolePODManifest>();
            auto leaf = later.GetPropertyOf(*held, "leaf");
            ExpectTrue(leaf.has_value());
            if (leaf) {
                auto value = later.GetPropertyOf(*leaf, "value");
                ExpectTrue(value.has_value());
            }
            return {};
        }
        std::expected<void, ErrorCode> fixed_strings_and_resource_owners_have_explicit_mutation_rules() {
            auto made = COM::CreateLogicWorld();
            if (!made)
                return std::unexpected(made.error());
            auto&           entities = *(*made)->Entities();
            ScriptECSBridge bridge(entities);
            bridge.RegisterComponentManifest<ConsolePODManifest>();
            auto e = entities.CreateEntity();
            entities.Attach(e, ConsolePOD {});
            ExpectTrue(bridge.SetProperty(e, "ConsolePOD", "label", ScriptVal {std::string("renamed")}).has_value());
            ExpectEq(std::string_view(entities.Get<ConsolePOD>(e)->label), std::string_view("renamed"));
            ExpectTrue(!bridge.SetProperty(e, "ConsolePOD", "label", ScriptVal {std::string(100, 'x')}));
            entities.Attach(e, ZHLN::Components::DynamicBodyComponent {});
            auto readBody = bridge.GetProperty(e, "DynamicBodyComponent", "bodyID");
            ExpectTrue(readBody.has_value());
            if (readBody)
                ExpectTrue(!bridge.SetProperty(e, "DynamicBodyComponent", "bodyID", *readBody));
            return {};
        }
        std::expected<void, ErrorCode> host_owned_stl_values_keep_reflection_without_becoming_com_components() {
            std::vector<ConsoleBranch> values(2);
            values[1].leaf.value = 6;
            auto encoded         = ToScriptVal(values);
            auto decoded         = FromScriptVal<std::vector<ConsoleBranch>>(encoded);
            ExpectTrue(decoded.has_value());
            if (decoded)
                ExpectEq((*decoded)[1].leaf.value, 6.f);
            return {};
        }
    };
};
} // namespace
