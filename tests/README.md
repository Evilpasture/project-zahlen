# Tests

The suite tests **public behaviour only**.

Allowed:

- `include/Zahlen/**`
- optional extras (`extras/**`, `ALife/**`, `import ZHLN.*` extras modules)
- this directory's framework (`TestsFramework.hpp`)
- third-party and standard-library headers needed to drive the public API

Forbidden:

- anything under `src/` (`engine/`, `render/`, cooker internals, system classes, …)
- adding `${PROJECT_SOURCE_DIR}/src` to a test include path

`configure/check_tests_public_api.py` runs at CMake configure time and fails the
build if a test includes engine internals.

GPU suites (`ZHLN_BUILD_GPU_TESTS`) judge public behaviour from
`CaptureScreenshotPPM` pixels — camera aim, PBR response, UI layout, and
ray-traced reflection colour vs PBR F0/roughness — not private systems.

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

`tests/core/TestOptional.cpp` follows the other core suites: a private
`OptionalTestSuite::Tests`, framework `Expect*` checks, and test-specific errors
returned as `std::expected<void, ZHLN::ErrorCode>`. `RunOptionalSuite()` is
registered in `RunCoreTests.cpp` and the source is listed in `CPU_Core`; there is
no separate test target, assertion macro, or `main()`.

Compile-time assertions pin the alias, reference/const semantics, rejected
borrowings, and constant evaluation. Runtime cases cover rebinding, conversion,
monadic operations, iteration and comparisons. The empty-`value()` exception case
is included only when exceptions are enabled: a fatal no-exceptions check must
not terminate the shared core-test process.

After building the existing core group, run it with:

```sh
ctest --test-dir build -R '^CPU_Core$' --output-on-failure
```
