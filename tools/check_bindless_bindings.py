#!/usr/bin/env python3
"""Cross-check descriptor writes against the shaders they feed, by name.

A host write names the binding it feeds -- `Vk::Slot<"texInput">(image)` -- and
HeapManager::WriteHeapParameters matches that name against the binding names
SPIRV-Reflect reported for the pass's mapping table. Two mistakes therefore stop
being visible only at runtime: a slot whose name matches no binding (its value is
never written) and a binding no slot names (it keeps whatever wrote it last, or
nothing). This checks both directions, plus the sampler names consumed by
InitHeapPassSamplers and a kind check (an image value named onto a buffer
binding), against the shader sources with their per-pass defines.

The write also *allocates* its block: the call returns the base slot its dispatch
pushes into the mapping's index word, so a dropped result -- or a dispatch still
taking a frame index / mip / parity where the base belongs, or any leftover of the
old per-pass variant reservations -- is a failure here rather than a picture of
another pass's descriptors on screen.

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


def statement_at(text: str, start: int) -> str:
    """The statement holding `start`: from the previous ; { or } to its ;."""
    begin = max(text.rfind(";", 0, start), text.rfind("{", 0, start), text.rfind("}", 0, start)) + 1
    i, depth = start, 0
    while i < len(text):
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
    return text[begin:i]


def slots_in(statement: str) -> list[tuple[str, str]]:
    """(name, value expression) of every `Vk::Slot<"name">(value)` in `statement`."""
    out = []
    for m in re.finditer(r'Vk::Slot<"(\w+)">\(', statement):
        j, depth = m.end() - 1, 0
        while j < len(statement):
            if statement[j] in "([{":
                depth += 1
            elif statement[j] in ")]}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((m.group(1), statement[m.end():j].strip()))
    return out


def write_sites(text: str, token: str) -> list[tuple[str, list[tuple[str, str]]]]:
    """Statements that name bindings through `token`, with the slots they carry.

    A statement counts when it carries `Vk::Slot<...>` values and either calls
    WriteHeapParameters itself (the block base is then in the statement) or hands
    them to ComputeChain::Step, which writes through the chain helper and
    allocates the block on the caller's behalf.
    """
    out = []
    for found in re.finditer(re.escape(token), text):
        statement = statement_at(text, found.start())
        if "WriteHeapParameters(" not in statement and "Step(" not in statement:
            continue
        slots = slots_in(statement)
        if slots:
            out.append((statement, slots))
    return out


def value_kind(expr: str) -> str | None:
    """The kind of descriptor a slot's value supplies, or None when unclear."""
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


def split_params(text: str, open_paren: int) -> list[str]:
    """Top-level items of the parenthesised list whose '(' sits at `open_paren`.

    Nesting in (), [] and {} is skipped, so a lambda body or an initializer in an
    argument does not split the list, and so is a template argument list -- a
    `SceneResources<A, B> in` parameter is one parameter, not two. `<` only opens
    one where it follows something it can be a template argument list of; a
    comparison operator in an argument would still read as arithmetic < (the
    worst case is a spurious arity report, never a missed split).
    """
    depth = 0
    angle = 0
    start = open_paren + 1
    prev = ""
    parts: list[str] = []
    for j in range(open_paren, len(text)):
        ch = text[j]
        if ch == "<" and (prev.isalnum() or prev in "_:>"):
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        elif angle:
            pass
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                tail = text[start:j].strip()
                if tail:
                    parts.append(tail)
                return parts
        elif ch == "," and depth == 1:
            parts.append(text[start:j].strip())
            start = j + 1
        if not ch.isspace():
            prev = ch
    return parts


def param_type(param: str) -> str:
    """A parameter without its default and without its name: what a caller matches."""
    param = re.sub(r"\s+", " ", param.split("=")[0].strip())
    return re.sub(r"\s+[A-Za-z_]\w*$", "", param)


def struct_execute_params(text: str, struct: str) -> list[str] | None:
    """`Execute`'s parameter list as declared inside `struct <struct> { ... }`."""
    m = re.search(r"struct " + struct + r"\s*\{", text)
    if m is None:
        return None
    depth = 0
    for j in range(m.end() - 1, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                body = text[m.end():j]
                break
    else:
        return None
    call = re.search(r"\bExecute\s*\(", body)
    return split_params(body, call.end() - 1) if call else None


def definition_params(text: str, struct: str) -> list[str] | None:
    """`Execute`'s parameter list as spelled in `Struct::Execute(...) {`."""
    m = re.search(re.escape(struct) + r"::Execute\s*\(", text)
    return split_params(text, m.end() - 1) if m else None


def signature_parity() -> bool:
    """The split pass structs' declarations, definitions and call sites agree.

    A pass whose `Execute` is declared in one header and defined in a .cpp is
    exactly the place where a new argument (the descriptor block, say) can reach
    two of the three spellings and miss the third: a definition that matches no
    declaration does not compile, but a call site one argument short passes a
    frame index where a block base belongs and compiles fine. Both are checked.
    """
    ok = True
    print("\n=== call/declaration parity ===")
    for header, definition, struct in (
        ("src/render/RenderInternal.hpp", "src/render/RenderPasses.cpp", "BlitPass"),
        ("src/render/RenderInternal.hpp", "src/render/RenderPasses.cpp", "ViewmodelPass"),
    ):
        decl = struct_execute_params((REPO / header).read_text(), struct)
        definition_params_ = definition_params((REPO / definition).read_text(), struct)
        if decl is None or definition_params_ is None:
            print(f"  !! {struct}: Execute is not declared/defined where this checker looks for it")
            ok = False
            continue
        if [param_type(p) for p in decl] != [param_type(p) for p in definition_params_]:
            print(f"  !! {struct}::Execute: {definition}'s definition matches no declaration")
            print(f"       def : {definition_params_}")
            print(f"       decl: {decl}")
            ok = False
            continue
        min_arity = sum(1 for p in decl if "=" not in p)
        max_arity = len(decl)
        call_re = re.compile(r"\b" + struct + r"\s*\{\s*\}\s*\.\s*Execute\s*\(")
        calls = 0
        for path in sorted(REPO.glob("src/**/*.*")):
            if path.suffix not in (".cpp", ".hpp", ".inl"):
                continue
            text = path.read_text()
            for m in call_re.finditer(text):
                calls += 1
                count = len(split_params(text, m.end() - 1))
                if not min_arity <= count <= max_arity:
                    line = text[:m.start()].count("\n") + 1
                    print(
                        f"  !! {path.relative_to(REPO)}:{line}: {struct}::Execute called with {count} argument(s); "
                        f"the declaration takes {min_arity}..{max_arity}"
                    )
                    ok = False
        print(f"  {struct:<16} declaration == definition, {calls} call site(s) in range")
    return ok


def initializer_value(text: str, after_equals: int) -> str:
    """The value initializing a designated field: up to the top-level ',' or '}'."""
    depth = 0
    for j in range(after_equals, len(text)):
        ch = text[j]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            if depth == 0:
                return text[after_equals:j].strip()
            depth -= 1
        elif ch == "," and depth == 0:
            return text[after_equals:j].strip()
    return text[after_equals:after_equals + 80].strip()


def is_designated(text: str, at: int) -> bool:
    """`.field = ...` right after a '{' or ',' is an initializer, not a member write.

    `rt.viewInfo = MakeViewCreateInfo2D(...)` and `TypedImage {... .viewInfo = p}`
    are the same text with different meanings, and only the second is this
    checker's business.
    """
    j = at - 1
    while j >= 0 and text[j] in " \t\r\n":
        j -= 1
    return j >= 0 and text[j] in "{,"


def image_write_field_shapes() -> bool:
    """`.view` and `.viewInfo` name a raw VkImageView and a create-info pointer.

    The engine's handle types do not convert implicitly. A `RenderTarget`'s view
    is an ImageView wrapper (its VkImageView is `.Get()`) and its viewInfo is a
    create info (its pointer is `&`); a `TypedImage`'s viewInfo is already a
    pointer. `viewInfo` therefore has a decidable shape -- an initializer that is
    neither `&value`, nor `nullptr`, nor a `.viewInfo` member is wrong -- and it
    bit the HiZ mip-0 write, which spelled both fields off the depth target.

    A `TypedImage` member is raw, so `<recv>.viewInfo` is decidable only with an
    idea of what `recv` is: a TypedImage carries its handle beside the view
    (`.handle`, `.aspect`, `.format`), and a wrapper does not. Evidence of those
    members for the same receiver means the value is already a pointer. `view` is
    reported as a note rather than a failure for the same reason -- the shape is
    identical for both -- so a line to read beats a wrong failure. Both halves of
    the HiZ mip-0 compile error are found this way.
    """
    sources = [
        path.read_text()
        for path in sorted(REPO.glob("src/**/*.*"))
        if path.suffix in (".cpp", ".hpp", ".inl")
    ]
    text_all = "\n".join(sources)

    def raw_member_evidence(receiver: str) -> bool:
        """True when `receiver` is used with a TypedImage-only member elsewhere."""
        return any((receiver + "." + member) in text_all for member in ("handle", "aspect", "format"))

    ok = True
    notes = 0
    checked = 0
    print("\n=== image write field shapes ===")
    for path in sorted(REPO.glob("src/**/*.*")):
        if path.suffix not in (".cpp", ".hpp", ".inl"):
            continue
        rel = str(path.relative_to(REPO))
        text = path.read_text()
        for m in re.finditer(r"\.view(Info)?\s*=\s*", text):
            if not is_designated(text, m.start()):
                continue
            is_view = m.group(1) is None  # the regex matched '.view', not '.viewInfo'
            value = initializer_value(text, m.end())
            checked += 1
            line = text[:m.start()].count("\n") + 1
            if is_view:
                if value.endswith(".view") and not raw_member_evidence(value[: -len(".view")].strip()):
                    print(f"  note: {rel}:{line}: .view = {value} -- raw only if that is a TypedImage member")
                    notes += 1
                continue
            if value.startswith("&") or value.startswith("nullptr"):
                continue
            if value.endswith(".viewInfo"):
                if not raw_member_evidence(value[: -len(".viewInfo")].strip()):
                    print(f"  !! {rel}:{line}: .viewInfo = {value} -- that receiver has no raw handle; a create info needs its address")
                    ok = False
                continue
            print(f"  !! {rel}:{line}: .viewInfo = {value} -- a create info needs its address")
            ok = False
    print(f"  {checked} field initializer(s) checked, {notes} to read")
    return ok


def local_lambda_arity() -> bool:
    """A helper lambda's call sites have to agree with its parameter list.

    `const auto buildCompute = [&](pass, layout, bindings, spirv)` is an argument
    list nothing else in the tree sees, so a call site that still passes an
    argument the lambda dropped (the per-dispatch variant count step 3 removed)
    is only caught by compiling. A name with more than one lambda in the file, or
    one that also appears outside a lambda, is skipped: its call sites are not
    known to belong to the lambda.
    """
    ok = True
    checked = 0
    print("\n=== helper lambda arity ===")
    for path in sorted(REPO.glob("src/**/*.*")):
        if path.suffix not in (".cpp", ".hpp", ".inl"):
            continue
        rel = str(path.relative_to(REPO))
        text = path.read_text()
        lambdas = list(re.finditer(r"(?:const\s+)?auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(", text))
        for m in lambdas:
            name = m.group(1)
            if sum(1 for other in lambdas if other.group(1) == name) > 1:
                continue
            if re.search(r"\b" + name + r"\b", text[: m.start()]):
                continue
            params = split_params(text, m.end() - 1)
            min_arity = sum(1 for p in params if "=" not in p)
            max_arity = len(params)
            call_re = re.compile(r"(?<![\w:.>])" + name + r"\s*\(")
            for c in call_re.finditer(text):
                if m.start() <= c.start() <= m.end():
                    continue
                count = len(split_params(text, c.end() - 1))
                checked += 1
                if not min_arity <= count <= max_arity:
                    line = text[:c.start()].count("\n") + 1
                    print(
                        f"  !! {rel}:{line}: {name}(...) takes {count} argument(s); the lambda at line "
                        f"{text[:m.start()].count(chr(10)) + 1} takes {min_arity}..{max_arity}"
                    )
                    ok = False
    print(f"  {checked} call site(s) checked")
    return ok


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
        # HiZ and both culling passes write a block per dispatch while the frame
        # is recorded, so the sites are in the recording paths, not at init.
        Case("hizHeapBindings", "hiz_generate.slang", g("self.hizHeapBindings")),
        Case("cullingHeapBindings", "culling.slang", [("src/render/RenderPasses.cpp", "ctx.cullingHeapBindings")]),
        Case("clusterCullingHeapBindings", "cluster_culling.slang", g("self.clusterCullingHeapBindings")),
        Case("clusterBoundsHeapBindings", "cluster_bounds.slang", g("clusterBoundsHeapBindings")),
        # One shared bake table, built from procedural_bake.slang: every bake
        # shader that dispatches through it has to name its output to match.
        # Each bake allocates its own block from the immediate partition (a mip
        # per dispatch), so the sites are one per write call.
        Case(
            "bakeHeapBindings",
            "procedural_bake.slang",
            [
                ("src/render/RenderProcedural.cpp", "ctx, bakeHeapBindings"),
                ("src/render/RenderInternal.hpp", "ctx, bakeHeapBindings"),
                ("src/render/IBLProcessor.hpp", "impl.bakeHeapBindings"),
            ],
        ),
    ]


BAKE_TABLE_SHADERS = ["procedural_bake.slang", "smaa_lut.slang", "brdf_lut.slang", "ibl_bake.slang"]


def main() -> int:
    ok = True
    texts = {GRAPH: (REPO / GRAPH).read_text()}
    heaps_text = (REPO / HEAPS).read_text()

    # The old per-pass variant reservations are gone: a write hands back the
    # block its dispatch must push, so any of these would mean a call site still
    # reasons in reserved variant indices.
    for token in ("VariantBase", "VariantSlot", "VariantSlotOf", "variantCount", "slotBlockBase", "SkipWrite"):
        for path in sorted(REPO.glob("src/**/*.*")):
            if path.suffix not in (".cpp", ".hpp", ".inl"):
                continue
            if token in path.read_text():
                rel = path.relative_to(REPO)
                print(f"\n!! {rel} still mentions {token}; writes own their block now")
                ok = False

    for case in cases():
        for text_path, token in case.sites:
            if text_path not in texts:
                texts[text_path] = (REPO / text_path).read_text()
            assert token in texts[text_path], f"{case.label}: token gone from {text_path}: {token!r}"

        per_site = []
        for text_path, token in case.sites:
            text = texts[text_path]
            found = write_sites(text, token)
            assert found, f"{case.label}: no write site for {token!r} in {text_path}"
            for statement, site in found:
                # The block base the write returns is what the dispatch pushes:
                # a dropped result would leave the pass reading the previous
                # contents of its block, so it is checked, not assumed. A chain
                # step carries the slots but lets ComputeChain do the writing.
                if "WriteHeapParameters(" in statement:
                    capture = re.search(r"([\w\[\]]+)\s*=\s*[\w.:]+WriteHeapParameters\(", statement)
                    if not capture:
                        print(f"\n!! {case.label}: a WriteHeapParameters result is dropped in {text_path}: {statement.strip()[:80]}")
                        ok = False
                    elif len(re.findall(r"\b" + re.escape(capture.group(1).split("[")[0]) + r"\b", text)) < 2:
                        print(f"\n!! {case.label}: block {capture.group(1)} is written but never dispatched with ({text_path})")
                        ok = False
                per_site.append(site)

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

            print(f"\n=== {case.label} [{case.shader} {variant}] ({len(per_site)} write site(s)) ===")
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
                if got not in (None, kind):
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

    # Coverage: every WriteHeapParameters call in the tree has to be reachable
    # from the case table, so a new pass cannot start writing blocks without a
    # check that its names match the shader it feeds.
    # A dispatch's block argument is what WriteHeapParameters returned. A frame
    # index, a mip, a parity or a chain step reaching a dispatch where the base
    # belongs is the shape the old per-pass variant reservations had, and it
    # compiles as a plain uint32_t just as well.
    print("\n=== dispatch block arguments ===")
    dispatch_re = re.compile(r"\b(?:DispatchHeap\w*|ExecuteHeap|ExecuteVariantHeap)\s*\(")
    stale_arg = re.compile(r"[,(]\s*(fIdx|frameIndex|mip|parity|step|variant)\s*[,)]")
    for path in sorted(REPO.glob("src/**/*.*")):
        if path.suffix not in (".cpp", ".hpp", ".inl"):
            continue
        rel = str(path.relative_to(REPO))
        if rel.startswith("src/vulkan/pipeline/"):
            continue  # the helpers define the arguments, they do not pass them
        text = path.read_text()
        for m in dispatch_re.finditer(text):
            statement = statement_at(text, m.start())
            hit = stale_arg.search(statement)
            if hit:
                line = text[:m.start()].count("\n") + 1
                print(f"  !! {rel}:{line}: dispatch takes '{hit.group(1)}' where a block base belongs")
                ok = False

    if not signature_parity():
        ok = False
    if not local_lambda_arity():
        ok = False
    if not image_write_field_shapes():
        ok = False

    print("\n=== write-site coverage ===")
    covered = {(path, token) for case in cases() for path, token in case.sites}
    # The helpers themselves (and the headers that only declare the write) carry
    # the call sites' contract, not a shader's binding list.
    core = {
        "src/vulkan/pipeline/ComputePass.hpp",
        "src/vulkan/pipeline/Postprocessing.inl",
        "src/vulkan/pipeline/Postprocessing.hpp",
        "src/vulkan/pipeline/HeapBindings.hpp",
        "src/vulkan/pipeline/DescriptorHeap.hpp",
        "src/vulkan/pipeline/DescriptorWrites.hpp",
    }
    seen_files = {}
    for path in sorted(REPO.glob("src/**/*.*")):
        if path.suffix not in (".cpp", ".hpp", ".inl"):
            continue
        rel = str(path.relative_to(REPO))
        text = path.read_text()
        if "WriteHeapParameters(" not in text:
            continue
        found = sum(1 for m in re.finditer(r"WriteHeapParameters\(", text) if "WriteHeapParameters(" in statement_at(text, m.start()))
        if rel in core:
            continue
        seen_files[rel] = found
        if not any(rel == path_ for path_, _ in covered):
            print(f"  !! {rel} writes descriptor blocks but has no case in this checker")
            ok = False
    for rel, count in seen_files.items():
        print(f"  {rel:<44} {count} write statement(s)")
    print("\n" + ("ALL CONSISTENT" if ok else "INCONSISTENCIES FOUND"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
