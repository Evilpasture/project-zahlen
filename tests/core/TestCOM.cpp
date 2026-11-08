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

struct ThrowingInterface {
    virtual int32_t Bad() const = 0;
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
