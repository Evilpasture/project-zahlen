// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// resources/shaders/cxx_abi.slang
//
// The host-mapping vocabulary: how the generated C++ spells what Slang
// declares. zshader's gpu-types mode reads these off the reflected fields,
// so the spelling sits on the member it describes and a Slang edit carries
// it along -- there is no table in the tool restating them.
//
// The defaults need no annotation: float2/float3 become std::array<float, N>,
// float4 becomes JPH::Vec4, float4x4 becomes JPH::Mat44, scalars become their
// stdint spelling, and Slang arrays become std::array. An annotation only
// overrides the default, and zshader fails the build -- naming the member --
// when one is used where it cannot apply, so a stale annotation is loud.
//
// The six, with the members that carry them:
//
//   [CxxArray]            float vector -> std::array<float, N>. The way back
//                         from the Vec4 default for plain float4 payload
//                         (colors, weights).
//   [CxxCArray]           float vector, or an array of float vectors or
//                         matrices, -> C array(s). For members the host
//                         memcpys, indexes, decays, or brace-initializes.
//   [CxxQuat]             float4 -> JPH::Quat. Only on float4.
//   [CxxEnum("...")]      int/uint scalar -> the named C++ enum. Slang-side
//                         stays a plain scalar (the GPU has no enums); the
//                         host gains the type.
//   [CxxDefault("...")]   default member initializer, emitted verbatim after
//                         `=`. Slang declares no defaults, so these are the
//                         host's alone.
//   [CxxSkip]             on a struct: no host struct is emitted. Only
//                         GPUMeshlet carries it: its ABI is fetchMeshlet's
//                         raw word protocol, not the declaration, so no
//                         declaration-derived spelling would be the layout
//                         the shaders read.
module cxx_abi;

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxArrayAttribute {
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxCArrayAttribute {
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxQuatAttribute {
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxEnumAttribute {
    string name;
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxDefaultAttribute {
    string init;
};

[__AttributeUsage(_AttributeTargets.Struct)]
public struct CxxSkipAttribute {
};
