# Upgrading an installed Zahlen toolchain

Two different things carry a version number:

* **zpkg**, the package manager, uses CalVer (`YYYY.MM.DD`, e.g. `2026.11.25`).
  On Arch it comes from `zahlen-zpkg`. `zpkg --version` shows it.
* **the installed engine toolchain** uses the engine release tag
  (`0.6.0-gnu-x86_64-release`, `v0.6.0`). `zpkg list` shows it.

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
binary keeps the toolchain it was built against. Reconfigure with the new prefix:

```sh
cmake -S my-game -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_PREFIX_PATH="$(zpkg prefix)"
cmake --build build
```

## Keys

The project signing key is long-lived and pinned at
[`packaging/keys/zahlen-release.pub`](../packaging/keys/zahlen-release.pub).
Trust it **once**:

```sh
zpkg key add packaging/keys/zahlen-release.pub --name "zahlen release"
# Arch users: /usr/share/zahlen/zahlen-release-key.pub
zpkg key list
```

Official installs always require a trusted signature, so an unannounced key
rotation would fail the upgrade. A deliberate rotation is dual-signed: the
transition release carries one signature line for the old key and one for the
new, so either store keeps working. If you do see
`no trusted key with fingerprint ...`, verify the fingerprint out of band
before trusting anything — do not just add the key from the same release page.

## Cleanup

```sh
zpkg remove 0.5.0-gnu-x86_64-release          # old toolchain, once the game works
zpkg key remove <old-fingerprint>              # only after you no longer need to
                                               # verify or reinstall that release
```

Removing an old key means old release archives can no longer be verified.

## If you prefer the explicit form

`sync` is a convenience over these commands; both are valid:

```sh
zpkg install official --version v0.6.0        # all packages of a pinned release
# or: zpkg install --version v0.6.0           # Application + its dependencies
zpkg use 0.6.0-gnu-x86_64-release
```

For a fresh install and the game build itself, see [Consuming Zahlen](CONSUMING.md).
