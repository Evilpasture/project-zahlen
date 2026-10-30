#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""PTY smoke test for --allow-debugger's real POSIX terminal/execve path.

Run without configuring the engine: python3 tools/ztest/tests/test_debugger_replay.py
The fake debugger records argv, environment and cwd; it never runs a worker.
"""

import errno
import os
import pathlib
import pty
import select
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
QUESTION = b"Launch debugger for this case? [Y/N]:"


@unittest.skipUnless(os.name == "posix", "PTY test requires POSIX")
class DebuggerReplaySmoke(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ztest replay spaces ")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = pathlib.Path(cls.temp.name)
        cls.work_dir = cls.directory / "worker dir"
        cls.tool_dir = cls.work_dir / "debugger tools"
        cls.work_dir.mkdir()
        cls.tool_dir.mkdir()
        cls.capture = cls.directory / "captured args.txt"
        cls.binary = cls.directory / "debugger replay smoke"
        cls.apple_binary = cls.directory / "simulated mac debugger replay"
        compiler = os.environ.get("CXX", "c++")
        cls.compiler = compiler
        command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-pedantic",
                   "-I" + str(ROOT / "tools/ztest"),
                   str(ROOT / "tools/ztest/tests/debugger_replay_smoke.cpp"),
                   str(ROOT / "tools/ztest/DebuggerReplay.cpp")]
        subprocess.run(command + ["-o", str(cls.binary)], check=True)
        # This also runs on Linux: it exercises the macOS LLDB argument/SIP
        # branch with an executable fake LLDB (not an Apple-only build).
        subprocess.run(command + ["-D__APPLE__", "-o", str(cls.apple_binary)], check=True)
        for tool_name in ("gdb", "lldb"):
            fake_tool = cls.tool_dir / tool_name
            fake_tool.write_text(
                "#!/bin/sh\n"
                "{\n"
                "  printf 'CWD=%s\\n' \"$(pwd)\"\n"
                "  printf 'MARKER=%s\\n' \"$ZTEST_MARKER\"\n"
                "  for arg do printf 'ARG=%s\\n' \"$arg\"; done\n"
                "} > \"$ZTEST_CAPTURE\"\n"
            )
            fake_tool.chmod(0o755)

    def setUp(self):
        self.capture.unlink(missing_ok=True)

    def _run_pty(self, reply: bytes, binary=None):
        binary = binary or self.binary
        pid, fd = pty.fork()
        if pid == 0:
            env = dict(os.environ)
            env["ZTEST_MARKER"] = "from-parent-env"
            os.execve(
                str(binary),
                [str(binary), "debugger tools", str(self.work_dir), str(self.capture)],
                env,
            )

        output = bytearray()
        answered = False
        exit_status = None
        deadline = time.monotonic() + 15
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if ready:
                    try:
                        chunk = os.read(fd, 4096)
                    except OSError as exc:
                        if exc.errno == errno.EIO:  # PTY slave closed
                            break
                        raise
                    if not chunk:
                        break
                    output.extend(chunk)
                    if not answered and QUESTION in output:
                        os.write(fd, reply)
                        answered = True
                if exit_status is None:
                    dead, status = os.waitpid(pid, os.WNOHANG)
                    if dead:
                        exit_status = status
                if exit_status is not None:
                    # The PTY may still have buffered debugger output; let the
                    # next read drain it before closing the master.
                    if not select.select([fd], [], [], 0.1)[0]:
                        break
            else:
                if exit_status is None:
                    os.kill(pid, 9)
                    os.waitpid(pid, 0)
                self.fail("debugger smoke test hung waiting for terminal input")
        finally:
            os.close(fd)
        if exit_status is None:
            _, exit_status = os.waitpid(pid, 0)
        return os.waitstatus_to_exitcode(exit_status), bytes(output)

    def test_command_construction(self):
        subprocess.run([str(self.binary)], check=True)

    def test_mac_signal_header_style_macro_compiles(self):
        # Darwin's signal.h defines sigemptyset(set) as a function-like macro.
        # A namespace-qualified ::sigemptyset would expand into invalid C++.
        shim = self.directory / "darwin-signal-macro.hpp"
        shim.write_text(
            "#include <signal.h>\n"
            "#undef sigemptyset\n"
            "#define sigemptyset(set) ((void)(set), 0)\n"
        )
        subprocess.run(
            [self.compiler, "-std=c++20", "-D__APPLE__", "-Wall", "-Wextra", "-Werror",
             "-I" + str(ROOT / "tools/ztest"), "-include", str(shim), "-fsyntax-only",
             str(ROOT / "tools/ztest/DebuggerReplay.cpp")],
            check=True,
        )

    def test_yes_replays_with_exact_argv_environment_and_cwd(self):
        exit_code, output = self._run_pty(b"y\n")
        self.assertEqual(exit_code, 0, output)
        captured = self.capture.read_text().splitlines()
        self.assertEqual(captured[0], "CWD=" + str(self.work_dir))
        self.assertEqual(captured[1], "MARKER=from-child-env")
        self.assertIn("ARG=--file=tests/case with spaces.cpp", captured)
        self.assertIn("ARG=--exact=tests/case with spaces.cpp::test_segfault", captured)
        self.assertNotIn("ARG=--ztest-worker", captured)
        self.assertNotIn("ARG=--skip-until=tests/case with spaces.cpp::test_segfault", captured)
        self.assertIn("ARG=/tmp/fake worker with spaces", captured)
        expected_flag = "ARG=--" if os.uname().sysname == "Darwin" else "ARG=--args"
        self.assertIn(expected_flag, captured)
        self.assertIn("ARG=run", captured)
        self.assertIn("ARG=bt", captured)

    def test_mac_lldb_replay_preserves_dyld_assignment(self):
        exit_code, output = self._run_pty(b"y\n", self.apple_binary)
        self.assertEqual(exit_code, 0, output)
        captured = self.capture.read_text().splitlines()
        self.assertIn('ARG=settings set target.env-vars "DYLD_LIBRARY_PATH=/tmp/library path"', captured)
        self.assertIn("ARG=--exact=tests/case with spaces.cpp::test_segfault", captured)
        self.assertIn("ARG=--", captured)
        self.assertNotIn("ARG=--args", captured)

    def test_no_never_starts_debugger(self):
        exit_code, output = self._run_pty(b"n\n")
        self.assertEqual(exit_code, 0, output)
        self.assertFalse(self.capture.exists())

    def test_piped_yes_without_terminal_cannot_start_debugger(self):
        result = subprocess.run(
            [str(self.binary), "--no-terminal"], input="y\n", text=True,
            capture_output=True, start_new_session=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse(self.capture.exists())

    def test_invalid_reply_reprompts_before_declining(self):
        exit_code, output = self._run_pty(b"?\nn\n")
        self.assertEqual(exit_code, 0, output)
        self.assertGreaterEqual(output.count(QUESTION), 2)
        self.assertFalse(self.capture.exists())


if __name__ == "__main__":
    unittest.main()
