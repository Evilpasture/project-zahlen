# Tests

The suite tests **public behaviour only**.

Allowed:

- `include/Zahlen/**`
- optional outer layers (`plugins/**`, `extensions/**`, `gameplay/**`, `import ZHLN.*` modules)
- this directory's framework (`TestsFramework.hpp`)
- third-party and standard-library headers needed to drive the public API

Forbidden:

- anything under `src/` (`engine/`, `render/`, cooker internals, system classes, …)
- adding `${PROJECT_SOURCE_DIR}/src` to a test include path

`configure/check_tests_public_api.py` runs at CMake configure time and fails the
build if a test includes engine internals.

GPU suites (`ZHLN_BUILD_GPU_TESTS`) judge public behaviour from the pixels
`RenderContext::CaptureFrame` hands back — camera aim, PBR response, UI layout,
and ray-traced reflection colour vs PBR F0/roughness — not private systems. The
pixels arrive in memory; the `.ppm`/`.png` files a run leaves behind are
diagnostic mirrors written by the test, never the thing the assertions read.
See `tests/INVARIANT_TESTING.md` for the no-absolute-pixels rule, quiet logs
(`ZHLN_TEST_LOG=verbose`), sample-asset coverage without committed GLBs,
and quarantine (`[ QUARANTINE ]` = known open engine bug, still running).

## Optional reference compatibility

`<Zahlen/Core/Optional.hpp>` follows the `FunctionRef` compatibility pattern:
use **`ZHLN::Optional<T&>`**, not a replacement declaration in `namespace std`.

- With `__cpp_lib_optional >= 202506L`, `ZHLN::Optional<T>` aliases the native
  `std::optional<T>`, including the C++26 reference specialization.
- On older libraries (including the Bloomberg reflection toolchain), value
  `ZHLN::Optional<T>` still aliases `std::optional<T>`, while `T&` selects a
  pointer-sized, trivially copyable borrowed-reference fallback.
- The fallback supports construction, rebinding assignment/emplace, const
  reference conversion, `reference_wrapper`, borrowing lvalue owning optionals,
  observers, `value_or`, swap/reset, monadic operations, zero-or-one borrowed
  iteration and value comparison. Hashing is not added to the reference type. Function and array references are also accepted;
  functions/unbounded arrays have no iterators, and arrays/functions have no
  `value_or`.
- Neither the wrapper nor its copies own the referred-to object. Keep it alive,
  and do not reset/destroy an owning optional while using a reference borrowed
  from it. A const wrapper does not make the target const; use `Optional<const T&>`.
- Like `FunctionRef`, the fallback is deliberately a safe subset: construction
  accepts pointer-compatible lvalues, not arbitrary user-defined reference
  conversions. Temporaries and temporary owning optionals are rejected. Ordered
  comparisons use the target's three-way comparison.
- Value optionals retain their library's API: on an older library their own
  `and_then`/`transform` cannot produce the custom reference type as if it were a
  native `std::optional`. The reference fallback's monadic operations can return
  either value optionals or reference wrappers. No standard feature macro is
  forged, and literal `std::optional<T&>` remains unavailable on those libraries.
- `value()` on an empty wrapper throws `std::bad_optional_access` with exceptions
  enabled and aborts in `-fno-exceptions` builds. `*`/`->` require engagement.

```cpp
#include <Zahlen/Core/Optional.hpp>

int first = 1;
int second = 2;
ZHLN::Optional<int&> borrowed = first;
borrowed = second; // Rebinds; first is unchanged.
*borrowed = 3;     // Mutates second.
```

`tests/core/TestOptional.cpp` is listed in `tests/CMakeLists.txt` (without CMake
globbing, so incremental builds stay exact) and auto-collected via C++26
reflection (`TestsAutoCollect.hpp`) with no separate test target, assertion
macro, or `RegisterSuites<...>` footer.

Compile-time assertions pin the alias, reference/const semantics, rejected
borrowings, and constant evaluation. Runtime cases cover rebinding, conversion,
monadic operations, iteration and comparisons. The empty-`value()` exception case
is included only when exceptions are enabled: a fatal no-exceptions check must
not terminate the worker process.

After building `ztest` and the test worker, run it with:

```sh
./build/bin/ztest tests/core/TestOptional.cpp
# or filter by directory / boolean expression:
./build/bin/ztest tests/assets/
./build/bin/ztest -k "Optional and not exception"
./build/bin/ztest --collect-only
```

## Zero-Boilerplate `namespace ZHLN::Test` Authoring

New test files write plain `test_*` functions directly inside
`namespace ZHLN::Test` (or `namespace ZHLN::Test { namespace { ... } }`) with
zero `struct *Suite`, `struct Tests`, or registration footers:

```cpp
#include "TestsFramework.hpp"

namespace ZHLN::Test {
namespace {

void test_quat_identity() {
    Expect(1 + 1) == 2;
}

ZHLN_ANNOTATION(Cases {
    NamedCase {"two", 2, 4},
    NamedCase {"ten", 10, 20},
})
void test_doubling_with_tempdir(TempDir& tmp, int x, int expected) {
    Expect(std::filesystem::is_directory(tmp.Path()));
    Expect(x * 2) == expected;
}

} // namespace
} // namespace ZHLN::Test
```

Add the `.cpp` path to the explicit source list in `tests/CMakeLists.txt` (no
`file(GLOB)`). `cmake/ZTest.cmake` generates a tiny idempotent epilogue wrapper
TU (`build/ztest_autogen/...`) that reflects `namespace ZHLN::Test` and primary-TU
suite structs at compile time. Both `void` and
`std::expected<void, ZHLN::ErrorCode>` return types are supported, and marks or
`UseFixtures<...>` placed on an enclosing single-level namespace inside
`ZHLN::Test` (`namespace ZHLN_ANNOTATION(UseFixtures<...>{}) { ... }`) apply to
every `test_*` function in that namespace.

## Running & Filtering with `ztest`

`ztest` (`tools/ztest/main.cpp`, built into `<build>/bin/ztest` when
`ZHLN_BUILD_TESTS=ON`) is the project's native pytest-style test orchestrator:

- `./build/bin/ztest` — collects individual `tests/.../File.cpp::test_*` items, shards
  CPU execution per source file in parallel across `-j N` isolated workers, and
  pools `GPU` workers per binary (`--cpu` or `--gpu` selects one pool).
- `./build/bin/ztest tests/assets/` or `./build/bin/ztest tests/core/TestMath.cpp` or
  `./build/bin/ztest tests/core/TestMath.cpp::test_scalar_math` — runs a directory,
  file, or single test node ID.
- `./build/bin/ztest -k "<expr>"` — boolean substring expression (`and`, `or`,
  `not`, parentheses) matched against `tests/.../File.cpp::test_name`.
- `./build/bin/ztest -m "<expr>"` — boolean mark/tag expression against CMake target
  labels (`cpu`, `gpu`, `fast`, `extras`) and C++26 `ZHLN_ANNOTATION(...)` marks
  (`Tag<"...">`, `Tags<...>`, `quarantine`, `skip`).
- `./build/bin/ztest --lf` — re-runs only items recorded in `<build>/.ztest_cache`
  from the previous failing run.
- `./build/bin/ztest --durations 10` — prints the slowest individual test items.
- Crash / timeout isolation: if an item crashes or hits its
  `Timeout<Seconds>` / watchdog deadline, `ztest` attributes the failure to that
  exact node ID and automatically respawns the remaining items in that file/group
  via `--skip-until`.
- `./build/current/bin/ztest --lf --allow-debugger` (or use any test selector):
  after an **attributed crash**, prints the normal `[ CRASH ]` diagnostics and
  asks `> Launch debugger for this case? [Y/N/S=skip remaining]:`. `N` (or
  Enter) declines this case only; `S` suppresses every further debugger prompt
  for this run without stopping the tests (normal parallel sharding resumes).
  `Y` opens LLDB on macOS (GDB on Linux, with the other as fallback),
  automatically runs **only that case**, prints `bt` at the stop, and leaves the
  debugger interactive. Exit the debugger with `quit` to resume the rest of the
  suite; the original crash remains a failure in the summary and `--lf` cache.
  The replay uses the same worker binary, working directory, target labels,
  filter/mark/strict settings, manifest and sandbox environment, validation
  override, and PATH prepends. It removes `--ztest-worker` (whose crash handler
  would hide the fault) and any `--skip-until` from a restarted shard, then
  selects the crashed file and exact node ID. A terminal is required: if there
  is none, ztest warns and runs normally without prompts (never reading piped
  stdin). In interactive debugger mode it runs one worker at a time so another
  shard's watchdog cannot expire while you are at the debugger prompt; normal
  runs and the remainder of a run after `S` are parallel. Timeouts and crashes
  outside a named case cannot be replayed as an exact case and do not prompt.
  On Windows, an installed LLDB or GDB executable on PATH is used if present.
  The standalone PTY/fake-debugger smoke test is
  `python3 tools/ztest/tests/test_debugger_replay.py`.
- Crash stack traces: a worker arms `ZHLN::InstallWorkerCrashReports()`
  (`extensions/Signal/WorkerCrashReports.hpp`) in `--ztest-worker` mode. A fatal signal
  (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT) or `std::terminate` makes the worker
  close the in-flight case and stream a framed `@@ZTEST STACKTRACE-*` record from
  inside the handler -- async-signal-safe, no allocation, on an alternate signal
  stack -- before it exits with `128 + signal`. `ztest` symbolizes those frames
  against the worker binary with `addr2line` (set `ADDR2LINE=<path>` to pick a
  specific one, `llvm-addr2line`/`eu-addr2line` are accepted) and prints them
  under the `[ CRASH ]` line:

  ```
    [ CRASH ] tests/core/TestFoo.cpp::FooSuite::breaks
      Exception: SIGSEGV at address 0x0 on thread 41237
      case was still in flight: it never reached @@ZTEST END
      Stack trace (4 frames):
        0   in FooSuite::breaks() at tests/core/TestFoo.cpp:88
        1   in main at tests/main.cpp:6
        ...
  ```

  `--stack-frames <N>` caps the printed depth (0 = everything the worker
  captured), `--no-symbolize` prints raw `module+0xoffset` frames, and
  `ZHLN_ZTEST_SYMBOLIZE=0` / `ZHLN_ZTEST_STACK_FRAMES=<N>` set the same defaults
  from the environment.

  On macOS the worker walks its own Mach-O load commands at install time to map
  address ranges to images, and ships *link-time* frame addresses -- the vmaddr
  form `atos` and `llvm-addr2line` expect, not a distance from the image base.
  Apple ships no `addr2line`, so symbolization there needs `llvm-addr2line`
  or `llvm-symbolizer` on `PATH` (they come with `brew install llvm`); point
  `ADDR2LINE` at one if it lives outside `PATH` (it is read from the environment
  of the `ztest` process, so either `export` it or put the tool on `PATH`).
  Without either, frames still print in the `module+0xoffset` form:

  ```sh
  ADDR2LINE=/opt/homebrew/opt/llvm/bin/llvm-addr2line .build/bin/ztest
  ```

  Frames in `dyld` and `libsystem` resolve to symbol names only: Apple strips
  line tables from the system images, so those show `in _sigtramp` rather than
  `in _sigtramp at file:line`, which is expected.

  `std::terminate` is captured as well (a no-exceptions build has nowhere else
  to put `std::terminate()`, a bare `throw`, or a `noexcept` violation), so
  instead of a worker that just stops after `[ RUN ]` the offending case still
  gets a frame list. Windows workers emit the same
  records, but the win32 reader is synchronous and leaves frames as
  `module+0xoffset` (`llvm-symbolizer` resolves those by hand).
- Perf measurements (`ZHLN::Test::WaitIdle`): mark a case whose result depends on
  a quiet machine --

  ```cpp
  struct ZHLN_ANNOTATION(ZHLN::Test::WaitIdle<"frame budget"> {}) PerfSuite {
      auto upload_throughput() -> void { /* timed work */ }
  };
  ```

  `ztest` pulls that case out of its file's parallel shard and gives it a shard
  of its own. The shard is only scheduled once every other worker has exited and
  been reaped, so nothing of the suite is competing for CPU, the GPU queue, or
  the page cache while the case is timed; ordinary parallelism resumes the
  moment it returns. A running measurement also blocks new spawns, so a shard
  that was queued behind it cannot start in the middle of the window. When it
  runs, the isolation is reported rather than assumed:

  ```
    [ WAIT-IDLE ] Machine quiesced for tests/perf/TestPerf.cpp::PerfSuite::upload_throughput -- no other worker is running.
    [ PASS ] tests/perf/TestPerf.cpp::PerfSuite::upload_throughput (41.20 ms, exclusive: machine idle for 812.40 ms before start)
  ```

  "idle for N ms before start" is measured from the moment the run last became
  fully idle, so a case that had to wait for a long shard to drain shows the
  larger number. `--collect-only` marks such cases `[wait-idle]`. `-k`/`-m`
  filters apply as usual: a filter that excludes the case means no draining
  happens at all. On Windows each shard already owns the machine, so the mark
  only affects reporting there.

## Pytest-Style Fixtures, `@parametrize`, & Decomposing `Expect`

`TestsFramework.hpp` uses C++26 static reflection and P3394 annotations to
provide pytest ergonomics with zero preprocessor macros:

- **Reflected Fixture Dependency Injection (`FunctionFixture`, `SuiteFixture`,
  `SessionFixture`, `FixtureTraits<T>`, `UseFixtures<...>`):**
  Declare fixtures (`ZHLN::Test::TempDir&`, `ZHLN::Test::CaseInfo`,
  `ZHLN::ECS::Registry&`, `CookerFixture&`, `ZHLN::Test::Fixture::WorldFixture&`,
  `ZHLN::Test::Headless::PooledEngine&`, or any struct annotated with
  `ZHLN_ANNOTATION(ZHLN::Test::FunctionFixture{})` / `SuiteFixture{}` /
  `SessionFixture{}`) directly as test method parameters. Fixture constructors
  may themselves request same- or longer-lived fixtures, and each scope tears
  its instances down in LIFO order. Ambient RAII fixtures that need no method
  parameter (such as `TaskSystemScope`, `TaskSystemAndJoltScope`, or
  `Headless::SessionScope`) can be attached to a suite or method via
  `ZHLN_ANNOTATION(ZHLN::Test::UseFixtures<...>{})`.
- **Compile-Time Parameterized Cases (`Case`, `NamedCase`, `CaseValues`,
  `Cases`, `Param`):**
  Attach `ZHLN_ANNOTATION(ZHLN::Test::Cases{ Case{...}, NamedCase{"id", ...} })`,
  `CaseValues<V0, V1, ...>{}`, or stacked Cartesian `Param{...}` axes to a test
  method. Each row or Cartesian tuple expands into a distinct addressable
  `Suite::method[params]` case in `--collect-only`, `-k <expr>`, `--lf`, and
  `--exact` (`-k method` also matches all parameterized children of `method`),
  and freely mixes with injected fixture parameters in any position.
- **Macro-Free Expression-Decomposing `Expect(lhs)`:**
  Write natural comparisons (`ZHLN::Test::Expect(actual) == expected`, `!=`,
  `<`, `<=`, `>`, `>=`, `ZHLN::Test::Expect(cond)`, and
  `ZHLN::Test::Expect(val) == ZHLN::Test::Approx(target, eps)`). Source location
  is captured automatically via default constructor argument, mixed signed and
  unsigned integers compare safely via `std::cmp_*`, C-strings compare by text
  content, and mismatches print both decomposed `lhs` and `rhs` values via
  `ZHLN::Reflect::ToDebugString`.

## CDN and remote-asset cache validation

`TestRemoteAsset` covers automatic expiry and bounded disk retention through
the public `Remote::DiskCache` API, including startup/read/write sweeps,
legacy file timestamps, oversized entries, failed writes, abandoned staging
files, path/symlink safety, and concurrent copies/independent cache instances.
`TestCDN` verifies that the same policy reaches `Fetch`, `Load`, and
`RequestAsset`/`Poll`, using the loopback HTTP server rather than a live CDN.
The expiry fixtures backdate file modification times; they do not wait for TTLs.

Origin-backed entries are also tested for bounded metadata and whole-body
integrity, including same-sized corruption that passes the GLB header check.
Mutable, synchronized loopback resources exercise same-URL edits, persisted
ETags, Last-Modified fallback, missing validators, guarded 304 reuse/retries,
no-cache/no-store/max-age, redirects, HTTP errors, and both sync/async APIs.
Run normal and `ZHLN_DEV_MODE` builds: the default origin-check intervals differ.
Tests about intentionally warm local hits pin a positive interval explicitly.

Both suites are registered already when their optional targets are available:

```sh
cmake --build build --target ztest TestRemoteAsset TestCDN
./build/bin/ztest -k "RemoteAssetTestSuite or CDNTestSuite"
```

See [CDN caching](../extensions/net/CDN/README.md) for the policy contract.
