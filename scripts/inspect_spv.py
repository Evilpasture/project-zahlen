#!/usr/bin/env python3
"""Minimal SPIR-V static inspector.

Extracts entry points, capabilities, set/binding decorations (resolved to
variable names) and builtin decorations (e.g. ViewIndex) from a SPIR-V module,
so the descriptor-heap binding order the C++ reflection depends on can be
checked without spirv-tools.
"""

import struct
import sys

# opcodes
OP_CAPABILITY = 17
OP_EXTENSION = 12
OP_ENTRY_POINT = 15
OP_NAME = 5
OP_DECORATE = 71

EXEC_MODELS = {0: "Vertex", 4: "Fragment", 5: "GLCompute", 5267: "TaskEXT", 5268: "MeshEXT"}

CAPABILITIES = {
    1: "Shader", 13: "Sampled1D", 23: "ImageQuery", 44: "MultiView", 53: "Int64",
    50: "StorageImageExtendedFormats", 55: "Int8", 56: "Int16",
    4427: "FragmentShadingRateKHR", 4439: "FragmentShadingRateKHR_",
    5283: "MeshShadingNV", 5347: "GroupNonUniformBallot", 5378: "MeshShadingEXT",
    5370: "MeshShadingNV", 5374: "FragmentBarycentricEXT", 5364: "TaskShadingEXT", 5365: "MeshShadingEXT_",
}

BUILTIN_NAMES = {
    0: "Position", 1: "PointSize", 3: "InvocationId", 7: "PrimitiveId", 8: "InvocationId_",
    9: "Layer", 10: "ViewportIndex", 15: "VertexIndex", 16: "InstanceIndex",
    42: "BaseVertex", 43: "BaseInstance", 44: "DrawIndex",
    4432: "PrimitiveShadingRateKHR", 4440: "ViewIndex",
    5264: "ViewportMaskNV",
}
BUILTIN_ViewIndex = 4440
BUILTINS_SAFE = dict(BUILTIN_NAMES)


def read_string(words, start):
    out = bytearray()
    for w in words[start:]:
        out += struct.pack("<I", w)
        if 0 in struct.pack("<I", w):
            break
    return out.split(b"\0")[0].decode(errors="replace")


def inspect(path: str) -> int:
    data = open(path, "rb").read()
    if data[:4] != b"\x03\x02\x23\x07":
        print(f"{path}: not SPIR-V")
        return 1
    words = struct.unpack(f"<{len(data) // 4}I", data)
    pos, end = 5, words[3]
    names, entry_points, caps, bindings, builtins = {}, [], [], {}, {}
    i = pos
    while i < len(words) and (words[i] >> 16) != 0:
        wc = words[i] >> 16
        op = words[i] & 0xFFFF
        body = words[i + 1:i + wc]
        if op == OP_CAPABILITY:
            caps.append(CAPABILITIES.get(body[0], str(body[0])))
        elif op == OP_NAME:
            names[body[0]] = read_string(body, 1)
        elif op == OP_ENTRY_POINT:
            model = EXEC_MODELS.get(body[0], str(body[0]))
            entry_points.append((model, read_string(body, 2), list(body[4:])))
        elif op == OP_DECORATE:
            target, deco = body[0], body[1]
            if deco == 34:  # DescriptorSet
                bindings.setdefault(target, {})["set"] = body[2]
            elif deco == 33:  # Binding
                bindings.setdefault(target, {})["binding"] = body[2]
            elif deco == 11:  # BuiltIn
                builtins.setdefault(target, []).append(BUILTINS_SAFE.get(body[2], str(body[2])))
        i += wc
    print(f"== {path}")
    print("capabilities:", ", ".join(sorted(set(caps))))
    for model, name, _iface in entry_points:
        print(f"entry: {model} {name}")
    for vid in sorted(bindings):
        b = bindings[vid]
        if "set" in b and "binding" in b:
            print(f"  set={b['set']} binding={b['binding']:2d}  {names.get(vid, f'id{vid}')}")
    view_index = {vid for vid, bl in builtins.items() if "ViewIndex" in bl}
    if view_index:
        for vid in sorted(view_index):
            print(f"  ViewIndex builtin on: {names.get(vid, f'id{vid}')}")
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(sum(inspect(p) for p in sys.argv[1:]) != 0 if len(sys.argv) > 1 else (print(__doc__), 1)[1])
