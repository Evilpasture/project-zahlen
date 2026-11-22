#!/usr/bin/env python3
"""Offline CLI acceptance tests; real packages and signed local indexes."""
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

binary = str(Path(sys.argv[1]).resolve())
openssl = sys.argv[2] if len(sys.argv) > 2 else None

with tempfile.TemporaryDirectory(prefix="zpkg-cli-") as tmp:
    root = Path(tmp)
    env = dict(os.environ, ZHLN_ROOT=str(root / "store"))

    def run(*args, code=0, contains=None):
        result = subprocess.run([binary, *map(str, args)], env=env,
                                capture_output=True, text=True, timeout=20)
        assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
        if contains:
            assert contains in result.stdout + result.stderr, (args, result.stdout, result.stderr)
        return result.stdout

    # One help text, whatever asked for it.
    usage = run(contains="Usage: zpkg")
    assert run("install", "--help") == usage
    assert run("help", "install") == usage
    assert run("--help") == usage
    assert "--only <a,b,...>" in usage and "--from" not in usage
    run("instal", code=2, contains="unknown command")
    for args in [("install", "--require-signaturs"),
                 ("install", "--require-signatures=no"),
                 ("list", "--from", "local"),
                 ("install", "--from"),
                 ("install", "--only="),
                 ("install", "--from", "local", "--from", "other")]:
        run(*args, code=2)
    # Usage failures must not create a store.
    assert not (root / "store").exists()
    run("install", code=1, contains="zpkg key add")
    run("install", "application", code=1, contains="zpkg key add")
    run("install", "application", "image", code=1, contains="zpkg key add")
    run("install", "application", "--only", "image", code=2, contains="not both")
    run("install", "application", "--version", "../bad", code=2, contains="invalid release tag")
    run("install", "application", "--from", "local", "--version", "v1", code=2)
    run("install", "./missing.tar.gz", code=1)

    published = root / "published"
    published.mkdir()
    toolchain = "0.1.0-cli-x86_64-release"
    metadata = dict(format=1, id=toolchain, version="0.1.0", commit="abc1234",
                    compiler="GNU", compiler_version="16.2.0", processor="x86_64",
                    profile="release", abi="deadbeef")
    for component, dependencies in [("Engine", []), ("Application", ["zahlen-engine"]), ("Image", [])]:
        package = "zahlen-" + component.lower()
        manifest = dict(format=1, package=package, component=component,
                        toolchain=toolchain, profile="release", requires=dependencies,
                        provides=["Zahlen::" + component])
        with tarfile.open(published / (package + ".tar.gz"), "w:gz") as archive:
            for name, data in {
                "usr/share/zahlen/toolchain.json": json.dumps(metadata),
                "usr/share/zahlen/manifests/" + package + ".json": json.dumps(manifest),
                "usr/lib/" + package + ".txt": "fixture\n",
            }.items():
                data = data.encode()
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
    run("index", published, "--out", published / "index.json")
    run("remote", "add", "official", published, code=1, contains="built-in")
    run("remote", "add", "local", published)
    # Shorthand is signed by default, even for a configured local remote.
    run("install", "application", "--from", "local", code=1, contains="unsigned index")
    assert not (root / "store/store" / toolchain).exists()

    if openssl:
        def crypto(*args):
            return subprocess.check_output([openssl, *map(str, args)], stderr=subprocess.DEVNULL)

        private = root / "private.pem"
        crypto("genpkey", "-algorithm", "ed25519", "-out", private)
        public = crypto("pkey", "-in", private, "-pubout", "-outform", "DER")[-32:]
        fingerprint = hashlib.sha256(public).hexdigest()[:32]
        key = root / "key.pub"
        key.write_bytes(base64.b64encode(public) + b"\n")
        signature = root / "signature.bin"
        crypto("pkeyutl", "-sign", "-inkey", private, "-rawin", "-in", published / "index.json", "-out", signature)
        (published / "index.json.ed25519").write_text(fingerprint + " " + base64.b64encode(signature.read_bytes()).decode() + "\n")
        run("install", "application", "--from", "local", code=1, contains="no trusted signature")
        run("key", "add", key)
        run("install", "application", "--from=local", contains="Build your game with:")
        manifests = root / "store/store" / toolchain / "usr/share/zahlen/manifests"
        assert sorted(p.name for p in manifests.iterdir()) == ["zahlen-application.json", "zahlen-engine.json"]
        prefix = run("prefix").strip()
        run("install", "application", "--from", "local", contains="Already installed:")
        assert run("prefix").strip() == prefix
        assert run("install", "application", "--from", "local", "--quiet") == ""
        run("install", "application", "image", "--from", "local")
        assert (manifests / "zahlen-image.json").exists()
        # Unknown selection and tampered signed metadata remain errors.
        run("install", "typo", "--from", "local", code=1, contains="nothing matching")
        index = published / "index.json"
        index.write_text(index.read_text() + " ")
        run("install", "application", "--from", "local", code=1, contains="does not verify")

    # Existing bare remote and remote:component forms keep working for unsigned
    # developer fixtures. They are not the secure release shortcut.
    (published / "index.json.ed25519").unlink(missing_ok=True)
    run("install", "local")
    run("install", "local:application")
    run("install", published)

print("PASS: zpkg CLI")
