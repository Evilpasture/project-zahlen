#!/usr/bin/env python3
"""Cross-check shader resource declaration order against the host WriteHeap
argument order, plus the sampler ordering consumed by InitHeapPassSamplers.

Both sides are positional: HeapManager::WriteBindings walks args against the
SPIR-V-reflected binding table by index, and InitHeapPassSamplers assigns
sampler create-infos to sampler slots in order of appearance. A single
misplaced declaration or argument therefore silently binds the wrong resource,
so this is worth checking mechanically.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

SHADER_TYPE_RE = re.compile(
    r"^\s*(?:(?:Texture2D(?:Array)?|TextureCube(?:Array)?|Texture3D)(?:<[^>]*>)?|SamplerState|SamplerComparisonState|"
    r"StructuredBuffer<[^>]*>|RWStructuredBuffer<[^>]*>|ConstantBuffer<[^>]*>|RaytracingAccelerationStructure|"
    r"RWTexture2D(?:<[^>]*>)?|RWTexture3D(?:<[^>]*>)?)\s+(\w+)\s*;"
)
SAMPLER_TYPES = {"SamplerState", "SamplerComparisonState"}


def shader_bindings(path: Path, disable_rtr: bool = False) -> list[tuple[str, str]]:
    """Ordered (type, name) list of set-0 resource declarations."""
    out = []
    skip = False
    for raw in path.read_text().split("\n"):
        line = raw.split("//")[0]
        if disable_rtr and "#ifndef DISABLE_RTR" in line:
            skip = True
        if skip and "#endif" in line:
            skip = False
            continue
        if skip:
            continue
        m = SHADER_TYPE_RE.match(line)
        if m:
            kind = line.strip().split("<")[0].split(" ")[0]
            out.append((kind, m.group(1)))
    return out


def writeheap_args(text: str, pass_name: str) -> list[str]:
    """Extract the argument expressions of one `pass.WriteHeap(` call."""
    key = f"self.{pass_name}.WriteHeap("
    i = text.index(key) + len(key)
    depth, j = 1, i
    while depth:
        if text[j] in "([{":
            depth += 1
        elif text[j] in ")]}":
            depth -= 1
        j += 1
    body = text[i : j - 1]
    # Split on top-level commas.
    args, depth, cur = [], 0, ""
    for ch in body:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    args = [re.sub(r"\s+", " ", a) for a in args if a.strip()]
    # WriteHeap(ctx, heapManager, heapIndex, <resources...>): the first three
    # parameters are not descriptor bindings.
    return args[3:]


def sampler_infos(text: str, pass_name: str) -> list[str]:
    """The infos array used for one pass in InitPassSamplerDescriptors."""
    m = re.search(
        r"std::array<VkSamplerCreateInfo,\s*\d+>\s*infos\s*=\s*\{([^}]*)\};[^}]*?"
        + re.escape(pass_name) + r"\.heapBindings,\s*infos\)",
        text,
        re.S,
    )
    if not m:
        return []
    return [x.strip() for x in m.group(1).split(",") if x.strip()]


def main() -> int:
    graph = (REPO / "src/engine/graphics/RenderGraphBuilder.cpp").read_text()
    heaps = (REPO / "src/engine/graphics/init/RenderInitHeaps.cpp").read_text()
    ok = True

    cases = [
        ("lighting.slang", "lightingPass", "lighting.slang"),
        ("reflection.slang", "reflectionPass", "reflection.slang"),
        ("reflection.slang", "translucentReflectionPass", "reflection.slang"),
    ]
    for shader_file, pass_name, label in cases:
        for variant, disable_rtr in (("", False), ("-DDISABLE_RTR", True)):
            if "reflection" in shader_file and disable_rtr:
                continue  # reflection variant shares the same table
            bindings = shader_bindings(REPO / "resources/shaders" / shader_file, disable_rtr)
            args = writeheap_args(graph, pass_name)
            samplers_shader = [n for k, n in bindings if k in SAMPLER_TYPES]
            infos = sampler_infos(heaps, pass_name)

            print(f"\n=== {pass_name} [{shader_file}{variant}] ===")
            print(f"  shader bindings : {len(bindings)}   WriteHeap args: {len(args)}")
            # The NoRT variants drop the trailing TLAS binding, so the host
            # passes one argument more than the table has entries. That is the
            # documented "hole stays at the tail" design: WriteBindings skips
            # args past the end of the reflected table, and keeping TLAS last
            # is what makes the skip land on nothing.
            expected_hole = disable_rtr and len(args) - len(bindings) == 1
            if len(bindings) != len(args) and not expected_hole:
                print(f"  !! COUNT MISMATCH (bindings {len(bindings)} vs args {len(args)})")
                ok = False
            elif expected_hole:
                print("  (NoRT tail hole: TLAS binding absent, trailing arg skipped)")
            print(f"  shader samplers : {samplers_shader}")
            print(f"  heap sampler infos: {infos}")
            if len(samplers_shader) != len(infos):
                print(f"  !! SAMPLER COUNT MISMATCH ({len(samplers_shader)} vs {len(infos)})")
                ok = False
            for idx, (kind, name) in enumerate(bindings):
                arg = args[idx] if idx < len(args) else "<none>"
                print(f"    [{idx:2}] {kind:<32} {name:<22} <- {arg[:74]}")

    print("\n" + ("ALL CONSISTENT" if ok else "INCONSISTENCIES FOUND"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
