// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace COMIdentityTest {

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

template <typename T> struct ITemplate : ZHLN::COM::IObject {};

// A well-formed interface that this object does not implement.
struct ITestMissing : ZHLN::COM::IObject {
    virtual int32_t Missing() const noexcept = 0;
};

struct ChainNodeA {
    uint32_t width  = 0;
    uint32_t height = 0;
};

struct ChainNodeB {
    float exposure = 1.0F;
    bool  hdr      = false;
};

struct ChainNodeUnknown {
    uint64_t secret = 42;
};

// Recursive leaf LayoutHash checks: same outer sizeof/alignof with altered
// nested subobject leaf geometry must produce distinct hashes, while restated
// identical leaf layouts (e.g. 4x4 float matrix vs flat float[16]) must agree.
struct NestedSliceV1 {
    uint32_t count = 0;
    float    scale = 1.0F;
};
struct NestedSliceDrifted {
    float    scale = 1.0F;
    uint16_t flags = 0;
    uint16_t extra = 0;
};
struct OuterV1 {
    uint64_t      header = 0;
    NestedSliceV1 nested {};
};
struct OuterDrifted {
    uint64_t           header = 0;
    NestedSliceDrifted nested {};
};

struct MatrixRows {
    float rows[4][4] {};
};
struct MatrixFlat {
    float elems[16] {};
};
struct WrapMatrixRows {
    MatrixRows world {};
    MatrixRows prev {};
};
struct WrapMatrixFlat {
    MatrixFlat world {};
    MatrixFlat prev {};
};

} // namespace COMIdentityTest

namespace {
using namespace COMIdentityTest;

static_assert(ZHLN::COM::InterfaceIdOf<ITestMath>().high == ZHLN::Hash64("COMIdentityTest::ITestMath"));
static_assert(ZHLN::COM::InterfaceIdOf<ITestMath>() != ZHLN::COM::InterfaceIdOf<ITestExtended>());
static_assert(ZHLN::COM::InterfaceIdOf<ITemplate<int>>() != ZHLN::COM::InterfaceIdOf<ITemplate<float>>());

static_assert(sizeof(OuterV1) == sizeof(OuterDrifted) && alignof(OuterV1) == alignof(OuterDrifted));
static_assert(ZHLN::Reflect::LayoutHash<OuterV1>() != ZHLN::Reflect::LayoutHash<OuterDrifted>());
static_assert(ZHLN::Reflect::LayoutHash<WrapMatrixRows>() == ZHLN::Reflect::LayoutHash<WrapMatrixFlat>());

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
    virtual void Configure(const ZHLN::COM::ChainInNode* inChain, ZHLN::COM::ChainOutNode* outChain) noexcept = 0;
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
static_assert(ZHLN::COM::ComParameterCompliant<const ZHLN::COM::ChainInNode*>());
static_assert(ZHLN::COM::ComParameterCompliant<ZHLN::COM::ChainOutNode*>());
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
static_assert(std::is_standard_layout_v<ZHLN::COM::ChainInNode>);
static_assert(std::is_standard_layout_v<ZHLN::COM::ChainOutNode>);
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

        std::expected<void, ZHLN::ErrorCode> chain_protocol_dispatches_and_rejects_drift() {
            // Empty input chain succeeds as a no-op.
            int visited = 0;
            const ZHLN::COM::Result<void> emptyOutcome =
                ZHLN::COM::DispatchInChain<ChainNodeA, ChainNodeB>(
                    ZHLN::COM::InChain(), [&](const auto&) -> ZHLN::COM::Result<void> {
                        ++visited;
                        return {};
                    });
            ZHLN::Test::ExpectTrue(emptyOutcome.HasValue());
            ZHLN::Test::ExpectEq(visited, 0);

            // Multi-node input chain forwards both lvalue and rvalue nodes.
            const ChainNodeA lvalueA {.width = 1920, .height = 1080};
            uint32_t         gotW        = 0;
            uint32_t         gotH        = 0;
            float            gotExposure = 0.0F;
            bool             gotHdr      = false;

            const ZHLN::COM::Result<void> validOutcome = ZHLN::COM::DispatchInChain<ChainNodeA, ChainNodeB>(
                ZHLN::COM::InChain(lvalueA, ChainNodeB {.exposure = 2.25F, .hdr = true}),
                [&]<typename NodeT>(const NodeT& node) -> ZHLN::COM::Result<void> {
                    if constexpr (std::is_same_v<NodeT, ChainNodeA>) {
                        gotW = node.width;
                        gotH = node.height;
                    } else if constexpr (std::is_same_v<NodeT, ChainNodeB>) {
                        gotExposure = node.exposure;
                        gotHdr      = node.hdr;
                    }
                    return {};
                });
            ZHLN::Test::ExpectTrue(validOutcome.HasValue());
            ZHLN::Test::ExpectEq(gotW, uint32_t {1920});
            ZHLN::Test::ExpectEq(gotH, uint32_t {1080});
            ZHLN::Test::ExpectEq(gotExposure, 2.25F);
            ZHLN::Test::ExpectTrue(gotHdr);

            // Unknown extension node is rejected with ComError::UnsupportedExtension.
            const ZHLN::COM::Result<void> unknownOutcome = ZHLN::COM::DispatchInChain<ChainNodeA>(
                ZHLN::COM::InChain(ChainNodeA {.width = 1, .height = 1}, ChainNodeUnknown {}),
                [&](const ChainNodeA&) -> ZHLN::COM::Result<void> { return {}; });
            ZHLN::Test::ExpectFalse(unknownOutcome.HasValue());
            ZHLN::Test::ExpectTrue(unknownOutcome.error.Is(ZHLN::COM::ComError::UnsupportedExtension));

            // Duplicate node type in manual chain is rejected with ComError::DuplicateExtension.
            ChainNodeA             dupPayload {.width = 1, .height = 1};
            ZHLN::COM::ChainInNode tailNode {
                .sType      = ZHLN::COM::ChainTypeID<ChainNodeA>(),
                .layoutHash = ZHLN::Reflect::LayoutHash<ChainNodeA>(),
                .size       = sizeof(ChainNodeA),
                .reserved   = 0,
                .data       = &dupPayload,
                .pNext      = nullptr,
            };
            ZHLN::COM::ChainInNode headNode = tailNode;
            headNode.pNext                  = &tailNode;
            const ZHLN::COM::Result<void> dupOutcome = ZHLN::COM::DispatchInChain<ChainNodeA>(
                &headNode, [&](const ChainNodeA&) -> ZHLN::COM::Result<void> { return {}; });
            ZHLN::Test::ExpectFalse(dupOutcome.HasValue());
            ZHLN::Test::ExpectTrue(dupOutcome.error.Is(ZHLN::COM::ComError::DuplicateExtension));

            // Tampered layoutHash or size is rejected with ComError::LayoutMismatch.
            ZHLN::COM::ChainInNode driftedNode = tailNode;
            driftedNode.layoutHash ^= 0x1ULL;
            const ZHLN::COM::Result<void> hashMismatch = ZHLN::COM::DispatchInChain<ChainNodeA>(
                &driftedNode, [&](const ChainNodeA&) -> ZHLN::COM::Result<void> { return {}; });
            ZHLN::Test::ExpectFalse(hashMismatch.HasValue());
            ZHLN::Test::ExpectTrue(hashMismatch.error.Is(ZHLN::COM::ComError::LayoutMismatch));

            driftedNode.layoutHash = tailNode.layoutHash;
            driftedNode.size       = sizeof(ChainNodeA) + 4U;
            const ZHLN::COM::Result<void> sizeMismatch = ZHLN::COM::DispatchInChain<ChainNodeA>(
                &driftedNode, [&](const ChainNodeA&) -> ZHLN::COM::Result<void> { return {}; });
            ZHLN::Test::ExpectFalse(sizeMismatch.HasValue());
            ZHLN::Test::ExpectTrue(sizeMismatch.error.Is(ZHLN::COM::ComError::LayoutMismatch));

            // Null payload pointer is rejected with ComError::NullArgument.
            driftedNode.size = sizeof(ChainNodeA);
            driftedNode.data = nullptr;
            const ZHLN::COM::Result<void> nullData = ZHLN::COM::DispatchInChain<ChainNodeA>(
                &driftedNode, [&](const ChainNodeA&) -> ZHLN::COM::Result<void> { return {}; });
            ZHLN::Test::ExpectFalse(nullData.HasValue());
            ZHLN::Test::ExpectTrue(nullData.error.Is(ZHLN::COM::ComError::NullArgument));

            // Output chain rejects null head and populates caller-owned nodes in place.
            const ZHLN::COM::Result<void> nullOutHead = ZHLN::COM::DispatchOutChain<ChainNodeA>(
                nullptr, [&](ChainNodeA&) -> ZHLN::COM::Result<void> { return {}; });
            ZHLN::Test::ExpectFalse(nullOutHead.HasValue());
            ZHLN::Test::ExpectTrue(nullOutHead.error.Is(ZHLN::COM::ComError::NullArgument));

            ChainNodeA outA {};
            ChainNodeB outB {};
            const ZHLN::COM::Result<void> outOutcome = ZHLN::COM::DispatchOutChain<ChainNodeA, ChainNodeB>(
                ZHLN::COM::OutChain(outA, outB), [&]<typename NodeT>(NodeT& dst) -> ZHLN::COM::Result<void> {
                    if constexpr (std::is_same_v<NodeT, ChainNodeA>) {
                        dst.width  = 2560;
                        dst.height = 1440;
                    } else if constexpr (std::is_same_v<NodeT, ChainNodeB>) {
                        dst.exposure = 0.75F;
                        dst.hdr      = true;
                    }
                    return {};
                });
            ZHLN::Test::ExpectTrue(outOutcome.HasValue());
            ZHLN::Test::ExpectEq(outA.width, uint32_t {2560});
            ZHLN::Test::ExpectEq(outA.height, uint32_t {1440});
            ZHLN::Test::ExpectEq(outB.exposure, 0.75F);
            ZHLN::Test::ExpectTrue(outB.hdr);
            return {};
        }
    };
};

} // namespace
