#!/usr/bin/env python3
"""Enforce the backend contract: render is VkD3D-only, backends are sealed, vkd3d forks.

Four rules, from the backend-contract review (Phase 5):

1. zahlen_render links zahlen_vkd3d and nothing else in the render stack.
   No To*/From*Vk or To*/From*D3D12 converter family anywhere in src/render
   (zahlen_render only ever links zahlen_vkd3d -- no exceptions), no includes
   or namespace references resolving into src/vulkan or src/d3d12, and no
   backend link dependency or include dir on the zahlen_render target.

2. zahlen_vulkan and zahlen_d3d12 never reach back up to zahlen_vkd3d.
   Zero contact: no <vkd3d/...> includes, no VkD3D:: names, and no CMake
   reference to zahlen_vkd3d from the backends' build files.

3. zahlen_vkd3d calls no raw Vulkan or D3D12. No raw API includes and no raw
   API identifiers under src/vkd3d; the backends are reached only through
   fork TUs that dispatch with a plain `if (VkD3D::APIType)` -- never through
   preprocessor selection -- calling ZHLN::Vk::X or ZHLN::D3D12::X. The raw
   header seam targets (zahlen_vulkan_address, zahlen_d3d12_address,
   Vulkan-Headers, ...) must not appear in zahlen_vkd3d's build either.

4. Each target owns its converters, and VkD3D orchestrates the ToVk and
   ToD3D12 calls. The To*/From*Vk family lives in src/vulkan (owner) and
   src/vkd3d (orchestrated from the fork TUs); the To*/From*D3D12 family in
   src/d3d12 (owner) and src/vkd3d. Anywhere else -- src/render above all --
   is a violation.

Occurrences are counted in comments too: reword comments rather than
nudging the rules.

Not enforced here, deliberately:

  * src/render/OpenGLHacks builds HostBlitSwapchain, a sibling target with
    its own raw-Vulkan host blit; it is not the zahlen_render target and
    lives outside this contract.
  * How the owning converters take their neutral inputs without naming
    VkD3D:: types in the backends (rule 2 forbids those names there). The
    vocabulary split that resolves this is a design step, not a lint.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GOVERNED_ROOTS = (
    "include",
    "src",
    "plugins",
    "extensions",
    "gameplay",
    "modules",
    "utils",
    "scripts",
    "tests",
    "samples",
    "app",
)
SOURCE_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx",
    ".h", ".hh", ".hpp", ".hxx",
    ".inl", ".ipp", ".cppm", ".ixx",
})
SKIP_DIRS = frozenset({".git", "build", "target", "__pycache__", ".cache", "node_modules"})

RENDER_TREE = "src/render"
RENDER_EXCLUDED = (f"{RENDER_TREE}/OpenGLHacks",)
RENDER_RULE_TARGETS = ("zahlen_render", "zahlen_shader_bytecode")
VKD3D_TREE = "src/vkd3d"
BACKEND_TREES = ("src/vulkan", "src/d3d12")

# Rules 1/4: the converter families and the trees allowed to mention them.
CONVERTER_RE = re.compile(r"(?<![A-Za-z0-9_])(?P<kind>To|From)(?P<api>Vk|D3D12)[A-Za-z0-9_]*")
CONVERTER_ALLOWED = {
    "Vk": frozenset({"src/vulkan", VKD3D_TREE}),
    "D3D12": frozenset({"src/d3d12", VKD3D_TREE}),
}

# Rule 1: render's closed world.
RENDER_LINK_ALLOWED = frozenset({"zahlen_vkd3d"})
RENDER_NAMESPACE_RE = re.compile(r"(?<![A-Za-z0-9_])(?:ZHLN::)?(?:Vk|D3D12)::")

# Rule 2: the seal on the backends.
VKD3D_NAME_RE = re.compile(r"(?<![A-Za-z0-9_])VkD3D(?![A-Za-z0-9_])")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

# Rule 3: raw API surface, fork dispatch, and the raw header seams.
RAW_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]((?:vulkan|d3d12|dxgi|wrl|d3dcommon|d3dcompiler|d3dx12|volk)[^>"]*)[>"]',
    re.MULTILINE,
)
RAW_IDENT_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:"
    r"vk[A-Z]\w*"                                  # vkCreateXxx
    r"|Vk(?!D3D)[A-Z]\w*"                          # VkImage, VkFormat -- not VkD3D
    r"|VK_[A-Z0-9_]+"                              # VK_FORMAT_...
    r"|ID3D12\w*|IDXGI\w*"
    r"|(?<!To)(?<!From)D3D12[A-Z_][A-Z0-9_]*"      # D3D12_..., D3D12Serialize... -- not ToD3D12...
    r")"
)
FORK_VK_RE = re.compile(r"(?<![A-Za-z0-9_])ZHLN::Vk::")
FORK_D3D12_RE = re.compile(r"(?<![A-Za-z0-9_])ZHLN::D3D12::")
BARE_BACKEND_RE = re.compile(r"(?<![A-Za-z0-9_])(?<!ZHLN::)(?:Vk|D3D12)::")
API_TYPE_RE = re.compile(r"(?<![A-Za-z0-9_])APIType(?![A-Za-z0-9_])")
API_MACRO_RE = re.compile(
    r"^\s*#\s*(?:if|elif|ifdef|ifndef)\b[^\n]*"
    r"(?:VULKAN[A-Z0-9_]*|D3D12[A-Z0-9_]*|D3D_API[A-Z0-9_]*|VK_API[A-Z0-9_]*|API_TYPE[A-Z0-9_]*|BACKEND_API[A-Z0-9_]*)"
)
RAW_SEAM_TARGETS = re.compile(
    r"(?i)(zahlen_vulkan_address|zahlen_d3d12_address|vulkan[-_]?headers|directx[-_]?headers|d3d12[-_]?headers|\bvolk\b)"
)
VKD3D_RAW_PATHS = re.compile(r"(?i)(src/vulkan|src/d3d12|extern/vulkan|extern/directx|volk)")

INCLUDE_ROOTS = (
    "include",
    "src",
    "src/render",
    "src/render/init",
    "src/vulkan",
    "src/d3d12",
    "src/vkd3d/include",
)


def iter_sources(roots: tuple[str, ...], exclude: tuple[str, ...] = ()) -> list[Path]:
    found: list[Path] = []
    for name in roots:
        base = ROOT / name
        if not base.is_dir():
            continue
        stack = [base]
        while stack:
            directory = stack.pop()
            with _scandir(directory) as it:
                for entry in it:
                    if entry.is_dir(follow_symlinks=False):
                        if entry.name not in SKIP_DIRS:
                            stack.append(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False):
                        path = Path(entry.path)
                        if path.suffix.lower() not in SOURCE_SUFFIXES:
                            continue
                        rel = path.relative_to(ROOT).as_posix()
                        if any(rel == ex or rel.startswith(ex + "/") for ex in exclude):
                            continue
                        found.append(path)
    return sorted(found)


def _scandir(directory: Path):
    import os

    return os.scandir(directory)


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def resolve_include(source: Path, spec: str) -> Path | None:
    """Resolve an include the way the governed targets' include roots allow."""
    candidate = (source.parent / spec).resolve()
    if candidate.is_file():
        return candidate
    for root in INCLUDE_ROOTS:
        candidate = (ROOT / root / spec).resolve()
        if candidate.is_file():
            return candidate
    return None


def tree_of(path: Path) -> str | None:
    rel = path.relative_to(ROOT).as_posix()
    for tree in ("src/vulkan", "src/d3d12", "src/vkd3d", RENDER_TREE):
        if rel == tree or rel.startswith(tree + "/"):
            return tree
    return None


def cmake_tokens(text: str, func: str, target: str) -> list[tuple[str, int]]:
    """Every argument token of `func(target ...)`, with its line number."""
    tokens: list[tuple[str, int]] = []
    for match in re.finditer(rf"{func}\s*\(\s*([^\s\)]+)([^)]*)\)", text):
        if match.group(1) != target:
            continue
        offset = match.start(2)
        for token in match.group(2).split():
            tokens.append((token, line_of(text, offset + match.group(2).find(token))))
    return tokens


def check_render_rule(violations: list[str]) -> None:
    # 1a/4: the converter family does not exist in src/render, at all.
    for source in iter_sources((RENDER_TREE,)):
        text = source.read_text(encoding="utf-8", errors="ignore")
        for match in CONVERTER_RE.finditer(text):
            violations.append(
                f"rule 1: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"converter '{match.group(0)}' in src/render -- zahlen_render links only zahlen_vkd3d "
                f"and never converts; VkD3D orchestrates the ToVk/ToD3D12 calls"
            )

    # 1c: no includes or namespace references resolving into the backends.
    for source in iter_sources((RENDER_TREE,), exclude=RENDER_EXCLUDED):
        text = source.read_text(encoding="utf-8", errors="ignore")
        for match in INCLUDE_RE.finditer(text):
            spec = match.group(1)
            resolved = resolve_include(source, spec)
            owner = tree_of(resolved) if resolved is not None else None
            if owner in ("src/vulkan", "src/d3d12"):
                violations.append(
                    f"rule 1: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                    f"include '{spec}' resolves into {owner} -- zahlen_render links only zahlen_vkd3d"
                )
        for match in RENDER_NAMESPACE_RE.finditer(text):
            violations.append(
                f"rule 1: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"backend namespace '{match.group(0)}' in zahlen_render's sources"
            )

    # 1b: the zahlen_render target links zahlen_vkd3d and nothing else.
    cmake = ROOT / RENDER_TREE / "CMakeLists.txt"
    if cmake.is_file():
        text = cmake.read_text(encoding="utf-8", errors="ignore")
        for target in RENDER_RULE_TARGETS:
            for token, line in cmake_tokens(text, "target_link_libraries", target):
                if token.startswith("$") or token in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                    continue
                if token not in RENDER_LINK_ALLOWED:
                    violations.append(
                        f"rule 1: {cmake.relative_to(ROOT)}:{line}: "
                        f"{target} links '{token}' -- zahlen_render links only zahlen_vkd3d (no exceptions)"
                    )
            for token, line in cmake_tokens(text, "target_include_directories", target):
                if re.search(r"(?i)(src/vulkan|src/d3d12)", token):
                    violations.append(
                        f"rule 1: {cmake.relative_to(ROOT)}:{line}: "
                        f"{target} adds include dir '{token}' -- backends stay outside zahlen_render"
                    )


def check_backends_sealed(violations: list[str]) -> None:
    for tree in BACKEND_TREES:
        for source in iter_sources((tree,)):
            text = source.read_text(encoding="utf-8", errors="ignore")
            for match in INCLUDE_RE.finditer(text):
                spec = match.group(1)
                if "vkd3d" in spec.replace("\\", "/").lower():
                    violations.append(
                        f"rule 2: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                        f"include '{spec}' reaches into zahlen_vkd3d -- backends never reach up"
                    )
                    continue
                resolved = resolve_include(source, spec)
                if resolved is not None and tree_of(resolved) == "src/vkd3d":
                    violations.append(
                        f"rule 2: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                        f"include '{spec}' resolves into src/vkd3d -- backends never reach up"
                    )
            for match in VKD3D_NAME_RE.finditer(text):
                violations.append(
                    f"rule 2: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                    f"'VkD3D' named in {tree} -- zero contact with zahlen_vkd3d"
                )
        cmake = ROOT / tree / "CMakeLists.txt"
        if cmake.is_file():
            text = cmake.read_text(encoding="utf-8", errors="ignore")
            for match in re.finditer(r"zahlen_vkd3d", text):
                violations.append(
                    f"rule 2: {cmake.relative_to(ROOT)}:{line_of(text, match.start())}: "
                    f"build file names zahlen_vkd3d -- backends never reach up"
                )


def check_vkd3d_raw_free(violations: list[str]) -> None:
    for source in iter_sources((VKD3D_TREE,)):
        text = source.read_text(encoding="utf-8", errors="ignore")
        for match in RAW_INCLUDE_RE.finditer(text):
            violations.append(
                f"rule 3: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"raw API include '{match.group(1)}' -- zahlen_vkd3d calls no raw Vulkan or D3D12"
            )
        for match in RAW_IDENT_RE.finditer(text):
            violations.append(
                f"rule 3: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"raw API identifier '{match.group(0)}' -- the fork TUs call backend verbs, never the raw API"
            )
        for match in API_MACRO_RE.finditer(text):
            violations.append(
                f"rule 3: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"preprocessor API selection -- use a plain if (VkD3D::APIType), never macros"
            )
        # Fork discipline: backend symbols only as paired ZHLN::-qualified fork calls.
        has_vk = FORK_VK_RE.search(text) is not None
        has_d3d = FORK_D3D12_RE.search(text) is not None
        if (has_vk or has_d3d) and API_TYPE_RE.search(text) is None:
            violations.append(
                f"rule 3: {source.relative_to(ROOT)}: "
                f"calls a backend verb without dispatching on VkD3D::APIType in the same file"
            )
        if has_vk != has_d3d and (has_vk or has_d3d):
            violations.append(
                f"rule 3: {source.relative_to(ROOT)}: "
                f"one-sided fork -- a fork TU selects ZHLN::Vk::X or ZHLN::D3D12::X and names both"
            )
        for match in BARE_BACKEND_RE.finditer(text):
            violations.append(
                f"rule 3: {source.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"unqualified backend call '{match.group(0)}' -- fork calls are spelled ZHLN::Vk:: / ZHLN::D3D12::"
            )

    cmake = ROOT / VKD3D_TREE / "CMakeLists.txt"
    if cmake.is_file():
        text = cmake.read_text(encoding="utf-8", errors="ignore")
        for match in RAW_SEAM_TARGETS.finditer(text):
            violations.append(
                f"rule 3: {cmake.relative_to(ROOT)}:{line_of(text, match.start())}: "
                f"raw header seam '{match.group(0)}' -- zahlen_vkd3d reaches the backends only through their verbs"
            )
        for call in ("target_link_libraries", "target_include_directories"):
            for match in re.finditer(rf"{call}\s*\(([^)]*)\)", text):
                for token in match.group(1).split():
                    if VKD3D_RAW_PATHS.search(token):
                        violations.append(
                            f"rule 3: {cmake.relative_to(ROOT)}:{line_of(text, match.start())}: "
                            f"{call} path '{token}' points at raw backend headers"
                        )


def check_converter_ownership(violations: list[str]) -> None:
    for source in iter_sources(GOVERNED_ROOTS):
        rel = source.relative_to(ROOT).as_posix()
        text = source.read_text(encoding="utf-8", errors="ignore")
        for match in CONVERTER_RE.finditer(text):
            allowed = CONVERTER_ALLOWED[match.group("api")]
            if any(rel == tree or rel.startswith(tree + "/") for tree in allowed):
                continue
            violations.append(
                f"rule 4: {rel}:{line_of(text, match.start())}: "
                f"converter '{match.group(0)}' outside its owners "
                f"({'/'.join(sorted(allowed))}) -- each target owns its converters"
            )


def main() -> int:
    violations: list[str] = []

    check_render_rule(violations)
    check_backends_sealed(violations)
    check_vkd3d_raw_free(violations)
    check_converter_ownership(violations)

    if violations:
        print(f"Backend contract violated ({len(violations)} finding(s)):", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print(
            "Fix direction: the raw converters and barrier emission move out of src/vkd3d into their "
            "owning backends (src/vulkan owns ToVk, src/d3d12 owns ToD3D12); zahlen_vkd3d keeps the "
            "template machinery and fork TUs that dispatch with if (VkD3D::APIType); src/render drops "
            "every converter call and backend include -- zahlen_render links zahlen_vkd3d only.",
            file=sys.stderr,
        )
        return 1

    scanned = len(iter_sources(GOVERNED_ROOTS))
    print(
        "Backend contract OK "
        f"({scanned} sources scanned: zahlen_render links zahlen_vkd3d only; converters confined to "
        "their owners and VkD3D orchestration; backends sealed from zahlen_vkd3d; zahlen_vkd3d fork-only)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
