# Release signing key

`zahlen-release.pub` is the **long-lived** public key users trust once with
`zpkg key add`. `FINGERPRINT` is the first 32 hex characters of SHA-256 over the
raw 32-byte public key — the same value `zpkg key list`, the `.ed25519` signer
lines, `KEY-FINGERPRINT.txt` and the release notes print.

Because the key is committed to this repository, git history is the out-of-band
trust anchor: download the `.pub` from here (or from the Arch package, which
installs it to `/usr/share/zahlen/zahlen-release-key.pub`), verify the
fingerprint, and trust it once. A key that is only published inside the release
it signs proves nothing.

## The private half

The private key is a base64-encoded PKCS#8 ed25519 PEM held in the
`ZHLN_RELEASE_KEY_PEM` GitHub Actions secret. `.github/workflows/release.yml`
passes it into the release build container; `scripts/package-toolchain.sh`
refuses to publish if it is missing, instead of quietly generating a throwaway
key. Nothing in this directory is secret.

Bootstrap once on a trusted machine:

```sh
scripts/bootstrap-release-key.sh          # prints the secret; never commit it
scripts/bootstrap-release-key.sh --verify <saved-secret-file>
```

## Rotation (deliberate only)

`scripts/package-toolchain.sh` signs `SHA256SUMS` and `index.json` with one line
per signer, and `zpkg` accepts a signature from *any* trusted key. For a
rotation:

1. Generate the new pair and commit the new `.pub` + `FINGERPRINT`.
2. Put the **new** private key in `ZHLN_RELEASE_KEY_PEM` and the **old** one in
   `ZHLN_RELEASE_KEY_PEM_PREVIOUS`.
3. Publish one dual-signed release, so stores holding either key still install.
4. Tell users to trust the new key, then drop `ZHLN_RELEASE_KEY_PEM_PREVIOUS`.

Users who pinned the old key can still verify the transition release; users who
never trusted a key can add the new one. `scripts/check-release-consistency.sh`
fails if a release is signed by a key that is not the pinned one, so a silent
rotation cannot reach a published asset.

## What CI checks

| Check | Where |
|---|---|
| secret present, derived public key equals `packaging/keys/zahlen-release.pub` | `scripts/package-toolchain.sh` |
| `KEY-FINGERPRINT.txt`, `.ed25519` signers, release notes and pinned fingerprint all agree | `scripts/check-release-consistency.sh` (pre-publish and post-publish) |
| `SHA256SUMS` / `SHA256SUMS.tools` cover exactly the shipped files | same |
| no `cube-consumer-*` / `zahlen-cube-*.png` example assets remain | same |
| `zpkg verify --require-signatures` succeeds with only the pinned key trusted | same |
| an existing store can install the new release without any `key add` | `consume` job, "Upgrade from the previous release" |
