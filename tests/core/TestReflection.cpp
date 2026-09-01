// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

// ============================================================================
// Test Enums & Structs
// ============================================================================

enum class WeaponType : uint32_t {
    Pistol[[= ZHLN::Description<"Sidearm with high mobility.">{}]],
    Rifle[[= ZHLN::Description<"Standard automatic assault rifle.">{}]],
    Shotgun[[= ZHLN::Description<"Close-range high damage scattergun.">{}]]
};

enum class StatusEffect : uint32_t { None = 0, Burning = 1 << 0, Frozen = 1 << 1, Poisoned = 1 << 2 };

struct WeaponData {
    std::string name   = "AK-74";
    float       damage = 35.0f;
    int32_t     ammo   = 30;
    WeaponType  type   = WeaponType::Rifle;

    bool operator==(const WeaponData& other) const {
        return ZHLN::Reflect::GenericEqual(*this, other);
    }
};

struct BaseStats {
    int32_t level = 1;
};

struct Character: BaseStats {
    std::string name   = "Stalker";
    float       health = 100.0f;

    constexpr int32_t get_level() const {
        return level;
    }
    constexpr float get_health() const {
        return health;
    }
};

// Schema synthesis runs a consteval block that hands field names to
// std::meta::define_aggregate through std::string. Under -fsanitize=undefined
// GCC rejects the pointer null-check inside std::string's constructor during
// constant evaluation (GCC bugzilla #71962, still open), so the aggregate can
// never be completed in a sanitizer build. Compile this test out of sanitizer
// builds: it is not a Docker-only issue, ZHLN_IN_DOCKER was only masking it
// inside CI (which configures USE_SANITIZERS=ON together with ZHLN_IN_DOCKER).
#if !defined(ZHLN_SANITIZER_BUILD)
struct SchemaContainer {
    using ItemSchema = ZHLN::Reflect::Define<"ItemSchema", ZHLN::Reflect::Field<uint32_t, "id">, ZHLN::Reflect::Field<float, "weight">>::type;
};
#endif

// ============================================================================
// Test Suite
// ============================================================================

struct ReflectionTestSuite {
    struct Tests {
        // --- 1. Enum Reflection & Introspection ---
        std::expected<void, ZHLN::Error> enum_reflection() {
            // ToString & StringToEnum
            std::string_view name = ZHLN::Reflect::EnumToString(WeaponType::Rifle);
            ZHLN::Test::ExpectEq(name, "Rifle");

            auto parsed      = ZHLN::Reflect::StringToEnum<WeaponType>("Shotgun");
            auto checkParsed = ZHLN::Test::AssertTrue(parsed.has_value());
            if (!checkParsed)
                return checkParsed;
            ZHLN::Test::ExpectEq(*parsed, WeaponType::Shotgun);

            // No-match path: must come back empty, not an engaged optional.
            // Nothing dereferences `missing`, so this is an expectation rather
            // than an early-returning assert.
            auto missing = ZHLN::Reflect::StringToEnum<WeaponType>("NotAnEnumerator");
            ZHLN::Test::ExpectFalse(missing.has_value());

            // EnumToMessage
            std::string_view msg = ZHLN::Reflect::EnumToMessage(WeaponType::Pistol);
            ZHLN::Test::ExpectEq(msg, "Sidearm with high mobility.");

            // EnumCount & EnumNames
            constexpr size_t count = ZHLN::Reflect::EnumCount<WeaponType>();
            ZHLN::Test::ExpectEq(count, static_cast<size_t>(3));

            constexpr auto names = ZHLN::Reflect::EnumNames<WeaponType>();
            ZHLN::Test::ExpectEq(names[0], "Pistol");
            ZHLN::Test::ExpectEq(names[1], "Rifle");
            ZHLN::Test::ExpectEq(names[2], "Shotgun");

            // EnumToFlagsString
            std::string      flagStr;
            auto             flags    = static_cast<StatusEffect>(static_cast<uint32_t>(StatusEffect::Burning) | static_cast<uint32_t>(StatusEffect::Frozen));
            std::string_view flagView = ZHLN::Reflect::EnumToFlagsString(flags, flagStr);
            ZHLN::Test::ExpectEq(flagView, "Burning | Frozen");

            // ForEachEnumerator & DispatchEnum
            size_t iteratedEnums = 0;
            ZHLN::Reflect::ForEachEnumerator<WeaponType>([&]<WeaponType Val>() { iteratedEnums++; });
            ZHLN::Test::ExpectEq(iteratedEnums, static_cast<size_t>(3));

            bool dispatched = false;
            ZHLN::Reflect::DispatchEnum(WeaponType::Shotgun, [&]<WeaponType Val>() {
                if constexpr (Val == WeaponType::Shotgun) {
                    dispatched = true;
                }
            });
            ZHLN::Test::ExpectTrue(dispatched);

            return {};
        }

        // --- 2. Field Introspection & Access ---
        std::expected<void, ZHLN::Error> field_introspection_and_access() {
            // FieldCount & HasField
            constexpr size_t fCount = ZHLN::Reflect::FieldCount<WeaponData>();
            ZHLN::Test::ExpectEq(fCount, static_cast<size_t>(4));

            constexpr bool hasDamage = ZHLN::Reflect::HasField<WeaponData>("damage");
            constexpr bool hasMana   = ZHLN::Reflect::HasField<WeaponData>("mana");
            ZHLN::Test::ExpectTrue(hasDamage);
            ZHLN::Test::ExpectFalse(hasMana);

            // IndexOfField
            constexpr size_t ammoIdx = ZHLN::Reflect::IndexOfField<"ammo", WeaponData>();
            ZHLN::Test::ExpectEq(ammoIdx, static_cast<size_t>(2));

            // FieldNames
            constexpr auto fNames = ZHLN::Reflect::FieldNames<WeaponData>();
            ZHLN::Test::ExpectEq(fNames[0], "name");
            ZHLN::Test::ExpectEq(fNames[1], "damage");
            ZHLN::Test::ExpectEq(fNames[2], "ammo");
            ZHLN::Test::ExpectEq(fNames[3], "type");

            // GetField (indexed) & GetFieldByName (compile-time literal)
            WeaponData weapon {.name = "SVD", .damage = 80.0f, .ammo = 10, .type = WeaponType::Rifle};
            ZHLN::Test::ExpectEq(ZHLN::Reflect::GetField<0>(weapon), "SVD");
            ZHLN::Test::ExpectEq(ZHLN::Reflect::GetFieldByName<"damage">(weapon), 80.0f);

            // SetFieldByName
            bool setOk = ZHLN::Reflect::SetFieldByName<"ammo">(weapon, 15);
            ZHLN::Test::ExpectTrue(setOk);
            ZHLN::Test::ExpectEq(weapon.ammo, 15);

            // VisitFieldByName (runtime string with compile-time type guard)
            bool visited = ZHLN::Reflect::VisitFieldByName(weapon, "damage", [](auto&& val) {
                if constexpr (std::is_same_v<std::decay_t<decltype(val)>, float>) {
                    val += 5.0f;
                }
            });
            ZHLN::Test::ExpectTrue(visited);
            ZHLN::Test::ExpectEq(weapon.damage, 85.0f);

            return {};
        }

        // --- 3. Field Iterators & Accessors ---
        std::expected<void, ZHLN::Error> iteration_and_accessors() {
            WeaponData weapon {.name = "RPG-7", .damage = 500.0f, .ammo = 1, .type = WeaponType::Shotgun};

            // ForEachFieldWithName
            size_t visitedCount = 0;
            ZHLN::Reflect::ForEachFieldWithName(weapon, [&](std::string_view fname, auto&& /*val*/) {
                if (!fname.empty()) {
                    visitedCount++;
                }
            });
            ZHLN::Test::ExpectEq(visitedCount, static_cast<size_t>(4));

            // ForEachFieldIndexed
            size_t indexSum = 0;
            ZHLN::Reflect::ForEachFieldIndexed(weapon, [&](size_t idx, auto&& /*val*/) { indexSum += idx; });
            // 0 + 1 + 2 + 3 = 6
            ZHLN::Test::ExpectEq(indexSum, static_cast<size_t>(6));

            // ForEachFieldInfo (Offset & Type verification)
            size_t infoFieldsCount = 0;
            ZHLN::Reflect::ForEachFieldInfo<WeaponData>([&]<typename FieldType>(std::string_view name, size_t offset) {
                if (!name.empty() && offset < sizeof(WeaponData)) {
                    infoFieldsCount++;
                }
            });
            ZHLN::Test::ExpectEq(infoFieldsCount, static_cast<size_t>(4));

            // ForEachFieldAccessor (Type-guarded getter/setter invocation)
            size_t accessorCount = 0;
            ZHLN::Reflect::ForEachFieldAccessor<WeaponData>([&]<typename FieldType>(std::string_view name, auto const_getter, auto mut_getter, auto setter) {
                accessorCount++;
                if constexpr (std::is_same_v<FieldType, int32_t>) {
                    if (name == "ammo") {
                        ZHLN::Test::ExpectEq(const_getter(weapon), 1);
                        setter(weapon, 5);
                        ZHLN::Test::ExpectEq(mut_getter(weapon), 5);
                    }
                }
            });
            ZHLN::Test::ExpectEq(accessorCount, static_cast<size_t>(4));
            ZHLN::Test::ExpectEq(weapon.ammo, 5);

            return {};
        }

        // --- 4. Tuple & Structural Transformations ---
        std::expected<void, ZHLN::Error> tuple_and_structural_transforms() {
            WeaponData weapon {.name = "M9", .damage = 25.0f, .ammo = 15, .type = WeaponType::Pistol};

            // TieFields
            auto tied = ZHLN::Reflect::TieFields(weapon);
            ZHLN::Test::ExpectEq(std::get<0>(tied), "M9");
            ZHLN::Test::ExpectEq(std::get<1>(tied), 25.0f);

            // ZipFieldsWithNames
            auto zipped    = ZHLN::Reflect::ZipFieldsWithNames(weapon);
            auto firstPair = std::get<0>(zipped);
            ZHLN::Test::ExpectEq(firstPair.first, "name");
            ZHLN::Test::ExpectEq(firstPair.second, "M9");

            // MakeFromTuple
            struct Point3D {
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
            };
            auto    ptTuple = std::make_tuple(1.0f, 2.0f, 3.0f);
            Point3D pt      = ZHLN::Reflect::MakeFromTuple<Point3D>(ptTuple);
            ZHLN::Test::ExpectEq(pt.x, 1.0f);
            ZHLN::Test::ExpectEq(pt.y, 2.0f);
            ZHLN::Test::ExpectEq(pt.z, 3.0f);

            // MapFieldIndex
            struct TargetWeapon {
                float       damage = 0.0f;
                std::string name;
            };
            // In WeaponData: 'name' is at index 0, in TargetWeapon 'name' is at index 1
            constexpr size_t mappedIdx = ZHLN::Reflect::MapFieldIndex<WeaponData, TargetWeapon>(0);
            ZHLN::Test::ExpectEq(mappedIdx, static_cast<size_t>(1));

            return {};
        }

        // --- 5. Class Inheritance & Member Methods ---
        std::expected<void, ZHLN::Error> inheritance_and_member_methods() {
            // HasBases & ForEachBase
            constexpr bool charHasBases   = ZHLN::Reflect::HasBases<Character>();
            constexpr bool weaponHasBases = ZHLN::Reflect::HasBases<WeaponData>();
            ZHLN::Test::ExpectTrue(charHasBases);
            ZHLN::Test::ExpectFalse(weaponHasBases);

            size_t baseCount = 0;
            ZHLN::Reflect::ForEachBase<Character>([&]<typename Base>() {
                if constexpr (std::is_same_v<Base, BaseStats>) {
                    baseCount++;
                }
            });
            ZHLN::Test::ExpectEq(baseCount, static_cast<size_t>(1));

            // MemberFunctionCount & MemberFunctionNames
            constexpr size_t methodCount = ZHLN::Reflect::MemberFunctionCount<Character>();
            ZHLN::Test::ExpectEq(methodCount, static_cast<size_t>(2));

            constexpr auto methodNames = ZHLN::Reflect::MemberFunctionNames<Character>();
            ZHLN::Test::ExpectEq(methodNames[0], "get_level");
            ZHLN::Test::ExpectEq(methodNames[1], "get_health");

            // ForEachMethodPointer
            Character hero {.name = "Hero", .health = 95.0f};
            hero.level = 10;

            size_t visitedMethods = 0;
            ZHLN::Reflect::ForEachMethodPointer<Character>([&](std::string_view mname, auto pmf) {
                visitedMethods++;
                if (mname == "get_health") {
                    ZHLN::Test::ExpectEq((hero.*pmf)(), 95.0f);
                }
            });
            ZHLN::Test::ExpectEq(visitedMethods, static_cast<size_t>(2));

            // CollectMethodResults
            auto resultsTuple = ZHLN::Reflect::CollectMethodResults(hero);
            ZHLN::Test::ExpectEq(std::get<0>(resultsTuple), 10);
            ZHLN::Test::ExpectEq(std::get<1>(resultsTuple), 95.0f);

            return {};
        }

        // --- 6. Generic Operators, Comparison & Formatting ---
        std::expected<void, ZHLN::Error> generic_operators_and_formatting() {
            WeaponData w1 {.name = "MP5", .damage = 20.0f, .ammo = 30, .type = WeaponType::Pistol};
            WeaponData w2 {.name = "MP5", .damage = 20.0f, .ammo = 30, .type = WeaponType::Pistol};
            WeaponData w3 {.name = "MP5", .damage = 25.0f, .ammo = 30, .type = WeaponType::Pistol};

            // GenericEqual
            ZHLN::Test::ExpectTrue(ZHLN::Reflect::GenericEqual(w1, w2));
            ZHLN::Test::ExpectFalse(ZHLN::Reflect::GenericEqual(w1, w3));

            // GenericLess & GenericCompare
            ZHLN::Test::ExpectTrue(ZHLN::Reflect::GenericLess(w1, w3));
            ZHLN::Test::ExpectFalse(ZHLN::Reflect::GenericLess(w3, w1));
            ZHLN::Test::ExpectTrue(ZHLN::Reflect::GenericCompare(w1, w2) == 0);

            // GenericHash
            size_t h1 = ZHLN::Reflect::GenericHash(w1);
            size_t h2 = ZHLN::Reflect::GenericHash(w2);
            size_t h3 = ZHLN::Reflect::GenericHash(w3);
            ZHLN::Test::ExpectEq(h1, h2);
            ZHLN::Test::ExpectNe(h1, h3);

            // CopyMatchingFields
            struct Src {
                std::string name   = "G36";
                float       damage = 30.0f;
                int32_t     ammo   = 30;
            };
            struct Dst {
                std::string name;
                float       damage = 0.0f;
            };
            Src s;
            Dst d;
            ZHLN::Reflect::CopyMatchingFields(d, s);
            ZHLN::Test::ExpectEq(d.name, "G36");
            ZHLN::Test::ExpectEq(d.damage, 30.0f);

            // ToDebugString
            std::string debugStr = ZHLN::Reflect::ToDebugString(w1);
            ZHLN::Test::ExpectTrue(!debugStr.empty());
            ZHLN::Test::ExpectTrue(debugStr.find("MP5") != std::string::npos);

            return {};
        }

        // --- 7. Declarative Schema Types & Nested Types ---
        std::expected<void, ZHLN::Error> declarative_schema_and_nested_types() {
            // Skipped in sanitizer builds: Define's consteval block reaches
            // std::string in constant evaluation, which GCC's UBSan rejects
            // (bugzilla #71962). See the SchemaContainer definition above.
#if !defined(ZHLN_SANITIZER_BUILD)
            using Schema = SchemaContainer::ItemSchema;

            // Schema name resolution
            std::string_view schemaName = ZHLN::Reflect::GetSchemaNameOf<Schema>();
            ZHLN::Test::ExpectEq(schemaName, "ItemSchema");

            // Instantiation of synthesized aggregate
            Schema item {};
            ZHLN::Reflect::SetFieldByName<"id">(item, 42u);
            ZHLN::Reflect::SetFieldByName<"weight">(item, 3.5f);

            ZHLN::Test::ExpectEq(ZHLN::Reflect::GetFieldByName<"id">(item), 42u);
            ZHLN::Test::ExpectEq(ZHLN::Reflect::GetFieldByName<"weight">(item), 3.5f);

            // ForEachNestedType
            size_t nestedTypesCount = 0;
            ZHLN::Reflect::ForEachNestedType<SchemaContainer>([&]<typename Nested>() {
                if constexpr (std::is_same_v<Nested, Schema>) {
                    nestedTypesCount++;
                }
            });
            ZHLN::Test::ExpectEq(nestedTypesCount, static_cast<size_t>(1));
#endif
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunReflectionSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ReflectionTestSuite>();
}

