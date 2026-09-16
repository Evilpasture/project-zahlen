#!/usr/bin/env python3
"""Cross-check descriptor writes against the shaders they feed, by name.

A host write names the binding it feeds -- `Vk::Slot<"texInput">(image)` -- and
HeapManager::WriteHeapParameters matches that name against the binding names
SPIRV-Reflect reported for the pass's mapping table. Two mistakes therefore stop
being visible only at runtime: a slot whose name matches no binding (its value is
never written) and a binding no slot names (it keeps whatever wrote it last, or
nothing). This checks both directions, plus the sampler ordering consumed by
InitHeapPassSamplers and a kind check (an image value named onto a buffer
binding), against the shader sources with their per-pass defines.

Samplers are excluded from the slot comparison on purpose: their descriptors live
in static sampler-heap slots. A binding a configuration drops (Slang removes
parameters nothing references -- `lighting.slang`'s blueNoiseTex and tlas are
absent from the NoRT module) is checked as "the slots may exceed the resource
list, but never fall short of it", which is exactly the runtime rule.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GRAPH = "src/render/RenderGraphBuilder.cpp"
HEAPS = "src/render/init/RenderInitHeaps.cpp"

SHADER_TYPE_RE = re.compile(
    r"^\s*(?P<type>(?:Texture2D(?:Array)?|TextureCube(?:Array)?|Texture3D)(?:<[^>]*>)?|SamplerState|SamplerComparisonState|"
    r"StructuredBuffer<[^>]*>|RWStructuredBuffer<[^>]*>|ConstantBuffer<[^>]*>|RaytracingAccelerationStructure|"
    r"RWTexture2D(?:Array)?(?:<[^>]*>)?|RWTexture3D(?:<[^>]*>)?)\s+(?P<name>\w+)\s*;"
)

# Shader declaration -> the kind of value a slot must carry for it.
KIND_OF_DECL = {
    "sampler": {"SamplerState", "SamplerComparisonState"},
    "accel": {"RaytracingAccelerationStructure"},
    "buffer": {"ConstantBuffer", "StructuredBuffer", "RWStructuredBuffer"},
}


def decl_kind(type_name: str) -> str:
    base = type_name.split("<")[0]
    for kind, types in KIND_OF_DECL.items():
        if base in types:
            return kind
    if base.startswith("Texture") or base.startswith("RWTexture"):
        return "image"
    return f"unknown:{base}"


def shader_decls(path: Path, defines: dict[str, bool]) -> list[tuple[str, str, bool]]:
    """Ordered (kind, name, live) of the set-0 declarations active under `defines`.

    `live` is what the module's table ends up holding: Slang drops a parameter
    nothing references, so a declaration that no *active* line mentions -- another
    pass's variant, or a parameter this configuration stopped using -- needs no
    slot and no sampler info. Naming one anyway is harmless: an unmatched name is
    skipped (see the header).

    Understands the two conditional forms the render shaders use: `#ifndef X`
    (DISABLE_RTR) and `#if defined(X)` (the SMAA pass variants).
    """
    declared, referenced, stack, active = [], set(), [], True
    for raw in path.read_text().split("\n"):
        line = raw.split("//")[0]
        stripped = line.strip()
        if stripped.startswith("#if"):
            if stripped.startswith("#ifndef"):
                name = stripped.split()[1]
                cond = not defines.get(name, False)
            elif stripped.startswith("#if defined"):
                name = re.search(r"defined\(\s*(\w+)\s*\)", stripped).group(1)
                cond = defines.get(name, False)
            else:
                cond = True  # A form this checker does not model: keep looking.
            stack.append(active)
            active = active and cond
            continue
        if stripped.startswith("#endif"):
            if stack:
                active = stack.pop()
            continue
        if not active:
            continue
        m = SHADER_TYPE_RE.match(line)
        if m:
            declared.append((decl_kind(m.group("type")), m.group("name")))
        else:
            referenced.update(re.findall(r"\b[A-Za-z_]\w*\b", line))
    return [(kind, name, name in referenced) for kind, name in declared]


def slot_uses(text: str, anchor: str) -> list[tuple[str, str]]:
    """(name, value expression) of every `Vk::Slot<"name">(value)` after `anchor`."""
    start = text.index(anchor)
    i, depth = start, 0
    while i < len(text):  # to the end of the statement
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth < 0:
                break
        elif ch == ";" and depth == 0:
            break
        i += 1
    tail = text[start:i]

    out = []
    for m in re.finditer(r'Vk::Slot<"(\w+)">\(', tail):
        j, depth = m.end() - 1, 0
        while j < len(tail):
            if tail[j] in "([{":
                depth += 1
            elif tail[j] in ")]}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((m.group(1), tail[m.end():j].strip()))
    return out


def value_kind(expr: str) -> str | None:
    """The kind of descriptor a slot's value supplies, or None when unclear."""
    if "SkipWrite" in expr:
        return "skip"
    if "AsAddressWrite" in expr or expr.strip() == "tlas":
        return "accel"
    if "Assume<" in expr or "AssumeLayout<" in expr or "TypedImage<" in expr or "ImageWrite" in expr:
        return "image"
    if "Buffer" in expr or "indirect" in expr:
        return "buffer"
    return None


def sampler_slots(text: str, label: str) -> list[str]:
    """The sampler names InitPassSamplerDescriptors names for one pass.

    Reads the `Vk::SamplerSlot<"name">(...)` arguments of the call that
    initializes `label`'s sampler slots; a pass with no sampler binding has no
    call at all (see the hiz_generate.slang note in RenderInitHeaps.cpp).
    """
    m = re.search(
        r"InitHeapPassSamplers\(\s*heapManager\s*,\s*"
        + r"(?:" + re.escape(label) + r"\.heapBindings|" + re.escape(label) + r")"
        + r"\s*,\s*(.*?)\);",
        text,
        re.S,
    )
    return re.findall(r'SamplerSlot<\s*"([^"]+)"\s*>', m.group(1)) if m else []


class Case:
    def __init__(self, label, shader, sites, sampler_label=None, defines=None, variants=("default",)):
        self.label = label
        self.shader = shader
        self.sites = sites
        self.sampler_label = sampler_label or label
        self.defines = defines or {}
        self.variants = variants


def cases() -> list[Case]:
    g = lambda anchor: [(GRAPH, anchor)]
    return [
        Case("lightingPass", "lighting.slang", g("self.lightingPass.WriteHeapParameters("), variants=("RT", "NoRT")),
        Case("reflectionPass", "reflection.slang", g("self.reflectionPass.WriteHeapParameters("), variants=("RT", "NoRT")),
        Case("translucentReflectionPass", "reflection.slang", g("self.translucentReflectionPass.WriteHeapParameters("), variants=("RT", "NoRT")),
        Case("rtrHalfHeapBindings", "rtr_half.slang", g("self.rtrHalfHeapBindings,")),
        Case("gtaoHeapBindings", "ao_gtao.slang", g("self.gtaoHeapBindings,")),
        Case("taaPass", "taa.slang", g("self.taaPass.WriteHeapParameters(")),
        Case("blitPass", "blit.slang", g("self.blitPass.WriteHeapParameters(") + [("src/render/RenderFrame.cpp", "blitPass.WriteHeapParameters(")]),
        Case("fxaaPass", "fxaa.slang", g("self.fxaaPass.WriteHeapParameters(")),
        Case("mlaaPass", "mlaa.slang", g("self.mlaaPass.WriteHeapParameters(")),
        Case("smaaEdgePass", "SMAA.slang", g("self.smaaEdgePass.WriteHeapParameters("), defines={"EDGE_PASS": True}),
        Case("smaaWeightPass", "SMAA.slang", g("self.smaaWeightPass.WriteHeapParameters("), defines={"WEIGHT_PASS": True}),
        Case("smaaBlendPass", "SMAA.slang", g("self.smaaBlendPass.WriteHeapParameters("), defines={"BLEND_PASS": True}),
        Case("volumetricFogInjectPass", "volumetric_fog_inject.slang", g("self.volumetricFogInjectPass.WriteHeapParameters(")),
        Case("volumetricLightInjectPass", "volumetric_light_inject.slang", g("self.volumetricLightInjectPass.WriteHeapParameters(")),
        Case("volumetricIntegrationPass", "volumetric_integration.slang", g("self.volumetricIntegrationPass.WriteHeapParameters(")),
        Case("volumetricTemporalPass", "volumetric_temporal.slang", g("self.volumetricTemporalPass.WriteHeapParameters(")),
        Case("bloomThresholdHeapBindings", "bloom_threshold_cs.slang", g("self.bloomThresholdCS, self.bloomThresholdHeapBindings")),
        Case("bloomDownHeapBindings", "bloom_down_cs.slang", g("self.bloomDownCS, self.bloomDownHeapBindings")),
        Case("bloomUpHeapBindings", "bloom_up_cs.slang", g("self.bloomUpCS, self.bloomUpHeapBindings")),
        Case("hdrDenoiseHeapBindings", "hdr_denoise_atrous.slang", g("self.hdrDenoiseCS, self.hdrDenoiseHeapBindings")),
        Case("hizHeapBindings", "hiz_generate.slang", [("src/render/init/RenderInitTargets.cpp", "hizHeapBindings,")]),
        Case("cullingHeapBindings", "culling.slang", [("src/render/init/RenderInitTargets.cpp", "cullingHeapBindings,")]),
        Case("clusterCullingHeapBindings", "cluster_culling.slang", [("src/render/init/RenderInitScenePipelines.cpp", "clusterCullingHeapBindings,")]),
        Case("clusterBoundsHeapBindings", "cluster_bounds.slang", [("src/render/init/RenderInitScenePipelines.cpp", "clusterBoundsHeapBindings,")]),
        # One shared bake table, built from procedural_bake.slang: every bake
        # shader that dispatches through it has to name its output to match.
        Case(
            "bakeHeapBindings",
            "procedural_bake.slang",
            [
                ("src/render/RenderProcedural.cpp", "bakeHeapBindings, kBake2DHeapIndex"),
                ("src/render/RenderInternal.hpp", "bakeHeapBindings, kBake2DHeapIndex"),
                ("src/render/IBLProcessor.hpp", "impl.bakeHeapBindings, RenderContext::Impl::kBake2DHeapIndex"),
                ("src/render/IBLProcessor.hpp", "impl.bakeHeapBindings, RenderContext::Impl::kBakeSpecHeapIndex0 + mip"),
            ],
        ),
    ]


BAKE_TABLE_SHADERS = ["procedural_bake.slang", "smaa_lut.slang", "brdf_lut.slang", "ibl_bake.slang"]


def main() -> int:
    ok = True
    texts = {GRAPH: (REPO / GRAPH).read_text()}
    heaps_text = (REPO / HEAPS).read_text()

    for case in cases():
        for text_path, anchor in case.sites:
            if text_path not in texts:
                texts[text_path] = (REPO / text_path).read_text()
            assert anchor in texts[text_path], f"{case.label}: anchor gone from {text_path}: {anchor!r}"

        per_site = [slot_uses(texts[text_path], anchor) for text_path, anchor in case.sites]
        uses = [u for site in per_site for u in site]
        slots = [name for name, _ in uses]
        for site in per_site:
            names_in_site = [name for name, _ in site]
            if len(set(names_in_site)) != len(names_in_site):
                print(f"\n!! {case.label}: one write names the same binding twice: {names_in_site}")
                ok = False

        all_decls = shader_decls(REPO / "resources/shaders" / case.shader, case.defines)
        all_names = {name for kind, name, _ in all_decls if kind != "sampler"}
        # A name no configuration declares is a typo; a name a *different*
        # configuration declares is an argument that configuration's pass does
        # not need (skipped, see the dead-strip note).
        declared_samplers = {name for kind, name, _ in all_decls if kind == "sampler"}
        named_samplers = sampler_slots(heaps_text, case.sampler_label)

        for variant in case.variants:
            defines = dict(case.defines)
            if variant == "NoRT":
                defines["DISABLE_RTR"] = True
            decls = shader_decls(REPO / "resources/shaders" / case.shader, defines)
            resources = [(kind, name) for kind, name, live in decls if kind != "sampler" and live]
            dead = [name for kind, name, live in decls if kind != "sampler" and not live]
            samplers = [name for kind, name, live in decls if kind == "sampler" and live]
            names = [name for _, name in resources]

            print(f"\n=== {case.label} [{case.shader} {variant}] ===")
            unknown_slots = [n for n in slots if n not in all_names]
            missing = [n for n in names if n not in slots]
            if unknown_slots:
                print(f"  !! names no binding has: {unknown_slots}  (the value is written nowhere)")
                ok = False
            if missing:
                print(f"  !! resource bindings no slot names: {missing}  (stale descriptor)")
                ok = False
            if dead:
                print(f"  note: declared here but referenced nowhere in this configuration: {dead}")
            unknown_samplers = [n for n in named_samplers if n not in declared_samplers]
            missing_samplers = [n for n in samplers if n not in named_samplers]
            if unknown_samplers:
                print(f"  !! sampler names no binding has: {unknown_samplers}  (initialized nowhere)")
                ok = False
            if missing_samplers:
                print(f"  !! live samplers never initialized: {missing_samplers}  (sampled through an unwritten slot)")
                ok = False
            print(f"  bindings {len(names)} | samplers {samplers} | named {named_samplers}")
            for name, expr in uses:
                if name not in names:
                    continue  # A binding this configuration drops: see the header.
                kind = {n: k for k, n in resources}[name]
                got = value_kind(expr)
                flat = re.sub(r"\s+", " ", expr)
                mark = ""
                if got not in (None, "skip", kind):
                    mark = f"   !! {kind} binding, {got} value"
                    ok = False
                print(f"    {name:<22} {kind:<6} <- {flat[:66]}{mark}")

    # The shared bake table's shaders must agree on the name, or a slot that
    # matches the table's name writes a binding the running shader calls
    # something else.
    print("\n=== bake table: shared binding name ===")
    for shader in BAKE_TABLE_SHADERS:
        decls = [name for kind, name, live in shader_decls(REPO / "resources/shaders" / shader, {}) if kind != "sampler" and live]
        if decls != ["outTexture"]:
            print(f"  !! {shader} declares {decls}, expected ['outTexture'] (one shared table, one shared name)")
            ok = False
        else:
            print(f"  {shader:<24} outTexture")

    # Informational only: a declaration nothing references is dead weight in the
    # shader, but with name matching it can no longer shift another binding.
    for shader in sorted({c.shader for c in cases()}):
        path = REPO / "resources/shaders" / shader
        stripped = "\n".join(line.split("//")[0] for line in path.read_text().split("\n"))
        unused = [n for _, n, live in shader_decls(path, {}) if not live]
        if unused:
            print(f"\nnote: {shader} declares {unused} without referencing them (Slang drops them; matching by name tolerates it)")

    print("\n" + ("ALL CONSISTENT" if ok else "INCONSISTENCIES FOUND"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
