// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/scripting/TestScriptValueTypes.cpp
//
// Covers ScriptValueTypes.hpp: the conversions that let the engine's own value
// types cross the script boundary, which is what makes the reflection-driven
// binder usable for engine calls at all.
//
// These are the conversions the monolithic ZHLN_DispatchCommand table exists to
// work around. Every argument blob in Scripting.cpp -- SetCharVelArgs,
// RaycastArgs, AddImpulseAtArgs and the rest -- is a hand-flattened spelling of
// a call that takes an Entity and a Vec3 or two. If they round-trip here, the
// table stops being load-bearing and the Lua side can call PhysicsContext
// directly through ZHLN_InvokeMethod.
//
// Nothing here depends on static reflection. ToScriptVal/FromScriptVal and the
// trait are ordinary templates, so this suite exercises real conversions rather
// than compile-time table generation -- including on a build where the
// compiler's reflection support is off.

#include "TestsFramework.hpp"
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Entity.hpp>
#include <cstdint>
#include <expected>
#include <Scripting/ScriptBinder.hpp>
#include <Scripting/ScriptValueTypes.hpp>
#include <string>

namespace {

enum class ScriptValueTypeTestError : uint32_t {
    ValueConversionFailed[[= ZHLN::Description<"A value type did not round-trip through ToScriptVal/FromScriptVal."> {}]] = 1,
    WrongRepresentation[[= ZHLN::Description<"A value type did not cross as the script representation it documents."> {}]],
    BadInputAccepted[[= ZHLN::Description<"A malformed script value was accepted instead of rejected."> {}]],
    WrongErrorReported[[= ZHLN::Description<"A rejected value reported a different ScriptError than expected."> {}]]
};

/// A ScriptVal holding a fixed-length array of numbers, spelled the way Lua would.
auto Numbers(std::initializer_list<double> values) -> ZHLN::ScriptVal {
    ZHLN::ScriptArray arr;
    arr.elements.reserve(values.size());
    for (double v: values) {
        arr.elements.push_back(v);
    }
    return arr;
}

/// A BoxedObject naming T and pointing at an existing instance.
///
/// The name comes from the trait rather than Reflect::TypeName<T>() because the
/// no-reflection stub in Core/Reflection.hpp returns "" for every type, which
/// would make the mismatch case below compare "" to "" and pass vacuously on a
/// build without reflection. On a reflection build these are the same string
/// std::meta::identifier_of produces, so the test reflects what a real producer
/// writes either way.
template <typename T>
auto BoxOf(T* ptr) -> ZHLN::ScriptVal {
    ZHLN::BoxedObject box {};
    box.typeName = ZHLN::ScriptValueTrait<T>::scriptName;
    box.rawPtr   = ptr;
    return box;
}

struct ScriptValueTypesTestSuite {
    // An Entity crosses as its packed 64-bit id, which is how the Fennel
    // sources already spell entities everywhere.
    std::expected<void, ZHLN::Error> entity_crosses_as_packed_id() {
        const ZHLN::Entity entity = ZHLN::Entity::Unpack(0x0000'1234'0000'5678ULL);

        ZHLN::ScriptVal val = ZHLN::ToScriptVal(entity);

        auto isNumber = ZHLN::Test::AssertTrue(std::get_if<double>(&val) != nullptr);
        if (!isNumber) {
            return isNumber;
        }
        ZHLN::Test::ExpectEq(*std::get_if<double>(&val), static_cast<double>(entity.Pack()));

        auto back = ZHLN::FromScriptVal<ZHLN::Entity>(val);
        auto ok   = ZHLN::Test::AssertTrue(back.has_value());
        if (!ok) {
            return ok;
        }
        ZHLN::Test::ExpectEq(back->Pack(), entity.Pack());

        return {};
    }

    std::expected<void, ZHLN::Error> entity_rejects_non_number() {
        auto res = ZHLN::FromScriptVal<ZHLN::Entity>(ZHLN::ScriptVal {std::string {"not an entity"}});

        auto rejected = ZHLN::Test::AssertTrue(!res.has_value());
        if (!rejected) {
            return rejected;
        }
        ZHLN::Test::ExpectTrue(res.error().Is(ZHLN::ScriptError::TypeMismatch));
        return {};
    }

    std::expected<void, ZHLN::Error> vec3_crosses_as_three_numbers() {
        const JPH::Vec3 v(1.5f, -2.25f, 3.0f);

        ZHLN::ScriptVal val = ZHLN::ToScriptVal(v);

        const auto* arr = std::get_if<ZHLN::ScriptArray>(&val);
        auto        ok  = ZHLN::Test::AssertTrue(arr != nullptr);
        if (!ok) {
            return ok;
        }
        ZHLN::Test::ExpectEq(arr->elements.size(), static_cast<size_t>(3));

        auto back = ZHLN::FromScriptVal<JPH::Vec3>(val);
        auto conv = ZHLN::Test::AssertTrue(back.has_value());
        if (!conv) {
            return conv;
        }
        ZHLN::Test::ExpectEq(back->GetX(), 1.5f);
        ZHLN::Test::ExpectEq(back->GetY(), -2.25f);
        ZHLN::Test::ExpectEq(back->GetZ(), 3.0f);

        // And the literal form a script would actually write.
        auto fromLiteral = ZHLN::FromScriptVal<JPH::Vec3>(Numbers({7.0, 8.0, 9.0}));
        auto literalOk   = ZHLN::Test::AssertTrue(fromLiteral.has_value());
        if (!literalOk) {
            return literalOk;
        }
        ZHLN::Test::ExpectEq(fromLiteral->GetX(), 7.0f);
        ZHLN::Test::ExpectEq(fromLiteral->GetZ(), 9.0f);

        return {};
    }

    std::expected<void, ZHLN::Error> dvec3_keeps_double_precision() {
        // Physics origins are RVec3, which is DVec3 under JPH_DOUBLE_PRECISION.
        // A round-trip through double must not truncate the way Vec3's float
        // path would -- these magnitudes are ordinary world coordinates.
        const JPH::DVec3 v(123456.789012, -987654.321098, 0.000001);

        ZHLN::ScriptVal val  = ZHLN::ToScriptVal(v);
        auto            back = ZHLN::FromScriptVal<JPH::DVec3>(val);

        auto conv = ZHLN::Test::AssertTrue(back.has_value());
        if (!conv) {
            return conv;
        }
        ZHLN::Test::ExpectEq(back->GetX(), 123456.789012);
        ZHLN::Test::ExpectEq(back->GetY(), -987654.321098);
        ZHLN::Test::ExpectEq(back->GetZ(), 0.000001);

        return {};
    }

    std::expected<void, ZHLN::Error> quat_crosses_as_four_numbers() {
        const JPH::Quat q(0.1f, 0.2f, 0.3f, 0.9f);

        ZHLN::ScriptVal val = ZHLN::ToScriptVal(q);

        const auto* arr = std::get_if<ZHLN::ScriptArray>(&val);
        auto        ok  = ZHLN::Test::AssertTrue(arr != nullptr);
        if (!ok) {
            return ok;
        }
        ZHLN::Test::ExpectEq(arr->elements.size(), static_cast<size_t>(4));

        auto back = ZHLN::FromScriptVal<JPH::Quat>(val);
        auto conv = ZHLN::Test::AssertTrue(back.has_value());
        if (!conv) {
            return conv;
        }
        ZHLN::Test::ExpectEq(back->GetX(), 0.1f);
        ZHLN::Test::ExpectEq(back->GetY(), 0.2f);
        ZHLN::Test::ExpectEq(back->GetZ(), 0.3f);
        ZHLN::Test::ExpectEq(back->GetW(), 0.9f);

        return {};
    }

    std::expected<void, ZHLN::Error> vec4_crosses_as_four_numbers() {
        // Colours reach scripts this way: DrawLine's endpoints, Material.baseColorFactor.
        const JPH::Vec4 c(0.25f, 0.5f, 0.75f, 1.0f);

        ZHLN::ScriptVal val  = ZHLN::ToScriptVal(c);
        auto            back = ZHLN::FromScriptVal<JPH::Vec4>(val);

        auto conv = ZHLN::Test::AssertTrue(back.has_value());
        if (!conv) {
            return conv;
        }
        ZHLN::Test::ExpectEq(back->GetX(), 0.25f);
        ZHLN::Test::ExpectEq(back->GetW(), 1.0f);

        return {};
    }

    // A vector read off a component arrives boxed; a script should be able to
    // pass it straight back without unpacking it into a literal.
    std::expected<void, ZHLN::Error> vectors_accept_boxed_instances() {
        JPH::Vec3       source(4.0f, 5.0f, 6.0f);
        ZHLN::ScriptVal boxed = BoxOf(&source);

        auto back = ZHLN::FromScriptVal<JPH::Vec3>(boxed);
        auto conv = ZHLN::Test::AssertTrue(back.has_value());
        if (!conv) {
            return conv;
        }
        ZHLN::Test::ExpectEq(back->GetX(), 4.0f);
        ZHLN::Test::ExpectEq(back->GetY(), 5.0f);
        ZHLN::Test::ExpectEq(back->GetZ(), 6.0f);

        return {};
    }

    // The binder casts a boxed pointer without being able to check it, so the
    // type name is the only guard. Reinterpreting one as the other would read
    // four floats out of a three-float object.
    std::expected<void, ZHLN::Error> boxed_type_mismatch_is_rejected() {
        JPH::Quat       quat(1.0f, 0.0f, 0.0f, 0.0f);
        ZHLN::ScriptVal boxedQuat = BoxOf(&quat);

        auto res = ZHLN::FromScriptVal<JPH::Vec3>(boxedQuat);

        auto rejected = ZHLN::Test::AssertTrue(!res.has_value());
        if (!rejected) {
            return rejected;
        }
        ZHLN::Test::ExpectTrue(res.error().Is(ZHLN::ScriptError::TypeMismatch));
        return {};
    }

    std::expected<void, ZHLN::Error> wrong_length_array_is_rejected() {
        auto res = ZHLN::FromScriptVal<JPH::Vec3>(Numbers({1.0, 2.0}));

        auto rejected = ZHLN::Test::AssertTrue(!res.has_value());
        if (!rejected) {
            return rejected;
        }
        // Two numbers for a three-component vector is a shape problem, not a
        // type problem; scripts get told which.
        ZHLN::Test::ExpectTrue(res.error().Is(ZHLN::ScriptError::ArityMismatch));
        return {};
    }

    std::expected<void, ZHLN::Error> non_numeric_element_is_rejected() {
        ZHLN::ScriptArray mixed;
        mixed.elements.push_back(1.0);
        mixed.elements.push_back(std::string {"y"});
        mixed.elements.push_back(3.0);

        auto res = ZHLN::FromScriptVal<JPH::Vec3>(ZHLN::ScriptVal {std::move(mixed)});

        auto rejected = ZHLN::Test::AssertTrue(!res.has_value());
        if (!rejected) {
            return rejected;
        }
        ZHLN::Test::ExpectTrue(res.error().Is(ZHLN::ScriptError::TypeMismatch));
        return {};
    }

    // The Jolt *Arg parameter types are aliases for const T (MathTypes.h), so a
    // method declared to take Vec3Arg must accept what Vec3 produces. This is
    // the case that decides whether PhysicsContext can be registered at all.
    std::expected<void, ZHLN::Error> arg_aliases_accept_the_underlying_type() {
        static_assert(std::is_same_v<std::decay_t<JPH::Vec3Arg>, JPH::Vec3>, "Vec3Arg stopped being an alias for Vec3");
        static_assert(std::is_same_v<std::decay_t<JPH::QuatArg>, JPH::Quat>, "QuatArg stopped being an alias for Quat");

        auto res = ZHLN::FromScriptVal<JPH::Vec3Arg>(Numbers({1.0, 2.0, 3.0}));

        auto conv = ZHLN::Test::AssertTrue(res.has_value());
        if (!conv) {
            return conv;
        }
        ZHLN::Test::ExpectEq(res->GetX(), 1.0f);
        return {};
    }
};

} // namespace

auto RunScriptValueTypesSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ScriptValueTypesTestSuite>();
}
