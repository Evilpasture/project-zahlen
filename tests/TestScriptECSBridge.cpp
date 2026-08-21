// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ScriptBinder.hpp>
#include <Zahlen/ScriptECSBridge.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>
#include <string>
#include <vector>

struct SubTransform {
    float x = 0.0f;
    float y = 0.0f;
};

struct PlayerDataComponent {
    std::string          playerName = "Explorer";
    int32_t              level      = 1;
    SubTransform         coords;
    std::vector<int32_t> inventory;

    int32_t LevelUp(int32_t amount) {
        level += amount;
        return level;
    }
};

struct BridgeManifest {
    using Comp1 = PlayerDataComponent;
    using Comp2 = SubTransform;
};

struct ScriptECSBridgeTestSuite {
    ScriptECSBridgeTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    ~ScriptECSBridgeTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> ecs_bridge_property_access_and_drilling() {
            ZHLN::ECS::Registry   reg;
            ZHLN::ScriptECSBridge bridge(reg);

            // Register Manifest with both ECS and ScriptBinder
            bridge.RegisterComponentManifest<BridgeManifest>();

            // Create entity and attach component
            ZHLN::Entity e = reg.Create();
            reg.Add<PlayerDataComponent>(
                e, PlayerDataComponent {.playerName = "Stalker_Alex", .level = 5, .coords = {.x = 100.0f, .y = 250.0f}, .inventory = {101, 102, 103}}
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
            auto* comp = reg.Get<PlayerDataComponent>(e);
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

            // 6. Direct Method Call via Bridge
            std::array<ZHLN::ScriptVal, 1> methodArgs = {ZHLN::ToScriptVal(3)};
            auto                           methodRes  = bridge.CallMethod(e, "PlayerDataComponent", "LevelUp", methodArgs);
            ZHLN::Test::ExpectTrue(methodRes.has_value());
            auto newLevel = ZHLN::FromScriptVal<int32_t>(*methodRes);
            ZHLN::Test::ExpectTrue(newLevel.has_value());
            ZHLN::Test::ExpectEq(*newLevel, 13);
            ZHLN::Test::ExpectEq(comp->level, 13);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ScriptECSBridgeTestSuite>();
}
