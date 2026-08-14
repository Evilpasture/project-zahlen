// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <expected>
#include <string>

enum class WeaponType : uint32_t {
    Pistol[[= ZHLN::Reflect::Description("Sidearm with high mobility.")]],
    Rifle[[= ZHLN::Reflect::Description("Standard automatic assault rifle.")]],
    Shotgun[[= ZHLN::Reflect::Description("Close-range high damage scattergun.")]]
};

struct WeaponData {
    std::string name   = "AK-74";
    float       damage = 35.0f;
    int32_t     ammo   = 30;
    WeaponType  type   = WeaponType::Rifle;

    bool operator==(const WeaponData& other) const {
        return ZHLN::Reflect::GenericEqual(*this, other);
    }
};

struct ReflectionTestSuite {
    struct Tests {
        // --- 1. Enum Stringification & Parsing ---
        std::expected<void, ZHLN::Error> enum_reflection() {
            std::string_view name = ZHLN::Reflect::EnumToString(WeaponType::Rifle);
            ZHLN::Test::ExpectEq(name, "Rifle");

            auto parsed      = ZHLN::Reflect::StringToEnum<WeaponType>("Shotgun");
            auto checkParsed = ZHLN::Test::AssertTrue(parsed.has_value());
            if (!checkParsed)
                return checkParsed;
            ZHLN::Test::ExpectEq(*parsed, WeaponType::Shotgun);

            // Test description annotation parsing
            std::string_view msg = ZHLN::Reflect::EnumToMessage(WeaponType::Pistol);
            ZHLN::Test::ExpectEq(msg, "Sidearm with high mobility.");

            return {};
        }

        // --- 2. Generic Equals, Comparison, & Hash ---
        std::expected<void, ZHLN::Error> generic_operators() {
            WeaponData w1 {.name = "MP5", .damage = 20.0f, .ammo = 30, .type = WeaponType::Pistol};
            WeaponData w2 {.name = "MP5", .damage = 20.0f, .ammo = 30, .type = WeaponType::Pistol};
            WeaponData w3 {.name = "MP5", .damage = 25.0f, .ammo = 30, .type = WeaponType::Pistol};

            ZHLN::Test::ExpectTrue(ZHLN::Reflect::GenericEqual(w1, w2));
            ZHLN::Test::ExpectFalse(ZHLN::Reflect::GenericEqual(w1, w3));

            size_t h1 = ZHLN::Reflect::GenericHash(w1);
            size_t h2 = ZHLN::Reflect::GenericHash(w2);
            size_t h3 = ZHLN::Reflect::GenericHash(w3);

            ZHLN::Test::ExpectEq(h1, h2);
            ZHLN::Test::ExpectNe(h1, h3);

            return {};
        }

        // --- 3. Field Copying Between Matching Structs ---
        std::expected<void, ZHLN::Error> copy_matching_fields() {
            struct Source {
                std::string name   = "M4A1";
                float       damage = 40.0f;
                int32_t     ammo   = 30;
            };

            struct Destination {
                std::string name;
                float       damage = 0.0f;
                bool        extra  = false;
            };

            Source      src;
            Destination dst;

            ZHLN::Reflect::CopyMatchingFields(dst, src);

            ZHLN::Test::ExpectEq(dst.name, "M4A1");
            ZHLN::Test::ExpectEq(dst.damage, 40.0f);
            ZHLN::Test::ExpectFalse(dst.extra); // Untouched field

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ReflectionTestSuite>();
}
