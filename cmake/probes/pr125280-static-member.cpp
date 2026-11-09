// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// cmake/probes/pr125280-static-member.cpp
//
// GCC PR c++/125280 -- "[reflection] -freflection flag causes incomplete type
// errors on class members that reference their owning type within a template
// class parameter".
//
//   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=125280
//
// Reported by Lewis Walton against `gcc version 16.1.1 20260430`, from a case
// found by Jorrit Rouwe -- the author of JoltPhysics, which this tree builds.
// Cause: consteval_only_p() called complete_type() while finishing a static
// data member, but the member's type names the class currently being defined,
// so it cannot be completed yet. Fixed by Marek Polacek on 2026-05-13 in
// r17-498 (de3a3064) and cherry-picked to releases/gcc-16 as r16-8903
// (ce1e520e); upstream keeps this source as g++.dg/reflect/type12.C.
//
// Why this file exists instead of a version floor. The branch `releases/gcc-16`
// reports 16.1.1 both before and after that cherry-pick, so
// `CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.1.1` cannot separate a compiler
// that builds this tree from one that cannot: 16.1.1-20260430 fails and
// 16.1.1-20260514 passes, and both answer "16.1.1". No comparison against a
// version string can express this gate, and a vendor check cannot either --
// the question is not who made the compiler but whether this one miscompiles
// the construct. So the construct is the question.
//
// Compile this with the reflection flags the build uses. Without -freflection
// consteval_only_p() is never reached and every compiler passes, which would
// make the probe meaningless rather than lenient.
//
// Keep this file free of quotes and backslashes: CMake reads it with file(READ)
// and hands it to check_cxx_source_compiles as a quoted argument.

// ---------------------------------------------------------------------------
// Upstream minimal reproduction, verbatim.
// ---------------------------------------------------------------------------
template <class T, int N>
class Array {
  public:
    T mElements[N];
};
class Element {
  public:
    static const Array<Element, 10> mData;
};
const Array<Element, 10> Element::mData;

// ---------------------------------------------------------------------------
// The same construct as extern/JoltPhysics actually spells it: Vec3 declares
// `static const StaticArray<Vec3, 1026> sUnitSphere;` while Vec3 is still
// incomplete. A compiler that fails here stops the build in the precompiled
// header of three targets, several minutes in.
// ---------------------------------------------------------------------------
template <typename T, int N>
struct StaticArray {
  protected:
    int mSize = 0;
    union {
        T             mElements[N];
        unsigned char mDummy;
    };
};

class alignas(16) Vec3 {
  public:
    static const StaticArray<Vec3, 1026> sUnitSphere;
    float                                x, y, z, w;
};

int main() {
    return 0;
}
