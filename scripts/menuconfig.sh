#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/menuconfig.sh -- open the Zahlen configuration TUI.
#
# Runs scripts/menuconfig_mconf.py -- a menuconfig front end over Kconfiglib that
# reproduces the Linux kernel's `make menuconfig` screen -- so that the .config it
# edits is the build directory's, not the current directory's, and so that a
# missing dependency is a sentence instead of a traceback.
#
#   ./scripts/menuconfig.sh                 # configure build/menuconfig
#   ./scripts/menuconfig.sh build/asan      # or any other build directory
#
# After it exits, re-run cmake in the same build directory. The preset
# (<name>-menuconfig) does both steps for you:
#
#   cmake --preset dev-menuconfig
#
# Kconfiglib comes from the repo's .venv (see scripts/requirements-menuconfig.txt)
# or from PATH; nothing is installed automatically.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_root}/build/menuconfig}"

# --- a menu needs a terminal -------------------------------------------------
# Checked before anything else: `cmake --preset <name>-menuconfig` from a CI
# script, an editor task or a docker run -T would otherwise die inside curses
# with "setupterm: could not find terminal", which reads like a broken build.
if [[ ! -t 0 || ! -t 1 ]]; then
    echo "menuconfig needs an interactive terminal (stdin and stdout are not a TTY)." >&2
    echo "Configure non-interactively instead: cmake --preset dev" >&2
    exit 1
fi

# --- a Python that can import kconfiglib -------------------------------------
# Kconfiglib is pure Python and small, but it is a dependency, and the rest of
# this tree builds without Python at all (cmake/PythonProbe.cmake). The menu is
# therefore opt-in: it never installs anything and never becomes a configure-time
# requirement for anyone who does not ask for it.
pick_python() {
    local candidates=()
    [[ -x "${repo_root}/.venv/bin/python3" ]] && candidates+=("${repo_root}/.venv/bin/python3")
    [[ -x "${repo_root}/.venv/Scripts/python.exe" ]] && candidates+=("${repo_root}/.venv/Scripts/python.exe")
    command -v python3 >/dev/null 2>&1 && candidates+=("$(command -v python3)")
    command -v python  >/dev/null 2>&1 && candidates+=("$(command -v python)")

    for py in "${candidates[@]}"; do
        if "$py" -c "import kconfiglib" >/dev/null 2>&1; then
            echo "$py"
            return 0
        fi
    done
    return 1
}

if ! python_bin="$(pick_python)"; then
    echo "menuconfig needs Kconfiglib, which is not importable by any Python on this host." >&2
    echo >&2
    echo "  Install it (either is fine):" >&2
    echo "    pip install -r ${repo_root}/scripts/requirements-menuconfig.txt" >&2
    echo "    ${repo_root}/.venv/bin/pip install -r ${repo_root}/scripts/requirements-menuconfig.txt" >&2
    echo >&2
    echo "Nothing was changed. The build itself does not need Kconfiglib." >&2
    exit 1
fi

# --- the menu ----------------------------------------------------------------
# KCONFIG_CONFIG is how menuconfig decides which file to load and save, so the
# build directory's .config is what gets edited. Setting it here rather than
# cd-ing into the build directory matters: menuconfig also resolves the Kconfig
# source files relative to where it runs, and those live in the source tree.
mkdir -p "${build_dir}"
export KCONFIG_CONFIG="${build_dir}/.config"

echo "Configuring ${KCONFIG_CONFIG}"
echo "Kconfig source: ${repo_root}/Kconfig"

"${python_bin}" "${repo_root}/scripts/menuconfig_mconf.py" \
    "${repo_root}/Kconfig"

echo
echo "Wrote ${KCONFIG_CONFIG}"
# The preset reads ${repo_root}/build/dev-menuconfig/.config, so only point at it
# when that is the directory that was just edited. Telling someone to re-run a
# preset that reads a different file is how a menu setting appears to do nothing.
if [[ "${build_dir}" == "${repo_root}/build/dev-menuconfig" ]]; then
    echo "Now reconfigure:  cmake --preset dev-menuconfig"
else
    echo "Now reconfigure:  cmake -B ${build_dir} -DZHLN_KCACHE_CONFIG=${KCONFIG_CONFIG}"
fi
