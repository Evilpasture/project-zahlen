#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep image codec implementations private and Core codec-free."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[1]

def main():
    errors = []
    for folder in ('src', 'include', 'modules', 'plugins', 'extensions', 'utils', 'tests', 'samples', 'app'):
        for p in (ROOT / folder).rglob('*'):
            if not p.is_file() or p.suffix not in ('.cpp', '.c', '.hpp', '.h', '.cppm'): continue
            name = p.relative_to(ROOT).as_posix()
            text = p.read_text()
            if name != 'extensions/Image/Image.cpp' and re.search(r'^\s*#\s*(?:include\s*[<"]stb_image|define\s+STB_IMAGE)|\bstbi_\w+\s*\(', text, re.M):
                errors.append(f'{name}: image codec escaped its private implementation')
            if folder in ('src', 'include', 'modules') and re.search(r'#\s*include\s*[<"]Image/', text):
                errors.append(f'{name}: Core must consume cooked pixels, not image codecs')
    cmake = (ROOT / 'extensions/Image/CMakeLists.txt').read_text()
    if 'zahlen_engine' in cmake or re.search(r'target_link_libraries\s*\(', cmake):
        errors.append('Image must remain engine-independent')
    for folder in ('cmake','plugins','extensions','utils','tests'):
        for p in (ROOT / folder).rglob('*'):
            if not p.is_file() or (p.name != 'CMakeLists.txt' and p.suffix != '.cmake'): continue
            name = p.relative_to(ROOT).as_posix()
            text = p.read_text()
            if 'zahlen_stb_image' in text: errors.append(f'{name}: removed raw codec target')
            if 'extern/stb' in text and name not in ('extensions/Image/CMakeLists.txt','utils/zcook/CMakeLists.txt','cmake/Toolchains.cmake'):
                errors.append(f'{name}: leaked STB search root')
    if errors:
        print('\n'.join(errors)); return 1
    print('Image boundary OK (one private codec implementation; no Core dependency).'); return 0
if __name__ == '__main__': raise SystemExit(main())
