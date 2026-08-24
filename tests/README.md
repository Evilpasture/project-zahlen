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

`tools/check_tests_public_api.py` runs at CMake configure time and fails the
build if a test includes engine internals.

GPU suites (`ZHLN_BUILD_GPU_TESTS`) judge public behaviour from
`CaptureScreenshotPPM` pixels — camera aim, PBR response, UI layout, and
ray-traced reflection colour vs PBR F0/roughness — not private systems.
