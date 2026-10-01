# Retired: UnlitTest.glb

The unlit suites no longer read a binary fixture. `tests/helpers/ChamferedBoxMesh.hpp`
generates the chamfered-box mesh and `tests/helpers/AuthoredUnlitFixture.hpp`
assembles the GLB bytes in memory; both carry the Khronos sample attribution
(CC-BY 4.0, (c) 2019 Analytical Graphics, Inc. -- Ed Mackey,
`Models/UnlitTest` at `cfbe2f9`) they were derived from.

# NegativeScaleTest.glb

Khronos glTF Sample Assets, `Models/NegativeScaleTest/glTF-Binary/NegativeScaleTest.glb` at
[`f36bfdab`](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/f36bfdabd1031c3cf6689a50570b8cdf3678b49c/Models/NegativeScaleTest).

© 2023 Analytical Graphics, Inc. — Ed Mackey. Licensed under
[Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/).
Unmodified. SHA-256: `ea8c41fd0630f03adcf7a5c06bf86d7b850fa510b6f07cc51eb1fccc205a50f2`.

The 62,568-byte fixture tests the real source material's `doubleSided: false`
on negatively scaled check/X geometry and `doubleSided: true` on spheres
shared between differently scaled nodes. The importer regression checks the
authored flags and shared geometry; the headless raster regression uses small
panels to check culling and two-sided normals under either winding. A full
golden-image comparison still uses the Fidelity Harness.
