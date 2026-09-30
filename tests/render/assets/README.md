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
