#!/usr/bin/env python3
"""Cross-check shader resource declaration order against the host parameter
blocks, plus the sampler ordering consumed by InitHeapPassSamplers.

Both sides are named and ordered: HeapManager::WriteHeapParameters walks a
block's fields against the SPIR-V-reflected binding table by index -- the k-th
field feeds the k-th non-sampler binding -- and InitHeapPassSamplers assigns
sampler create-infos to sampler slots in order of appearance. A misplaced
declaration or a block field out of order therefore silently binds the wrong
resource, so this is worth checking mechanically. Fields carry the shader's own
binding names, which is what makes a name-by-name check possible.
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


def block_fields(text: str, anchor: str) -> tuple[str, list[tuple[str, str]]]:
    """The (block type, [(field, initializer), ...]) written at `anchor`.

    `anchor` locates a WriteHeapParameters call -- either the pass wrapper or the
    binding table of a direct heap write; the block is the PassParams aggregate
    that follows it.
    """
    start = text.index(anchor)
    m = re.search(r"PassParams::(\w+)\s*\{", text[start:])
    if m is None:
        raise ValueError(f"no PassParams block after {anchor!r}")
    block = m.group(1)
    i = start + m.end()  # first character after the opening brace
    depth, fields, token = 1, [], ""
    while depth:
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                break
        if depth == 1 and ch == ",":
            fields.append(token)
            token = ""
        else:
            token += ch
        i += 1
    if token.strip():
        fields.append(token)

    out = []
    for field in fields:
        m = re.match(r"\s*\.(\w+)\s*=\s*(.*)", field, re.S)
        if m:
            out.append((m.group(1), re.sub(r"\s+", " ", m.group(2)).strip()))
    return block, out


def declared_fields(text: str) -> dict[str, list[str]]:
    """Field lists of every PassParams block declaration in PassParameters.hpp."""
    blocks = {}
    for m in re.finditer(r"struct (\w+)\s*\{(.*?)\n\};", text, re.S):
        fields = re.findall(r"^\s+(?:PassParams::\w+|Vk::[A-Za-z_:<>]+|[\w:]+)\s*&?\s*(\w+)\s*;", m.group(2), re.M)
        blocks[m.group(1)] = [f for f in fields if f not in ("operator", "using")]
    return blocks


def site_mismatches(source: str, blocks: dict[str, list[str]]) -> list[tuple[int, str, list[str], list[str]]]:
    """Every `PassParams::<Block> { ... }` site whose fields differ from the block.

    A designated initializer may legally omit a field, so a site that spells
    fewer fields than the block declares still compiles -- the missing value
    silently leaves that binding's descriptor to whoever wrote it last. Compare
    the site's field list to the declaration, in order.
    """
    out = []
    for m in re.finditer(r"PassParams::(\w+)\s*\{", source):
        block = m.group(1)
        i, depth, token, fields = m.end(), 1, "", []
        while depth:
            ch = source[i]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
                if depth == 0:
                    break
            if depth == 1 and ch == ",":
                f = re.match(r"\s*\.(\w+)\s*=", token)
                if f:
                    fields.append(f.group(1))
                token = ""
            else:
                token += ch
            i += 1
        f = re.match(r"\s*\.(\w+)\s*=", token)
        if f:
            fields.append(f.group(1))
        want = blocks.get(block)
        if want is None:
            out.append((source[:m.start()].count("\n") + 1, block, fields, ["<no such block>"]))
        elif fields != want:
            out.append((source[:m.start()].count("\n") + 1, block, fields, want))
    return out


def sampler_infos(text: str, pass_name: str) -> list[str]:
    """The infos array used for one pass in InitPassSamplerDescriptors."""
    m = re.search(
        r"std::array<VkSamplerCreateInfo,\s*\d+>\s*infos\s*=\s*\{([^}]*)\};[^}]*?"
        + r"(?:" + re.escape(pass_name) + r"\.heapBindings|" + re.escape(pass_name) + r")"
        + r",\s*infos\)",
        text,
        re.S,
    )
    if not m:
        return []
    return [x.strip() for x in m.group(1).split(",") if x.strip()]


def unused_declarations(path: Path) -> list[str]:
    """Declared set-0 resources whose name appears nowhere else in the file.

    Slang dead-strips unreferenced shader parameters, so the reflected table
    silently loses the entry and every block field after it shifts down one
    binding. Any unused declaration is therefore a hard error.
    """
    stripped = "\n".join(line.split("//")[0] for line in path.read_text().split("\n"))
    return [
        name
        for _kind, name in shader_bindings(path)
        if len(re.findall(rf"\b{re.escape(name)}\b", stripped)) < 2
    ]


def main() -> int:
    graph = (REPO / "src/render/RenderGraphBuilder.cpp").read_text()
    heaps = (REPO / "src/render/init/RenderInitHeaps.cpp").read_text()
    ok = True

    # (shader file, write anchor in RenderGraphBuilder.cpp, label)
    cases = [
        ("lighting.slang", "self.lightingPass.WriteHeapParameters(", "lightingPass"),
        ("reflection.slang", "self.reflectionPass.WriteHeapParameters(", "reflectionPass"),
        ("reflection.slang", "self.translucentReflectionPass.WriteHeapParameters(", "translucentReflectionPass"),
        ("rtr_half.slang", "self.rtrHalfHeapBindings,", "rtrHalfHeapBindings"),
        ("ao_gtao.slang", "self.gtaoHeapBindings,", "gtaoHeapBindings"),
    ]

    seen_shaders = set()
    for shader_file, _anchor, _label in cases:
        if shader_file in seen_shaders:
            continue
        seen_shaders.add(shader_file)
        unused = unused_declarations(REPO / "resources/shaders" / shader_file)
        if unused:
            print(f"\n!! {shader_file}: declared-but-unused resources {unused} "
                  "(Slang strips these, shifting the positional heap table)")
            ok = False

    for shader_file, anchor, label in cases:
        for variant, disable_rtr in (("", False), ("-DDISABLE_RTR", True)):
            if shader_file in ("reflection.slang", "rtr_half.slang", "ao_gtao.slang") and disable_rtr:
                continue  # reflection variant shares the same table; rtr_half/ao_gtao have one variant
            bindings = shader_bindings(REPO / "resources/shaders" / shader_file, disable_rtr)
            resources = [(kind, name) for kind, name in bindings if kind not in SAMPLER_TYPES]
            samplers_shader = [name for kind, name in bindings if kind in SAMPLER_TYPES]
            block, fields = block_fields(graph, anchor)
            infos = sampler_infos(heaps, label)

            print(f"\n=== {label} [{shader_file}{variant}] ===")
            print(f"  shader bindings : {len(bindings)}   {block} fields: {len(fields)}")
            # The NoRT variants drop the trailing TLAS declaration, so the host
            # block has one field more than the table has entries. That is the
            # documented "hole stays at the tail" design: WriteHeapParameters
            # drops fields past the end of the reflected table, and keeping TLAS
            # last is what makes the drop land on nothing.
            expected_hole = disable_rtr and len(fields) - len(resources) == 1 and fields[-1][0] == "tlas"
            if len(fields) != len(resources) and not expected_hole:
                print(f"  !! COUNT MISMATCH (resource bindings {len(resources)} vs block fields {len(fields)})")
                ok = False
            elif expected_hole:
                print("  (NoRT tail hole: TLAS binding absent, trailing field dropped)")
            print(f"  shader samplers : {samplers_shader}")
            print(f"  heap sampler infos: {infos}")
            if len(samplers_shader) != len(infos):
                print(f"  !! SAMPLER COUNT MISMATCH ({len(samplers_shader)} vs {len(infos)})")
                ok = False
            for idx, (kind, name) in enumerate(resources):
                if idx >= len(fields):
                    print(f"    [{idx:2}] {kind:<32} {name:<22} <- <missing field>")
                    ok = False
                    continue
                field, value = fields[idx]
                mark = "" if field == name else f"  !! field named {field!r}"
                print(f"    [{idx:2}] {kind:<32} {name:<22} <- {value[:74]}{mark}")
                if field != name:
                    ok = False

    # Every write site must spell exactly its block's fields, in order: an
    # omitted field still compiles but leaves that binding to whoever wrote it
    # last, which is the silent failure the block model exists to prevent.
    header = (REPO / "src/render/PassParameters.hpp").read_text()
    blocks = declared_fields(header)
    for path in sorted(REPO.glob("src/**/*.*pp")):
        for line, block, got, want in site_mismatches(path.read_text(), blocks):
            print(f"\n!! {path.relative_to(REPO)}:{line} {block}\n   site : {got}\n   decl : {want}")
            ok = False

    print("\n" + ("ALL CONSISTENT" if ok else "INCONSISTENCIES FOUND"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
