// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MangleFixture.hpp"

auto ZHLN_MangleGlobalEmpty() noexcept -> uint32_t {
    return 0xA11CE001u;
}

auto ZHLN_MangleGlobalSubst(
    ZHLN_MangleGlobalPod a, const ZHLN_MangleGlobalPod* b, ZHLN_MangleGlobalPod* c
) noexcept -> int32_t {
    const int32_t sum = a.value + (b ? b->value : 0);
    if (c != nullptr) c->value = sum * 2;
    return sum;
}

namespace ZHLN::Test::Mangle {

namespace Inl {
auto InlineNsEntry(Tag a, const Tag* b) noexcept -> uint32_t {
    return a.v + (b ? b->v : 0u) + 0x100u;
}
} // namespace Inl

auto StaticHost::Entry(const Node* node, const StaticHost* self) noexcept -> int32_t {
    return (node ? node->x : 0) + (self ? self->addend : 0);
}

auto CreateValue(const COM::ChainInNode* config, int32_t input) noexcept -> COM::Result<int32_t> {
    ConfigPayload cfg {};
    const auto    checked = COM::DispatchInChain<ConfigPayload>(
        config, [&](const ConfigPayload& value) noexcept { cfg = value; }
    );
    if (!checked.HasValue()) {
        return checked.error;
    }
    return input * cfg.scale + cfg.bias;
}

auto RepeatedQualifiers(
    const Node*, const Node*, Node*, volatile Node*, const volatile Node*,
    const volatile Node& f, Node* const*, const Node* const*
) noexcept -> uint32_t {
    return 0xB0000000u + static_cast<uint32_t>(f.x);
}

auto AllBuiltinsAndAliases(
    bool pBool, char, signed char, unsigned char, wchar_t, char8_t, char16_t, char32_t,
    short, unsigned short, int, unsigned int, long, unsigned long, long long, unsigned long long,
    float, double, long double, uint8_t, int16_t, uint32_t pU32, int64_t,
    std::size_t pSize, std::ptrdiff_t, std::uintptr_t, std::nullptr_t, std::byte pByte,
    const int*, int*, const int*, const int& pConstIntRef,
    const volatile int*, const volatile int&, const std::byte*
) noexcept -> uint32_t {
    return (pBool ? 1u : 0u) + pU32 + static_cast<uint32_t>(pSize) +
           static_cast<uint32_t>(std::to_integer<uint8_t>(pByte)) +
           static_cast<uint32_t>(pConstIntRef);
}

auto ArraysAndUnions(
    const int (*a)[4], const int (*)[4], const Int4Alias*, const int (*)[3][4],
    const int (*)[], const Node (*)[4], const Node* g, Payload h, const Payload*
) noexcept -> uint32_t {
    return static_cast<uint32_t>((a ? (*a)[3] : 0) + (g ? g->x : 0) + h.i);
}

auto FunctionPointers(
    int32_t (*noexceptCbA)(int32_t) noexcept,
    int32_t (*throwingCb)(int32_t),
    int32_t (*noexceptCbB)(int32_t) noexcept,
    void (*voidNoexceptCb)() noexcept,
    COM::CreateAppLayerRawFn
) noexcept -> int32_t {
    if (voidNoexceptCb) voidNoexceptCb();
    return (noexceptCbA ? noexceptCbA(10) : 0) +
           (throwingCb ? throwingCb(20) : 0) +
           (noexceptCbB ? noexceptCbB(5) : 0);
}

auto TemplatesAndNttps(
    ZHLN_MangleGlobalBox<float>, ZHLN_MangleGlobalBox<int> bi, ZHLN_MangleGlobalBox<float>,
    Outer<float>, Outer<int>, Outer<int>::Inner inner,
    Outer<int>::MemberTpl<float>, Outer<int>::MemberTpl<double>, Outer<char>::MemberTpl<float>,
    Defaulted<char>, Defaulted<char, double>,
    ZHLN_MangleNttp<true>, ZHLN_MangleNttp<false>, ZHLN_MangleNttp<-7>,
    ZHLN_MangleNttp<'Z'>, ZHLN_MangleNttp<u8'a'>, ZHLN_MangleNttp<static_cast<short>(-2)>,
    ZHLN_MangleNttp<INT64_MIN>, ZHLN_MangleNttp<UINT64_MAX>,
    ZHLN_MangleNttp<SignedEnum::Neg>, ZHLN_MangleNttp<SignedEnum::Pos>,
    ZHLN_MangleNttp<UnscopedSeven>, ZHLN_MangleNttp<nullptr>,
    ZHLN_MangleSizedBuf<64>, ZHLN_MangleHigher<ZHLN_MangleGlobalBox>, ZHLN_MangleHigher<Outer>
) noexcept -> uint32_t {
    return 0xC0FFEE00u + static_cast<uint32_t>(bi.value + inner.tag);
}

auto TemplateArgBeforeSpec(
    ZHLN_MangleHigher<Outer>, Outer<float>, ZHLN_MangleHigher<Outer>
) noexcept -> uint32_t {
    return 0x77AA5511u;
}

auto References(const Node& a, Node& b, Node&& c, const Node& d) noexcept -> int32_t {
    b.x += c.x;
    return a.x + b.x + d.x;
}

auto TopLevelConstStripped(int* const ptr, const uint32_t addend) noexcept -> int32_t {
    return (ptr ? *ptr : 0) + static_cast<int32_t>(addend);
}

auto Overloaded(const Node*) noexcept -> uint32_t        { return 101u; }
auto Overloaded(Node*) noexcept -> uint32_t              { return 102u; }
auto Overloaded(const Node* const*) noexcept -> uint32_t { return 103u; }
auto Overloaded(char) noexcept -> uint32_t               { return 104u; }
auto Overloaded(signed char) noexcept -> uint32_t        { return 105u; }
auto Overloaded(unsigned char) noexcept -> uint32_t      { return 106u; }
auto Overloaded(uint32_t) noexcept -> uint32_t           { return 107u; }
auto Overloaded(uint64_t) noexcept -> uint32_t           { return 108u; }

auto Base36Rollover(
    Slot<0>,  Slot<1>,  Slot<2>,  Slot<3>,  Slot<4>,  Slot<5>,  Slot<6>,  Slot<7>,  Slot<8>,  Slot<9>,
    Slot<10>, Slot<11>, Slot<12>, Slot<13>, Slot<14>, Slot<15>, Slot<16>, Slot<17>, Slot<18>, Slot<19>,
    Slot<20>, Slot<21>, Slot<22>, Slot<23>, Slot<24>, Slot<25>, Slot<26>, Slot<27>, Slot<28>, Slot<29>,
    Slot<30>, Slot<31>, Slot<32>, Slot<33>, Slot<34>, Slot<35>, Slot<36>, Slot<37>, Slot<38>,
    Slot<0>,  Slot<1>,  Slot<9>,  Slot<10>, Slot<35>, Slot<36>, Slot<37>, Slot<38>
) noexcept -> uint32_t {
    return 0x36363636u;
}

auto NoexceptVsPlainCallback(void (*a)(int) noexcept, void (*b)(int), void (*c)(int) noexcept) noexcept -> uint32_t {
    return (a == c && b != nullptr) ? 0xA11CEu : 0u;
}

auto ConstVolatileSub(const volatile int* a, const volatile int* b, const int* c, volatile int* d) noexcept -> uint32_t {
    return static_cast<uint32_t>((a == b ? 10 : 0) + (c ? *c : 0) + (d ? *d : 0));
}

auto TemplateTemplateBeforeSpec(ZHLN_MangleHigher<FreshTmpl>, FreshTmpl<int>, ZHLN_MangleHigher<FreshTmpl>) noexcept -> uint32_t {
    return 0x5B57u;
}

auto FixtureSymbolAddress(uint32_t index) noexcept -> const void* {
    const void* const kTable[] = {
        reinterpret_cast<const void*>(&ZHLN_MangleGlobalEmpty),
        reinterpret_cast<const void*>(&ZHLN_MangleGlobalSubst),
        reinterpret_cast<const void*>(&Inl::InlineNsEntry),
        reinterpret_cast<const void*>(&StaticHost::Entry),
        reinterpret_cast<const void*>(&CreateValue),
        reinterpret_cast<const void*>(&RepeatedQualifiers),
        reinterpret_cast<const void*>(&AllBuiltinsAndAliases),
        reinterpret_cast<const void*>(&ArraysAndUnions),
        reinterpret_cast<const void*>(&FunctionPointers),
        reinterpret_cast<const void*>(&TemplatesAndNttps),
        reinterpret_cast<const void*>(&TemplateArgBeforeSpec),
        reinterpret_cast<const void*>(&References),
        reinterpret_cast<const void*>(&TopLevelConstStripped),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(const Node*) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(Node*) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(const Node* const*) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(char) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(signed char) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(unsigned char) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(uint32_t) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(static_cast<uint32_t (*)(uint64_t) noexcept>(&Overloaded)),
        reinterpret_cast<const void*>(&Base36Rollover),
        reinterpret_cast<const void*>(&FixtureSymbolAddress),
        reinterpret_cast<const void*>(&NoexceptVsPlainCallback),
        reinterpret_cast<const void*>(&ConstVolatileSub),
        reinterpret_cast<const void*>(&TemplateTemplateBeforeSpec),
    };
    return (index < (sizeof(kTable) / sizeof(kTable[0]))) ? kTable[index] : nullptr;
}

} // namespace ZHLN::Test::Mangle
