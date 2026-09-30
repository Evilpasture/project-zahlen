# UnlitTest.glb

Khronos glTF Sample Assets, `Models/UnlitTest/glTF-Binary/UnlitTest.glb` at
[`cfbe2f9`](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/cfbe2f9ac259490855940ff85feb5b4b02386046/Models/UnlitTest).

© 2019 Analytical Graphics, Inc. — Ed Mackey. Licensed under
[Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/).
Unmodified. SHA-256: `e07b68c6fd9fbf73d372c610a681b89d6b013bdd1a506e46a411df82210a2481`.

The 3,992-byte fixture tests whether `KHR_materials_unlit` survives the real
import path and whether its two multi-normal objects render without face shading.
No golden image is checked in: the headless test compares face colors and their
invariance to lighting; use the Fidelity Harness for a full viewer comparison.

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
