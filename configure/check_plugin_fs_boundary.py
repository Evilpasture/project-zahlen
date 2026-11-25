#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
"""Configure-time governance check for the plugin filesystem boundary.

Asset importers, decoders, rasterizers, font loaders, and format/scripting
plugins under `plugins/` must never bypass the engine's Virtual File System
(`FS::VirtualFileSystem` / `COM::IAssetLibrary`) to touch the host OS
filesystem directly:

  1. No source or header under `plugins/` (outside the explicit opt-in
     host-only convenience headers allowlisted below) may include
     `<filesystem>`, `<fstream>`, `<Zahlen/FileSystem/Paths.hpp>`, POSIX
     filesystem headers (`<fcntl.h>`, `<unistd.h>`, `<dirent.h>`,
     `<sys/stat.h>`), or call `std::filesystem`, `std::*fstream`, `fopen`,
     `FS::Paths::`, or resvg's file/directory/system-font C APIs.
  2. No file inside `plugins/` (including `*Plugin.cpp`) may include the
     opt-in host convenience headers (`SVGFile.hpp`, `ReflectLuaFile.hpp`).
     Those headers exist solely for host binaries, CLI tools, and tests that
     explicitly read OS paths at the host boundary.

Hosts own mount policy (`IAssetLibrary::MountDirectory` with optional
`MountDirectoryOptions{.virtualPrefix, .caseInsensitive}`, or `MountPak`) and
pass virtual paths or byte spans across the plugin boundary.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGINS_ROOT = ROOT / "plugins"

SOURCE_SUFFIXES = frozenset({
    ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp",
    ".c", ".cc", ".cpp", ".cxx", ".cppm", ".ixx",
})

# Explicit opt-in host-only convenience headers. These are header-only inline
# wrappers that must NEVER be included by any translation unit or core header
# under `plugins/`.
HOST_FILE_WRAPPER_ALLOWLIST: dict[str, str] = {
    "plugins/SVG/SVGFile.hpp": "opt-in host-only disk-read wrapper over SVG::LoadData / RasterizeData",
    "plugins/lua/ReflectLuaFile.hpp": "opt-in host-only disk-read wrapper over ReflectLua::*String",
}

FORBIDDEN_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]('
    r'filesystem|fstream|Zahlen/FileSystem/Paths\.hpp|'
    r'fcntl\.h|unistd\.h|dirent\.h|sys/stat\.h|'
    r'(?:SVG/)?SVGFile\.hpp|(?:lua/)?ReflectLuaFile\.hpp'
    r')[>"]',
    re.MULTILINE,
)

FORBIDDEN_TOKEN_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("std::filesystem", re.compile(r"\bstd::filesystem\b")),
    ("std::ifstream/ofstream/fstream", re.compile(r"\bstd::(?:i|o)?fstream\b")),
    ("fopen", re.compile(r"(?:\bstd::)?\bfopen\s*\(")),
    ("FS::Paths::", re.compile(r"\bFS::Paths::")),
    ("resvg_parse_tree_from_file", re.compile(r"\bresvg_parse_tree_from_file\b")),
    ("resvg_options_load_font_file", re.compile(r"\bresvg_options_load_font_file\b")),
    ("resvg_options_load_system_fonts", re.compile(r"\bresvg_options_load_system_fonts\b")),
    ("resvg_options_set_resources_dir", re.compile(r"\bresvg_options_set_resources_dir\b")),
)


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments while preserving line counts."""
    def _blank_block(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    text = re.sub(r"/\*.*?\*/", _blank_block, text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def iter_plugin_sources():
    if not PLUGINS_ROOT.is_dir():
        return
    for path in sorted(PLUGINS_ROOT.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if any(part in {".git", "build", "__pycache__"} for part in path.parts):
            continue
        yield path


def main() -> int:
    violations: list[str] = []
    scanned = 0

    for path in iter_plugin_sources():
        rel = path.relative_to(ROOT).as_posix()
        scanned += 1
        if rel in HOST_FILE_WRAPPER_ALLOWLIST:
            continue

        raw = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments(raw)

        for match in FORBIDDEN_INCLUDE_RE.finditer(code):
            lineno = code[: match.start()].count("\n") + 1
            violations.append(
                f"{rel}:{lineno}: forbidden host-filesystem include '{match.group(1)}' "
                f"(plugins must read via VFS / IAssetLibrary or byte spans)"
            )

        for label, pattern in FORBIDDEN_TOKEN_PATTERNS:
            for match in pattern.finditer(code):
                lineno = code[: match.start()].count("\n") + 1
                violations.append(
                    f"{rel}:{lineno}: forbidden host-filesystem API '{label}' "
                    f"(plugins must read via VFS / IAssetLibrary or byte spans)"
                )

    if violations:
        print(
            "Plugin filesystem-boundary governance check FAILED:\n"
            + "\n".join(f"  - {v}" for v in violations),
            file=sys.stderr,
        )
        return 1

    print(
        f"Plugin filesystem-boundary check passed "
        f"({scanned} files scanned under plugins/, {len(HOST_FILE_WRAPPER_ALLOWLIST)} opt-in host wrappers allowlisted)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
