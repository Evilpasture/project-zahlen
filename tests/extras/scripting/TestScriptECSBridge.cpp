// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Scripting/ScriptBinder.hpp>
#include <Scripting/ScriptECSBridge.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Core/String.hpp>
#include <array>
#include <expected>
#include <string>
#include <vector>

struct SubTransform {
    float x = 0.0f;
    float y = 0.0f;
};

struct PlayerDataComponent {
    ZHLN::String64         playerName = "Explorer";
    int32_t              level      = 1;
    SubTransform         coords;
    std::array<int32_t, 3> inventory {};

    int32_t LevelUp(int32_t amount) {
        level += amount;
        return level;
    }
};

struct BridgeManifest {
    using Comp1 = PlayerDataComponent;
    using Comp2 = SubTransform;
};

namespace {

struct ScriptECSBridgeTestSuite {
    ScriptECSBridgeTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    ~ScriptECSBridgeTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> ecs_bridge_property_access_and_drilling() {
            auto created = ZHLN::COM::CreateLogicWorldRef();
            if (!created)
                return std::unexpected(created.error());
            auto                  world = std::move(*created);
            auto&                 reg   = *world->Entities();
            ZHLN::ScriptECSBridge bridge {ZHLN::ScriptComponentAccess(reg)};

            // Register Manifest with both ECS and ScriptBinder
            bridge.RegisterComponentManifest<BridgeManifest>();

            // Create entity and attach component
            ZHLN::Entity e = reg.CreateEntity();
            reg.Attach(
                e, PlayerDataComponent {
                       .playerName = ZHLN::String64("Stalker_Alex"), .level = 5, .coords = {.x = 100.0f, .y = 250.0f}, .inventory = {101, 102, 103}
                   }
            );

            // 1. Direct Component Property Reading via Bridge
            auto nameVal = bridge.GetProperty(e, "PlayerDataComponent", "playerName");
            ZHLN::Test::ExpectTrue(nameVal.has_value());
            auto nameStr = ZHLN::FromScriptVal<std::string>(*nameVal);
            ZHLN::Test::ExpectTrue(nameStr.has_value());
            ZHLN::Test::ExpectEq(*nameStr, "Stalker_Alex");

            // 2. Direct Component Property Mutation via Bridge
            auto setRes = bridge.SetProperty(e, "PlayerDataComponent", "level", ZHLN::ToScriptVal(10));
            ZHLN::Test::ExpectTrue(setRes.has_value());
            auto comp = reg.Get<PlayerDataComponent>(e);
            ZHLN::Test::ExpectTrue(comp != nullptr);
            ZHLN::Test::ExpectEq(comp->level, 10);

            // 3. Multi-level Sub-structure Property Drilling (coords.x)
            auto coordsBoxed = bridge.GetProperty(e, "PlayerDataComponent", "coords");
            ZHLN::Test::ExpectTrue(coordsBoxed.has_value());
            auto xVal = bridge.GetPropertyOf(*coordsBoxed, "x");
            ZHLN::Test::ExpectTrue(xVal.has_value());
            auto xFloat = ZHLN::FromScriptVal<float>(*xVal);
            ZHLN::Test::ExpectTrue(xFloat.has_value());
            ZHLN::Test::ExpectEq(*xFloat, 100.0f);

            // 4. Sub-structure Mutation
            auto setXRes = bridge.SetPropertyOf(*coordsBoxed, "x", ZHLN::ToScriptVal(500.0f));
            ZHLN::Test::ExpectTrue(setXRes.has_value());
            ZHLN::Test::ExpectEq(comp->coords.x, 500.0f);

            // 5. Array Index Access (inventory[1])
            auto elemVal = bridge.GetPropertyElementAt(e, "PlayerDataComponent", "inventory", 1);
            ZHLN::Test::ExpectTrue(elemVal.has_value());
            auto elemInt = ZHLN::FromScriptVal<int32_t>(*elemVal);
            ZHLN::Test::ExpectTrue(elemInt.has_value());
            ZHLN::Test::ExpectEq(*elemInt, 102);

            auto setElemRes = bridge.SetPropertyElementAt(e, "PlayerDataComponent", "inventory", 1, ZHLN::ToScriptVal(999));
            ZHLN::Test::ExpectTrue(setElemRes.has_value());
            ZHLN::Test::ExpectEq(comp->inventory[1], 999);

            // A boxed component member must be re-resolved after a sparse-set
            // swap removal, and must stop resolving when its owner is removed.
            const auto other = reg.CreateEntity();
            reg.Attach(other, PlayerDataComponent {.coords = {.x = 37.0f}});
            auto movedBox = bridge.GetProperty(other, "PlayerDataComponent", "coords");
            ZHLN::Test::ExpectTrue(movedBox.has_value());
            reg.Detach<PlayerDataComponent>(e);
            auto movedX = bridge.GetPropertyOf(*movedBox, "x");
            ZHLN::Test::ExpectTrue(movedX.has_value());
            if (movedX) ZHLN::Test::ExpectEq(*ZHLN::FromScriptVal<float>(*movedX), 37.0f);
            reg.Detach<PlayerDataComponent>(other);
            ZHLN::Test::ExpectTrue(!bridge.GetPropertyOf(*movedBox, "x"));
            reg.Despawn(other);
            world->Flush();
            ZHLN::Test::ExpectTrue(!bridge.GetPropertyOf(*movedBox, "x"));

            return {};
        }
    };
};

} // namespace
