# Retired: UnlitTest.glb

The unlit suites no longer read a binary fixture. `TestUnlitMaterials.cpp`
uploads the authored chamfered solid from `tests/helpers/ChamferedBoxMesh.hpp`
straight through engine API calls, and `TestGLTFImport.cpp` checks the
extension against a tiny in-memory unlit-triangle document. The mesh helper
carries the Khronos sample attribution (CC-BY 4.0, (c) 2019 Analytical
Graphics, Inc. -- Ed Mackey, `Models/UnlitTest` at `cfbe2f9`) it was
derived from.

# Retired: NegativeScaleTest.glb

The negative-scale suites no longer read the 62,568-byte Khronos binary
(`Models/NegativeScaleTest` at `f36bfdab`). `TestGLTFImport.cpp` checks
sidedness flags, shared geometry and spawned parity against a tiny authored
in-memory document, and `TestNegativeScale.cpp` checks culling and two-sided
normals under either winding with C++ panels. A full golden-image comparison
still uses the Fidelity Harness.
