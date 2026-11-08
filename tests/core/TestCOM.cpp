// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

enum class COMTestError : uint8_t { DivisionByZero = 1, UnknownInterface = 2 };

struct ITestMath : ZHLN::COM::IObject {
    virtual int32_t Add(int32_t a, int32_t b) const noexcept = 0;
    virtual ZHLN::COM::StringView Name() const noexcept = 0;
    virtual ZHLN::COM::Result<int32_t> Divide(int32_t a, int32_t b) const noexcept = 0;
};

// An interface inheriting an interface: the reflected base walk has to find
// ITestMath one level below ITestExtended, not just the direct bases.
struct ITestExtended : ITestMath {
    virtual int32_t Extra() const noexcept = 0;
};

struct ITestBuffer : ZHLN::COM::IObject {
    virtual uint32_t Sum(ZHLN::COM::Span<const uint8_t> bytes) const noexcept = 0;
};

// A well-formed interface that this object does not implement.
struct ITestMissing : ZHLN::COM::IObject {
    virtual int32_t Missing() const noexcept = 0;
};

// Watches the assignment that destroys it. Release has to happen last, so the
// slot being assigned already names its new object by the time this runs.
ZHLN::COM::Ref<ITestBuffer>* g_assignedSlot          = nullptr;
const void*                  g_slotTargetWhileDying   = nullptr;
bool                         g_observedDestructorRan  = false;

// Three interfaces, no virtual inheritance, no macros, and no handwritten
// QueryInterface switch. Implements aggregates the interface branches so one
// declaration of AddRef/Release/QueryRaw is the final overrider for all of them.
class TestObject final : public ZHLN::COM::Implements<TestObject, ITestExtended, ITestBuffer> {
public:
    int32_t Add(int32_t a, int32_t b) const noexcept override {
        return a + b;
    }

    ZHLN::COM::StringView Name() const noexcept override {
        return "test-object";
    }

    ZHLN::COM::Result<int32_t> Divide(int32_t a, int32_t b) const noexcept override {
        if (b == 0) return COMTestError::DivisionByZero;
        return a / b;
    }

    int32_t Extra() const noexcept override {
        return 7;
    }

    uint32_t Sum(ZHLN::COM::Span<const uint8_t> bytes) const noexcept override {
        uint32_t total = 0;
        for (const uint8_t value : std::span<const uint8_t>(bytes)) {
            total += value;
        }
        return total;
    }
};

class ObservingObject final : public ZHLN::COM::Implements<ObservingObject, ITestBuffer> {
public:
    ~ObservingObject() {
        g_observedDestructorRan = true;
        g_slotTargetWhileDying =
            (g_assignedSlot != nullptr) ? static_cast<const void*>(g_assignedSlot->Get()) : nullptr;
    }

    uint32_t Sum(ZHLN::COM::Span<const uint8_t> bytes) const noexcept override {
        return static_cast<uint32_t>(bytes.size);
    }
};

struct ThrowingInterface {
    virtual int32_t Bad() const = 0;
};

// A fixed-layout struct is fine by value or by reference.
struct TestTransform {
    float position[3];
    float scale;
};

// The accident the parameter audit exists for: noexcept, returns void, and
// still drags a std::string across the boundary by value.
struct StringValueParamInterface {
    virtual void SetTitle(std::string title) noexcept = 0;
};

// Passing it by const reference is no better: the callee still has to agree
// with the caller about std::string's layout and allocator.
struct StringRefParamInterface {
    virtual void SetTitle(const std::string& title) noexcept = 0;
};

// Hidden behind a compliant leading parameter, so the audit cannot stop at the
// first one it likes.
struct LateViolationParamInterface {
    virtual void Write(int32_t slot, std::string payload) noexcept = 0;
};

struct CompliantParamInterface {
    virtual void Apply(const TestTransform& transform, int32_t count) noexcept = 0;
    virtual void Store(TestTransform value, const char* label, void* opaque) noexcept = 0;
    virtual void Nothing() noexcept = 0;
};

struct VirtualDtorInterface {
    virtual int32_t Good() const noexcept = 0;
    virtual ~VirtualDtorInterface() = default;
};

static_assert(ZHLN::COM::AuditInterfaceABI<ITestMath>());
static_assert(ZHLN::COM::AuditInterfaceABI<ITestExtended>());
static_assert(ZHLN::COM::AuditInterfaceABI<ITestBuffer>());
static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<ThrowingInterface>());
static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<VirtualDtorInterface>());
static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<StringValueParamInterface>());
static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<StringRefParamInterface>());
static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<LateViolationParamInterface>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<CompliantParamInterface>());

// The parameter rule itself: pointers are opaque, references are audited as
// what they refer to, values must be fixed-layout.
static_assert(ZHLN::COM::ComParameterCompliant<int32_t>());
static_assert(ZHLN::COM::ComParameterCompliant<const TestTransform&>());
static_assert(ZHLN::COM::ComParameterCompliant<TestTransform>());
static_assert(ZHLN::COM::ComParameterCompliant<void*>());
static_assert(ZHLN::COM::ComParameterCompliant<const std::string*>());
static_assert(!ZHLN::COM::ComParameterCompliant<std::string>());
static_assert(!ZHLN::COM::ComParameterCompliant<const std::string&>());

// A reference return would ask the boundary to express a lifetime it cannot.
static_assert(ZHLN::COM::ComReturnCompliant<void>());
static_assert(ZHLN::COM::ComReturnCompliant<TestTransform>());
static_assert(ZHLN::COM::ComReturnCompliant<ZHLN::COM::IObject*>());
static_assert(!ZHLN::COM::ComReturnCompliant<TestTransform&>());
static_assert(!ZHLN::COM::ComReturnCompliant<std::string>());

// Reflection reports parameters without the implicit object parameter.
static_assert(ZHLN::Reflect::MethodSignature<void, true, int32_t, float>::ParameterCount == 2);
static_assert(std::is_same_v<ZHLN::Reflect::MethodSignature<void, true, int32_t>::ParameterTypes,
                             std::tuple<int32_t>>);
static_assert(ZHLN::COM::InterfaceIdOf<ITestMath>() == ZHLN::COM::InterfaceIdOf<ITestMath>());
static_assert(ZHLN::COM::InterfaceIdOf<ITestMath>() != ZHLN::COM::InterfaceIdOf<ITestBuffer>());
static_assert(std::is_standard_layout_v<ZHLN::COM::InterfaceID>);
static_assert(std::is_standard_layout_v<ZHLN::COM::StringView>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Span<const std::byte>>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Result<int32_t>>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Result<void>>);
static_assert(sizeof(ZHLN::COM::InterfaceID) == 2 * sizeof(uint64_t));
static_assert(sizeof(ZHLN::COM::StringView) == sizeof(const char*) + sizeof(size_t));
static_assert(sizeof(ZHLN::COM::Span<const std::byte>) == sizeof(const std::byte*) + sizeof(size_t));

struct COMTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> reflected_query_finds_direct_and_transitive_interfaces() {
            using ZHLN::COM::Ref;

            auto*            raw    = new TestObject();
            Ref<ITestBuffer> buffer = Ref<ITestBuffer>::Adopt(static_cast<ITestBuffer*>(raw));
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(buffer))) {
                return std::unexpected(COMTestError::UnknownInterface);
            }

            Ref<ITestExtended> extended = buffer.As<ITestExtended>();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(extended))) {
                return std::unexpected(COMTestError::UnknownInterface);
            }
            ZHLN::Test::ExpectEq(extended->Extra(), 7);

            // ITestMath is not a direct base of TestObject. Reflection has to
            // walk ITestExtended to find it.
            Ref<ITestMath> math = extended.As<ITestMath>();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(math))) {
                return std::unexpected(COMTestError::UnknownInterface);
            }
            ZHLN::Test::ExpectEq(math->Add(20, 22), 42);
            ZHLN::Test::ExpectEq(std::string_view(math->Name()), std::string_view("test-object"));

            Ref<ZHLN::COM::IObject> root = buffer.As<ZHLN::COM::IObject>();
            ZHLN::Test::ExpectTrue(static_cast<bool>(root));
            ZHLN::Test::ExpectTrue(root.Get() != nullptr);

            Ref<ITestMissing> missing = buffer.As<ITestMissing>();
            ZHLN::Test::ExpectFalse(static_cast<bool>(missing));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> boundary_vocabulary_converts_without_copying() {
            constexpr std::string_view text = "abcdef";
            const ZHLN::COM::StringView view = text;
            ZHLN::Test::ExpectTrue(view.data == text.data());
            ZHLN::Test::ExpectEq(view.size, text.size());
            ZHLN::Test::ExpectEq(std::string_view(view), text);

            const ZHLN::COM::StringView literal = "literal";
            ZHLN::Test::ExpectEq(std::string_view(literal), std::string_view("literal"));

            std::array<uint8_t, 4> bytes {1, 2, 3, 4};
            const ZHLN::COM::Span<const uint8_t> span = bytes;
            ZHLN::Test::ExpectTrue(span.data == bytes.data());
            ZHLN::Test::ExpectEq(span.size, bytes.size());
            ZHLN::Test::ExpectEq(std::span<const uint8_t>(span).size(), size_t {4});

            auto*                       raw    = new TestObject();
            ZHLN::COM::Ref<ITestBuffer> buffer = ZHLN::COM::Ref<ITestBuffer>::Adopt(static_cast<ITestBuffer*>(raw));
            ZHLN::Test::ExpectEq(buffer->Sum(bytes), uint32_t {10});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> result_carries_the_original_error_code() {
            const ZHLN::COM::Result<int32_t> good = 42;
            ZHLN::Test::ExpectTrue(good.HasValue());
            ZHLN::Test::ExpectEq(good.value, 42);
            const std::expected<int32_t, ZHLN::ErrorCode> asExpected = good;
            ZHLN::Test::ExpectTrue(asExpected.has_value());

            const ZHLN::COM::Result<int32_t> bad = COMTestError::DivisionByZero;
            ZHLN::Test::ExpectFalse(bad.HasValue());
            ZHLN::Test::ExpectTrue(bad.error.Is(COMTestError::DivisionByZero));
            const std::expected<int32_t, ZHLN::ErrorCode> asError = bad;
            ZHLN::Test::ExpectFalse(asError.has_value());
            ZHLN::Test::ExpectTrue(asError.error().Is(COMTestError::DivisionByZero));

            const ZHLN::COM::Result<void> fine;
            ZHLN::Test::ExpectTrue(fine.HasValue());
            const ZHLN::COM::Result<void> failed = COMTestError::UnknownInterface;
            ZHLN::Test::ExpectFalse(failed.HasValue());
            ZHLN::Test::ExpectTrue(failed.error.Is(COMTestError::UnknownInterface));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> assignment_releases_the_old_object_last() {
            using ZHLN::COM::Ref;

            // The discriminating case. Assigning over the last reference to an
            // object runs its destructor inside operator=; with release-first
            // the slot still names the dead object while that destructor runs.
            Ref<ITestBuffer> slot        = Ref<ITestBuffer>::Adopt(static_cast<ITestBuffer*>(new ObservingObject()));
            Ref<ITestBuffer> replacement = Ref<ITestBuffer>::Adopt(static_cast<ITestBuffer*>(new TestObject()));

            g_assignedSlot          = &slot;
            g_slotTargetWhileDying  = nullptr;
            g_observedDestructorRan = false;

            slot = replacement;

            ZHLN::Test::ExpectTrue(g_observedDestructorRan);
            ZHLN::Test::ExpectTrue(g_slotTargetWhileDying == static_cast<const void*>(replacement.Get()));
            ZHLN::Test::ExpectEq(slot.Get(), replacement.Get());
            g_assignedSlot = nullptr;

            // Two independent Refs naming one object: `this != &other` is true,
            // so the guard alone would not have protected this.
            auto*            raw  = new TestObject();
            Ref<ITestBuffer> left = Ref<ITestBuffer>::Adopt(static_cast<ITestBuffer*>(raw));
            Ref<ITestBuffer> right {left.Get()};
            ZHLN::COM::IObject* base = static_cast<ITestBuffer*>(raw);

            left = right;
            ZHLN::Test::ExpectEq(left.Get(), right.Get());
            ZHLN::Test::ExpectEq(base->AddRef(), uint32_t {3});
            ZHLN::Test::ExpectEq(base->Release(), uint32_t {2});

            // Self-assignment through an alias the compiler cannot fold away.
            Ref<ITestBuffer>* self = &left;
            left                   = *self;
            ZHLN::Test::ExpectTrue(static_cast<bool>(left));
            ZHLN::Test::ExpectEq(base->AddRef(), uint32_t {3});
            ZHLN::Test::ExpectEq(base->Release(), uint32_t {2});

            std::array<uint8_t, 3> bytes {1, 2, 3};
            ZHLN::Test::ExpectEq(left->Sum(bytes), uint32_t {6});

            // A move consumes right's reference rather than adding one.
            left = std::move(right);
            ZHLN::Test::ExpectFalse(static_cast<bool>(right));
            ZHLN::Test::ExpectEq(base->AddRef(), uint32_t {2});
            ZHLN::Test::ExpectEq(base->Release(), uint32_t {1});
            ZHLN::Test::ExpectEq(left->Sum(bytes), uint32_t {6});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> ref_counts_survive_copy_move_and_query() {
            using ZHLN::COM::Ref;

            auto*             raw   = new TestObject();
            Ref<ITestBuffer>  owner = Ref<ITestBuffer>::Adopt(static_cast<ITestBuffer*>(raw));
            ZHLN::COM::IObject* base = static_cast<ITestBuffer*>(raw);

            ZHLN::Test::ExpectEq(base->AddRef(), uint32_t {2});
            ZHLN::Test::ExpectEq(base->Release(), uint32_t {1});

            Ref<ITestBuffer> copy = owner;
            ZHLN::Test::ExpectEq(base->AddRef(), uint32_t {3});
            ZHLN::Test::ExpectEq(base->Release(), uint32_t {2});

            Ref<ITestBuffer> moved = std::move(copy);
            ZHLN::Test::ExpectFalse(static_cast<bool>(copy));
            ZHLN::Test::ExpectTrue(static_cast<bool>(moved));
            std::array<uint8_t, 2> pair {5, 6};
            ZHLN::Test::ExpectEq(moved->Sum(pair), uint32_t {11});

            Ref<ITestBuffer> assigned;
            assigned = owner;
            ZHLN::Test::ExpectTrue(static_cast<bool>(assigned));
            assigned = std::move(moved);
            ZHLN::Test::ExpectFalse(static_cast<bool>(moved));

            // owner + assigned are the two remaining references; a query adds
            // and releases one more without disturbing them.
            Ref<ITestMath> queried = owner.As<ITestMath>();
            ZHLN::Test::ExpectTrue(static_cast<bool>(queried));
            queried = Ref<ITestMath> {};
            ZHLN::Test::ExpectFalse(static_cast<bool>(queried));
            return {};
        }
    };
};

} // namespace
