// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Shared declarations between tests/core/MangleFixture.cpp (compiled as a
// standalone shared library with -fvisibility=hidden) and tests/core/TestMangle.cpp
// (which never links the fixture and resolves every symbol strictly through
// COM::MangleFunction + SharedLibrary::GetSymbol / COM::ResolveSymbol<Fn>(lib)).

#include <Zahlen/Common.h>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <cstddef>
#include <cstdint>

struct ZHLN_MangleGlobalPod {
    int32_t value = 0;
};

template <typename T>
struct ZHLN_MangleGlobalBox {
    T value {};
};

template <auto V>
struct ZHLN_MangleNttp {};

template <std::size_t N>
struct ZHLN_MangleSizedBuf {
    char data[N] {};
};

template <template <typename> class TT>
struct ZHLN_MangleHigher {};

ZHLN_EXPORT uint32_t ZHLN_MangleGlobalEmpty() noexcept;

ZHLN_EXPORT int32_t ZHLN_MangleGlobalSubst(
    ZHLN_MangleGlobalPod a, const ZHLN_MangleGlobalPod* b, ZHLN_MangleGlobalPod* c
) noexcept;

namespace ZHLN::Test::Mangle {

struct Node {
    int32_t x = 0;
};

struct ConfigPayload {
    int32_t scale = 1;
    int32_t bias  = 0;
};

union Payload {
    int32_t i;
    float   f;
};

enum class SignedEnum : int16_t {
    Neg = -13,
    Pos = 29,
};

enum UnscopedEnum {
    UnscopedZero  = 0,
    UnscopedSeven = 7,
};

using Int4Alias = int[4];

template <typename T>
struct Outer {
    struct Inner {
        int32_t tag = 0;
    };
    template <typename U>
    struct MemberTpl {
        T first {};
        U second {};
    };
};

template <typename A, typename B = int>
struct Defaulted {
    A a {};
    B b {};
};

inline namespace Inl {
struct Tag {
    uint32_t v = 0;
};

ZHLN_EXPORT uint32_t InlineNsEntry(Tag a, const Tag* b) noexcept;
} // namespace Inl

struct StaticHost {
    ZHLN_EXPORT static int32_t Entry(const Node* node, const StaticHost* self) noexcept;
    int32_t        addend = 0;
};

ZHLN_EXPORT COM::Result<int32_t> CreateValue(const COM::ChainInNode* config, int32_t input) noexcept;

ZHLN_EXPORT uint32_t RepeatedQualifiers(
    const Node*          a,
    const Node*          b,
    Node*                c,
    volatile Node*       d,
    const volatile Node* e,
    const volatile Node& f,
    Node* const*         g,
    const Node* const*   h
) noexcept;

ZHLN_EXPORT uint32_t AllBuiltinsAndAliases(
    bool                pBool,
    char                pChar,
    signed char         pSChar,
    unsigned char       pUChar,
    wchar_t             pWChar,
    char8_t             pC8,
    char16_t            pC16,
    char32_t            pC32,
    short               pShort,
    unsigned short      pUShort,
    int                 pInt,
    unsigned int        pUInt,
    long                pLong,
    unsigned long       pULong,
    long long           pLLong,
    unsigned long long  pULLong,
    float               pFloat,
    double              pDouble,
    long double         pLDouble,
    uint8_t             pU8,
    int16_t             pI16,
    uint32_t            pU32,
    int64_t             pI64,
    std::size_t         pSize,
    std::ptrdiff_t      pDiff,
    std::uintptr_t      pUptr,
    std::nullptr_t      pNull,
    std::byte           pByte,
    const int*          pConstIntA,
    int*                pMutInt,
    const int*          pConstIntB,
    const int&          pConstIntRef,
    const volatile int* pCVInt,
    const volatile int& pCVIntRef,
    const std::byte*    pConstByte
) noexcept;

ZHLN_EXPORT uint32_t ArraysAndUnions(
    const int (*a)[4],
    const int (*b)[4],
    const Int4Alias* c,
    const int (*d)[3][4],
    const int (*e)[],
    const Node (*f)[4],
    const Node*    g,
    Payload        h,
    const Payload* k
) noexcept;

ZHLN_EXPORT int32_t FunctionPointers(
    int32_t (*noexceptCbA)(int32_t) noexcept,
    int32_t (*throwingCb)(int32_t),
    int32_t (*noexceptCbB)(int32_t) noexcept,
    void (*voidNoexceptCb)() noexcept,
    COM::CreateAppLayerFn pluginFactory
) noexcept;

ZHLN_EXPORT uint32_t TemplatesAndNttps(
    ZHLN_MangleGlobalBox<float>,
    ZHLN_MangleGlobalBox<int>,
    ZHLN_MangleGlobalBox<float>,
    Outer<float>,
    Outer<int>,
    Outer<int>::Inner,
    Outer<int>::MemberTpl<float>,
    Outer<int>::MemberTpl<double>,
    Outer<char>::MemberTpl<float>,
    Defaulted<char>,
    Defaulted<char, double>,
    ZHLN_MangleNttp<true>,
    ZHLN_MangleNttp<false>,
    ZHLN_MangleNttp<-7>,
    ZHLN_MangleNttp<'Z'>,
    ZHLN_MangleNttp<u8'a'>,
    ZHLN_MangleNttp<static_cast<short>(-2)>,
    ZHLN_MangleNttp<INT64_MIN>,
    ZHLN_MangleNttp<UINT64_MAX>,
    ZHLN_MangleNttp<SignedEnum::Neg>,
    ZHLN_MangleNttp<SignedEnum::Pos>,
    ZHLN_MangleNttp<UnscopedSeven>,
    ZHLN_MangleNttp<nullptr>,
    ZHLN_MangleSizedBuf<64>,
    ZHLN_MangleHigher<ZHLN_MangleGlobalBox>,
    ZHLN_MangleHigher<Outer>
) noexcept;

ZHLN_EXPORT uint32_t TemplateArgBeforeSpec(
    ZHLN_MangleHigher<Outer>, Outer<float>, ZHLN_MangleHigher<Outer>
) noexcept;

ZHLN_EXPORT int32_t References(const Node& a, Node& b, Node&& c, const Node& d) noexcept;

ZHLN_EXPORT int32_t TopLevelConstStripped(int* const ptr, const uint32_t addend) noexcept;

// Overload set disambiguated solely through mangled parameter encodings.
ZHLN_EXPORT uint32_t Overloaded(const Node*) noexcept;

ZHLN_EXPORT uint32_t Overloaded(Node*) noexcept;

ZHLN_EXPORT uint32_t Overloaded(const Node* const*) noexcept;

ZHLN_EXPORT uint32_t Overloaded(char) noexcept;

ZHLN_EXPORT uint32_t Overloaded(signed char) noexcept;

ZHLN_EXPORT uint32_t Overloaded(unsigned char) noexcept;

ZHLN_EXPORT uint32_t Overloaded(uint32_t) noexcept;

ZHLN_EXPORT uint32_t Overloaded(uint64_t) noexcept;

// Base-36 substitution rollover (>37 distinct substitution candidates: S_, S0_..S9_, SA_..SZ_, S10_..).
template <unsigned K>
struct Slot {};

ZHLN_EXPORT uint32_t Base36Rollover(
    Slot<0>,  Slot<1>,  Slot<2>,  Slot<3>,  Slot<4>,  Slot<5>,  Slot<6>,  Slot<7>,  Slot<8>,  Slot<9>,
    Slot<10>, Slot<11>, Slot<12>, Slot<13>, Slot<14>, Slot<15>, Slot<16>, Slot<17>, Slot<18>, Slot<19>,
    Slot<20>, Slot<21>, Slot<22>, Slot<23>, Slot<24>, Slot<25>, Slot<26>, Slot<27>, Slot<28>, Slot<29>,
    Slot<30>, Slot<31>, Slot<32>, Slot<33>, Slot<34>, Slot<35>, Slot<36>, Slot<37>, Slot<38>,
    Slot<0>,  Slot<1>,  Slot<9>,  Slot<10>, Slot<35>, Slot<36>, Slot<37>, Slot<38>
) noexcept;

template <typename>
struct FreshTmpl {};

ZHLN_EXPORT uint32_t NoexceptVsPlainCallback(void (*a)(int) noexcept, void (*b)(int), void (*c)(int) noexcept) noexcept;

ZHLN_EXPORT uint32_t ConstVolatileSub(const volatile int* a, const volatile int* b, const int* c, volatile int* d) noexcept;

ZHLN_EXPORT uint32_t TemplateTemplateBeforeSpec(ZHLN_MangleHigher<FreshTmpl>, FreshTmpl<int>, ZHLN_MangleHigher<FreshTmpl>) noexcept;

// Returns the compiler-emitted in-DSO address of each fixture symbol by index,
// letting TestMangle assert exact bit-for-bit equality against dlsym(mangled).
ZHLN_EXPORT const void* FixtureSymbolAddress(uint32_t index) noexcept;

} // namespace ZHLN::Test::Mangle
