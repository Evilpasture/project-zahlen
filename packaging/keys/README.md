# Release signing key

`zahlen-release.pub` is the public key used to verify official package releases.
CMake embeds its contents into `zpkg` during the build. `FINGERPRINT` contains
the first 32 hexadecimal characters of SHA-256 over the raw 32-byte public key.
The fingerprint appears in `zpkg key list`, signature files, `KEY-FINGERPRINT.txt`,
and release notes.

## Private key

The private key is a base64-encoded PKCS#8 Ed25519 PEM stored in the
`ZHLN_RELEASE_KEY_PEM` GitHub Actions secret. `.github/workflows/release.yml`
passes it to the release build. `scripts/package-toolchain.sh` checks that the
secret is present before signing. The private key does not belong in this
directory or in the repository.

Create a key pair on a trusted machine with:

```sh
scripts/bootstrap-release-key.sh
scripts/bootstrap-release-key.sh --verify <saved-secret-file>
```

## Rotation

`scripts/package-toolchain.sh` signs `SHA256SUMS` and `index.json`. A transition
release can carry signatures from both the old and new keys:

1. Generate the new key pair and commit the new `.pub` and `FINGERPRINT` files.
2. Store the new private key in `ZHLN_RELEASE_KEY_PEM` and the old private key
   in `ZHLN_RELEASE_KEY_PEM_PREVIOUS`.
3. Publish a release signed by both keys.
4. Build and publish a `zpkg` containing the new public key, then remove
   `ZHLN_RELEASE_KEY_PEM_PREVIOUS` from the workflow secrets.

`zpkg` accepts a signature from any trusted key. Existing installations with
the old key can verify the transition release; managers built with the new key
can verify it as well.

## CI checks

| Check | Where |
|---|---|
| Signing secret derives the checked-in public key | `scripts/package-toolchain.sh` |
| Published key, fingerprint, signatures and release notes agree | `scripts/check-release-consistency.sh` (before and after publishing) |
| Package and tool checksum files match the shipped archives and tools | same |
| Example projects and rendered frames are absent from release assets | same |
| `zpkg verify --require-signatures` accepts the package archives | same |
| Installation and upgrade between release generations | `consume` job, "Upgrade from the previous release" |
