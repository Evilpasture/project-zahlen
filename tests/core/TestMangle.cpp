// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MangleFixture.hpp"
#include "TestsFramework.hpp"
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IErrorRegistry.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/ErrorDescribe.hpp>
#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/Reflection/Callable.hpp>
#include <Zahlen/Core/Reflection/Entity.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <vector>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>


namespace ZHLN::Test {

namespace {

// --- Compile-time ABI & mangling verification --------------------------------

// Plugin entry point mangles to exact Itanium ABI symbol with trailing NUL.
static_assert(
    COM::kCreateAppLayerRawSymbol == "_ZN4ZHLN3COM17CreateAppLayerRawEPKNS0_11ChainInNodeE"
);
static_assert(COM::kCreateAppLayerRawSymbol.data()[COM::kCreateAppLayerRawSymbol.size()] == '\0');
static_assert(
    COM::MangleFunction<&ZHLN_MangleGlobalEmpty>() == "_Z22ZHLN_MangleGlobalEmptyv"
);
static_assert(
    COM::MangleFunction<&ZHLN_MangleGlobalSubst>() ==
    "_Z22ZHLN_MangleGlobalSubst20ZHLN_MangleGlobalPodPKS_PS_"
);
static_assert(
    COM::MangleFunction<&Mangle::CreateValue>() ==
    "_ZN4ZHLN4Test6Mangle11CreateValueEPKNS_3COM11ChainInNodeEi"
);

// --- Domain-neutrality checks for Core/Reflection/{Entity,Callable}.hpp ----
// Constructs that Core/COM/ItaniumMangle rejects by ABI policy (std::string,
// variadic class templates, C variadics, internal linkage, floating-point
// NTTPs) stay cleanly introspectable through Reflect::.
template <typename... Ts>
struct NeutralVariadicPack {};
template <typename T, int... Ns>
struct NeutralHeadAndInts {};
template <typename T, typename U = long>
struct NeutralFixedDefault {};
template <float F>
struct NeutralFloatHolder {};
template <typename T>
struct NeutralOuter {
    template <typename U>
    struct Inner {};
};
using NeutralU32Alias = uint32_t;
using NeutralSlotAlias = Mangle::Slot<7>;
[[maybe_unused]] auto InternalStringFn(int, const double&) -> std::string { return {}; }
[[maybe_unused]] auto CVariadicDecl(int, ...) noexcept -> int { return 0; }

static_assert(Reflect::EntityTokenOf<NeutralU32Alias>() == Reflect::EntityTokenOf<unsigned int>());
static_assert(Reflect::EntityTokenOf<NeutralSlotAlias>() == Reflect::EntityTokenOf<Mangle::Slot<7>>());
static_assert(Reflect::EntityTokenOf<COM::ChainInNode>() != Reflect::EntityTokenOf<const COM::ChainInNode>());
static_assert(Reflect::EntityTokenOf<const COM::ChainInNode>() != Reflect::EntityTokenOf<const COM::ChainInNode*>());
static_assert(Reflect::EntityKindOf<COM::ChainInNode>() == Reflect::EntityKind::Class);
static_assert(Reflect::EntityKindOf<Mangle::Payload>() == Reflect::EntityKind::Union);
static_assert(Reflect::EntityKindOf<Mangle::SignedEnum>() == Reflect::EntityKind::Enum);
static_assert(Reflect::EntityKindOf<int>() == Reflect::EntityKind::Other);
static_assert(Reflect::EntityName<COM::ChainInNode>() == "ChainInNode");
static_assert(Reflect::EntityName<Mangle::Slot<0>>() == "Slot");
static_assert(Reflect::HasIdentifier<Mangle::Slot<0>>());
static_assert(!Reflect::IsFunctionLocal<COM::ChainInNode>());
static_assert(Reflect::IsTemplateSpecialization<Mangle::Slot<0>>());
static_assert(!Reflect::IsVariadicSpecialization<Mangle::Slot<0>>());
static_assert(!Reflect::IsVariadicSpecialization<NeutralFixedDefault<int>>());
static_assert(Reflect::IsVariadicSpecialization<NeutralVariadicPack<>>());
static_assert(Reflect::IsVariadicSpecialization<NeutralVariadicPack<int, float>>());
static_assert(Reflect::IsVariadicSpecialization<NeutralHeadAndInts<int>>());
static_assert(Reflect::IsTemplateSpecialization<NeutralFloatHolder<1.5F>>());
static_assert(Reflect::DescribeEntity<Mangle::Slot<0>>().templateId == Reflect::DescribeEntity<Mangle::Slot<1>>().templateId);
static_assert(Reflect::DescribeEntity<Mangle::Slot<0>>().id != Reflect::DescribeEntity<Mangle::Slot<1>>().id);
static_assert(
    Reflect::DescribeEntity<NeutralOuter<int>::Inner<char>>().templateId !=
    Reflect::DescribeEntity<NeutralOuter<long>::Inner<char>>().templateId
);
static_assert(Reflect::DescribeEntity<std::string>().hasIdentifier);
static_assert(Reflect::DescribeEntity<decltype(&COM::CreateLoggerRaw)>().kind == Reflect::EntityKind::Other);

using InternalTraits = Reflect::FunctionTraits<&InternalStringFn>;
static_assert(InternalTraits::name == "InternalStringFn");
static_assert(!InternalTraits::hasExternalLinkage);
static_assert(!InternalTraits::isNoexcept);
static_assert(!InternalTraits::isCVariadic);
static_assert(InternalTraits::parameterCount == 2);
static_assert(std::is_same_v<InternalTraits::ReturnType, std::string>);
static_assert(std::is_same_v<InternalTraits::ParameterTypes, std::tuple<int, const double&>>);

using CreateLoggerTraits = Reflect::FunctionTraits<&COM::CreateLoggerRaw>;
static_assert(CreateLoggerTraits::name == "CreateLoggerRaw");
static_assert(CreateLoggerTraits::hasExternalLinkage);
static_assert(!std::is_void_v<CreateLoggerTraits::ReturnType>);

using CVarTraits = Reflect::FunctionTraits<&CVariadicDecl>;
static_assert(CVarTraits::isCVariadic && CVarTraits::isNoexcept && CVarTraits::parameterCount == 1);

using CreateLayerTraits = Reflect::FunctionTraits<&COM::CreateAppLayerRaw>;
static_assert(CreateLayerTraits::name == "CreateAppLayerRaw");
static_assert(CreateLayerTraits::hasExternalLinkage);
static_assert(CreateLayerTraits::isNoexcept);
static_assert(!CreateLayerTraits::isStaticMember);
static_assert(!CreateLayerTraits::isGlobalScope);
static_assert(!CreateLayerTraits::isTemplateSpecialization);

// Positive COM free-function ABI audit checks (including by-value Result<T>).
using CreateEngineChainFn = COM::Result<COM::IEngine*> (*)(const COM::ChainInNode*) noexcept;
static_assert(COM::EntryPointContract<static_cast<CreateEngineChainFn>(&COM::CreateEngineRaw)>() != 0);

static_assert(
    COM::MangleFunction<static_cast<CreateEngineChainFn>(&COM::CreateEngineRaw)>() ==
    "_ZN4ZHLN3COM15CreateEngineRawEPKNS0_11ChainInNodeE"
);
static_assert(COM::AuditFunctionABI<&COM::CreateAppLayerRaw>());
static_assert(COM::AuditFunctionABI<static_cast<CreateEngineChainFn>(&COM::CreateEngineRaw)>());
static_assert(COM::AuditFunctionABI<&COM::CreateLogicWorldRaw>());
static_assert(COM::AuditFunctionABI<&COM::RenderGraphDump>());
static_assert(COM::AuditFunctionABI<&Mangle::CreateValue>());
static_assert(COM::AuditFunctionABI<&Mangle::StaticHost::Entry>());

// Negative COM free-function ABI audit checks.
[[maybe_unused]] auto ThrowingFactory(const COM::ChainInNode*) -> COM::Result<COM::IAppLayer*> { return nullptr; }
[[maybe_unused]] auto NonComReturn() noexcept -> std::string { return {}; }
[[maybe_unused]] auto SmartPtrReturn() noexcept -> COM::Ref<COM::IAppLayer> { return {}; }
[[maybe_unused]] void NonComValueParam(std::string) noexcept {}
[[maybe_unused]] void NonComRefParam(const std::string&) noexcept {}
[[maybe_unused]] auto VariadicFn(int, ...) noexcept -> int { return 0; }

static_assert(!COM::AuditFunctionABI<&ThrowingFactory>());
static_assert(!COM::AuditFunctionABI<&NonComReturn>());
static_assert(!COM::AuditFunctionABI<&SmartPtrReturn>());
static_assert(!COM::AuditFunctionABI<&NonComValueParam>());
static_assert(!COM::AuditFunctionABI<&NonComRefParam>());
static_assert(!COM::AuditFunctionABI<&VariadicFn>());
static_assert(!COM::FunctionTypeCompliant<int>());
static_assert(!COM::FunctionTypeCompliant<void*>());

[[nodiscard]] auto OpenMangleFixture(SharedLibrary& out) -> bool {
    std::vector<std::filesystem::path> dirs;
    if (const auto engDir = SharedLibrary::RuntimeModuleDirectory(); !engDir.empty()) {
        dirs.push_back(engDir / "lib");
        dirs.push_back(engDir / "bin");
        dirs.push_back(engDir);
    }
    if (const auto exeDir = FS::Paths::ExecutableDir(); !exeDir.empty()) {
        dirs.push_back(exeDir);
        if (const auto parent = exeDir.parent_path(); !parent.empty() && parent != exeDir) {
            dirs.push_back(parent / "lib");
            dirs.push_back(parent / "bin");
            dirs.push_back(parent);
        }
    }
    auto opened = SharedLibrary::OpenPlugin(
        "ztest_mangle_fixture",
        PluginSearchOptions {
            .stemPrefix = "",
            .searchDirs = dirs,
        }
    );
    if (!opened.has_value() || !opened->has_value()) {
        return false;
    }
    // The fixture must stay open in `out` for the symbol lookups below.
    out = std::move((*opened)->library);
    return true;
}

using FixtureAddrFn = decltype(&Mangle::FixtureSymbolAddress);

template <auto Fn>
[[nodiscard]] auto ResolveChecked(
    const SharedLibrary& lib, FixtureAddrFn addrOf, uint32_t expectedSlot
) -> decltype(Fn) {
    constexpr std::string_view mangled = COM::MangleFunction<Fn>();
    static_assert(!mangled.empty());
    static_assert(mangled.data()[mangled.size()] == '\0');
    void* const       raw      = lib.GetSymbol(mangled.data());
    const auto        typed    = COM::ResolveSymbol<Fn>(lib);
    const void* const expected = addrOf ? addrOf(expectedSlot) : nullptr;
    ExpectTrue(raw != nullptr);
    ExpectTrue(typed != nullptr);
    ExpectTrue(raw == reinterpret_cast<void*>(typed));
    ExpectTrue(raw == expected);
    return typed;
}

auto AddOneNoexcept(int32_t x) noexcept -> int32_t { return x + 1; }
auto DoubleThrowing(int32_t x) -> int32_t { return x * 2; }
bool g_voidCallbackHit = false;
void MarkVoidNoexcept() noexcept { g_voidCallbackHit = true; }

} // namespace

struct UnknownMangleExt {
    uint64_t cookie = 1;
};

void test_mangle_dso_symbol_resolution_and_invocation() {
    SharedLibrary lib;
    if (!ExpectTrue(OpenMangleFixture(lib))) return;
    const auto addrOf = COM::ResolveSymbol<&Mangle::FixtureSymbolAddress>(lib);
    if (!ExpectTrue(addrOf != nullptr)) return;
    ExpectTrue(reinterpret_cast<const void*>(addrOf) == addrOf(22u));

    // 1. Global zero-parameter and global substitution functions.
    if (const auto fn = ResolveChecked<&ZHLN_MangleGlobalEmpty>(lib, addrOf, 0u)) {
        ExpectEq(fn(), uint32_t {0xA11CE001u});
    }
    if (const auto fn = ResolveChecked<&ZHLN_MangleGlobalSubst>(lib, addrOf, 1u)) {
        const ZHLN_MangleGlobalPod b {.value = 12};
        ZHLN_MangleGlobalPod       c {};
        ExpectEq(fn({.value = 30}, &b, &c), int32_t {42});
        ExpectEq(c.value, int32_t {84});
    }

    // 2. Inline namespace & static member function.
    if (const auto fn = ResolveChecked<&Mangle::Inl::InlineNsEntry>(lib, addrOf, 2u)) {
        const Mangle::Inl::Tag b {.v = 9u};
        ExpectEq(fn({.v = 6u}, &b), uint32_t {0x10Fu});
    }
    if (const auto fn = ResolveChecked<&Mangle::StaticHost::Entry>(lib, addrOf, 3u)) {
        const Mangle::Node       n {.x = 19};
        const Mangle::StaticHost h {.addend = 23};
        ExpectEq(fn(&n, &h), int32_t {42});
    }

    // 3. By-value COM::Result<T> across DSO boundary (success + typed error).
    if (const auto fn = ResolveChecked<&Mangle::CreateValue>(lib, addrOf, 4u)) {
        const auto ok = fn(COM::InChain(Mangle::ConfigPayload {.scale = 4, .bias = 7}), 9);
        ExpectTrue(ok.HasValue());
        if (ok.HasValue()) ExpectEq(ok.value, int32_t {43});

        const auto err = fn(COM::InChain(UnknownMangleExt {}), 9);
        ExpectFalse(err.HasValue());
        if (!err.HasValue()) ExpectTrue(err.error == COM::Error::UnsupportedExtension);
    }

    // 4. Layered cv + pointer + reference substitutions (K, V, VK, P, PK, R).
    if (const auto fn = ResolveChecked<&Mangle::RepeatedQualifiers>(lib, addrOf, 5u)) {
        Mangle::Node        n {.x = 0x55};
        Mangle::Node*       np  = &n;
        const Mangle::Node* cnp = &n;
        ExpectEq(fn(&n, &n, &n, &n, &n, n, &np, &cnp), uint32_t {0xB0000055u});
    }

    // 5. Full dealias'd builtin + fixed-width/size_t/nullptr_t/std::byte coverage.
    if (const auto fn = ResolveChecked<&Mangle::AllBuiltinsAndAliases>(lib, addrOf, 6u)) {
        int       iv = 10;
        std::byte bv {5};
        const uint32_t got = fn(
            true, 'a', static_cast<signed char>(-1), static_cast<unsigned char>(2),
            L'w', u8'u', u's', U'i',
            static_cast<short>(-3), static_cast<unsigned short>(4),
            -5, 6u, -7L, 8UL, -9LL, 10ULL,
            1.0f, 2.0, 3.0L,
            uint8_t {1}, int16_t {-2}, uint32_t {100}, int64_t {-4},
            std::size_t {20}, std::ptrdiff_t {-1}, std::uintptr_t {0},
            nullptr, bv, &iv, &iv, &iv, iv, &iv, iv, &bv
        );
        ExpectEq(got, uint32_t {136u});
    }

    // 6. Bounded, aliased, multi-dimensional & unbounded const arrays + unions.
    if (const auto fn = ResolveChecked<&Mangle::ArraysAndUnions>(lib, addrOf, 7u)) {
        const int          a4[4]    = {1, 2, 3, 9};
        const int          a34[3][4] = {};
        const Mangle::Node n4[4]    = {};
        const Mangle::Node g {.x = 11};
        Mangle::Payload    p {};
        p.i = 22;
        ExpectEq(fn(&a4, &a4, &a4, &a34, nullptr, &n4, &g, p, &p), uint32_t {42u});
    }

    // 7. Function pointer parameters (C++17 DoF...E vs throwing F...E + CreateAppLayerRawFn).
    if (const auto fn = ResolveChecked<&Mangle::FunctionPointers>(lib, addrOf, 8u)) {
        g_voidCallbackHit = false;
        ExpectEq(fn(&AddOneNoexcept, &DoubleThrowing, &AddOneNoexcept, &MarkVoidNoexcept, nullptr), int32_t {57});
        ExpectTrue(g_voidCallbackHit);
    }

    // 8. Class templates, nested/member templates, template-template args, and NTTPs.
    if (const auto fn = ResolveChecked<&Mangle::TemplatesAndNttps>(lib, addrOf, 9u)) {
        const uint32_t got = fn(
            {1.0f}, {7}, {2.0f},
            {}, {}, {.tag = 8},
            {}, {}, {},
            {}, {},
            {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
            {}, {}, {}
        );
        ExpectEq(got, uint32_t {0xC0FFEE0Fu});
    }
    if (const auto fn = ResolveChecked<&Mangle::TemplateArgBeforeSpec>(lib, addrOf, 10u)) {
        ExpectEq(fn({}, {}, {}), uint32_t {0x77AA5511u});
    }

    // 9. Lvalue/rvalue references & top-level const parameter stripping.
    if (const auto fn = ResolveChecked<&Mangle::References>(lib, addrOf, 11u)) {
        const Mangle::Node a {.x = 3};
        Mangle::Node       b {.x = 10};
        const Mangle::Node d {.x = 4};
        ExpectEq(fn(a, b, Mangle::Node {.x = 5}, d), int32_t {22});
        ExpectEq(b.x, int32_t {15});
    }
    if (const auto fn = ResolveChecked<&Mangle::TopLevelConstStripped>(lib, addrOf, 12u)) {
        int v = 33;
        ExpectEq(fn(&v, 9u), int32_t {42});
    }

    // 10. Eight-way overload resolution by mangled parameter list alone.
    const auto ov1 = ResolveChecked<static_cast<uint32_t (*)(const Mangle::Node*) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 13u);
    const auto ov2 = ResolveChecked<static_cast<uint32_t (*)(Mangle::Node*) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 14u);
    const auto ov3 = ResolveChecked<static_cast<uint32_t (*)(const Mangle::Node* const*) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 15u);
    const auto ov4 = ResolveChecked<static_cast<uint32_t (*)(char) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 16u);
    const auto ov5 = ResolveChecked<static_cast<uint32_t (*)(signed char) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 17u);
    const auto ov6 = ResolveChecked<static_cast<uint32_t (*)(unsigned char) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 18u);
    const auto ov7 = ResolveChecked<static_cast<uint32_t (*)(uint32_t) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 19u);
    const auto ov8 = ResolveChecked<static_cast<uint32_t (*)(uint64_t) noexcept>(&Mangle::Overloaded)>(lib, addrOf, 20u);
    if (ov1 && ov2 && ov3 && ov4 && ov5 && ov6 && ov7 && ov8) {
        ExpectEq(ov1(nullptr), uint32_t {101u});
        ExpectEq(ov2(nullptr), uint32_t {102u});
        ExpectEq(ov3(nullptr), uint32_t {103u});
        ExpectEq(ov4('x'), uint32_t {104u});
        ExpectEq(ov5(static_cast<signed char>(-1)), uint32_t {105u});
        ExpectEq(ov6(static_cast<unsigned char>(1)), uint32_t {106u});
        ExpectEq(ov7(uint32_t {1}), uint32_t {107u});
        ExpectEq(ov8(uint64_t {1}), uint32_t {108u});
    }

    // 11. Base-36 substitution sequence IDs past SZ_ -> S10_, S11_, ...
    if (const auto fn = ResolveChecked<&Mangle::Base36Rollover>(lib, addrOf, 21u)) {
        ExpectEq(
            fn(
                {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                {}, {}, {}, {}, {}, {}, {}, {}, {},
                {}, {}, {}, {}, {}, {}, {}, {}
            ),
            uint32_t {0x36363636u}
        );
    }

    // 12. noexcept-vs-plain callback substitution, combined const-volatile
    //     substitution, and template-template arg recorded before specialization.
    if (const auto fn = ResolveChecked<&Mangle::NoexceptVsPlainCallback>(lib, addrOf, 23u)) {
        const auto neCb = +[](int) noexcept {};
        const auto plCb = +[](int) {};
        ExpectEq(fn(neCb, plCb, neCb), uint32_t {0xA11CEu});
    }
    if (const auto fn = ResolveChecked<&Mangle::ConstVolatileSub>(lib, addrOf, 24u)) {
        const volatile int cv = 1;
        const int          ci = 5;
        volatile int       vi = 7;
        ExpectEq(fn(&cv, &cv, &ci, &vi), uint32_t {22u});
    }
    if (const auto fn = ResolveChecked<&Mangle::TemplateTemplateBeforeSpec>(lib, addrOf, 25u)) {
        ExpectEq(fn({}, {}, {}), uint32_t {0x5B57u});
    }
}

namespace {
template <auto Fn>
[[nodiscard]] auto ExpectExactEngineSymbol(
    const SharedLibrary&         engineLib,
    const std::filesystem::path& expectedModule
) -> decltype(Fn) {
    constexpr std::string_view mangled    = COM::MangleFunction<Fn>();
    void* const                fromHandle = engineLib.GetSymbol(mangled.data());
    const auto                 typed      = COM::ResolveSymbol<Fn>(engineLib);
    ExpectTrue(fromHandle != nullptr);
    ExpectTrue(fromHandle == reinterpret_cast<void*>(typed));
    if (fromHandle != nullptr) {
        std::error_code ec;
        const auto      owner = SharedLibrary::ModulePath(fromHandle);
        ExpectFalse(owner.empty());
        if (!owner.empty()) {
            ExpectTrue(std::filesystem::equivalent(owner, expectedModule, ec) && !ec);
        }
    }
    return typed;
}
} // namespace

void test_mangle_matches_live_engine_dso_exports() {
    const auto enginePath = SharedLibrary::RuntimeModulePath();
    if (!ExpectFalse(enginePath.empty())) return;

    SharedLibrary engineLib;
    if (!ExpectTrue(engineLib.Open(enginePath, SharedLibraryBind::Local))) return;

    // Verify exact symbol name and in-DSO code address against libzahlen_engine.so.
    (void) ExpectExactEngineSymbol<static_cast<CreateEngineChainFn>(&COM::CreateEngineRaw)>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&COM::CreateLogicWorldRaw>(engineLib, enginePath);
    const auto rgDump = ExpectExactEngineSymbol<&COM::RenderGraphDump>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&COM::CreateErrorRegistryRaw>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&COM::CreateLoggerRaw>(engineLib, enginePath);
    // Layer 0 surface: the sink/level entry points every consumer DSO resolves
    // to route its ambient lines. Only the ABI-clean signatures are asserted
    // here -- DescribeStaticError / WriteAmbientLog / RegisterStaticErrorCategoryRaw
    // carry std:: types by design, so ItaniumMangle refuses them; their export
    // rules are pinned by cmake/zahlen_engine.map via check_engine_exports.
    (void) ExpectExactEngineSymbol<static_cast<void (*)(ZHLN::LogSink) noexcept>(&ZHLN::SetLogSink)>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&ZHLN::GetLogSink>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&ZHLN::SetLogLevel>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&ZHLN::AmbientLogEnabled>(engineLib, enginePath);

    const auto pageSz = ExpectExactEngineSymbol<&GetPageSize>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&AllocatePages>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&ProtectPages>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&FreePages>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&AllocateGuardedRegion>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&FreeGuardedRegion>(engineLib, enginePath);
    const auto getPid = ExpectExactEngineSymbol<&GetPID>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&HaltThread>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&Platform::Init>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&CPUProfiler::IterateMetrics>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&TaskSystem::GetWorkerIndex>(engineLib, enginePath);
    const auto workers = ExpectExactEngineSymbol<&TaskSystem::GetWorkerCount>(engineLib, enginePath);
    (void) ExpectExactEngineSymbol<&TaskSystem::Shutdown>(engineLib, enginePath);

    // Invoke safe side-effect-free getters and verify identical return values.
    if (pageSz) ExpectEq(pageSz(), GetPageSize());
    if (getPid) ExpectEq(getPid(), GetPID());
    if (workers) ExpectEq(workers(), TaskSystem::GetWorkerCount());
    if (rgDump) ExpectEq(std::string_view(rgDump(AAMode::None)), std::string_view(COM::RenderGraphDump(AAMode::None)));
}
} // namespace ZHLN::Test
