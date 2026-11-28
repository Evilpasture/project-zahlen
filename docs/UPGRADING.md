# Upgrading an installed Zahlen toolchain

Two different things carry a version number:

* **zpkg**, the package manager, uses CalVer (`YYYY.MM.DD`).
  On Arch it comes from `zpkg` (which replaces the former `zahlen-zpkg` package). `zpkg --version` shows it.
* **the installed engine toolchain** uses the engine release tag
  (`0.7.0-gnu-x86_64-release`, `v0.7.0`). `zpkg list` shows it.

Upgrading the PKGBUILD bumps **only the manager**. It does not touch the
per-user store at `$ZHLN_ROOT/store/<toolchain-id>/`, so an engine upgrade is a
separate step. Each release is installed into its own immutable toolchain
directory; nothing is overwritten in place, and `zpkg install` deliberately
leaves the active toolchain alone.

## Normal upgrade

```sh
zpkg --version                 # manager CalVer
zpkg list                      # installed toolchains, active marked with *
zpkg sync --activate           # install the active toolchain's package set from
                               # the latest official release, then make it active
zpkg list
zpkg prefix                    # now the new toolchain's prefix
```

`zpkg sync` reinstalls exactly the packages you currently have (plus their
dependency closure) from the official release into the new toolchain id. It
never guesses: a release that dropped or renamed a package fails loudly instead
of silently shrinking your prefix. Flags:

| Flag | Effect |
|---|---|
| `--version <tag>` | sync to a specific release tag instead of `latest` |
| `--from <remote>` | sync from a configured remote instead of `official` |
| `--activate` | run `zpkg use <new-id>` after a successful install |
| `--dry-run` | print the package set and target without downloading anything |
| `--force` | passed through to `install` |

Without `--activate`, `sync` installs the new toolchain and prints the
`zpkg use <id>` command, leaving your current build alone.

## Rebuild your game

Switching the active toolchain only affects **future** builds; an already-linked
binary keeps the toolchain it was built against. Reconfigure with the new
installed toolchain file:

```sh
cmake -S my-game -B build --toolchain="$(zpkg prefix)/share/zahlen/ZahlenToolchain.cmake"
cmake --build build
```

## Cleanup

```sh
zpkg remove 0.5.0-gnu-x86_64-release  # old toolchain, once the game works
```

## If you prefer the explicit form

`sync` is a convenience over these commands; both are valid:

```sh
zpkg install official --version v0.7.0        # all packages of a pinned release
# or: zpkg install --version v0.7.0           # Application + its dependencies
zpkg use 0.7.0-gnu-x86_64-release
```

For a fresh install and the game build itself, see [Consuming Zahlen](CONSUMING.md).
