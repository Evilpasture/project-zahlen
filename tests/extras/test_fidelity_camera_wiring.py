#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin the sample's orbit wiring without requiring a GPU or Sponza's assets.

Khronos deliberately authors radius=0 for Sponza (theta=90, phi=90) and
MetalRoughSpheresNoTextures. An eye-to-target lookAt is undefined there; the
orbit angles must still determine a forward direction. Source-level coverage
is appropriate because SetFidelityCamera belongs to this executable's main()
and is not a reusable engine API.
"""

import math
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HARNESS = (ROOT / "samples/FidelityHarness.cpp").read_text()
LIST_TOOL = (ROOT / "tools/fidelity/main.cpp").read_text()


class FidelityCameraWiringTest(unittest.TestCase):
    def test_zero_radius_uses_orbit_direction_not_eye_to_target(self) -> None:
        camera = HARNESS.split("void SetFidelityCamera(", 1)[1].split("// CONFORMANCE SETTINGS", 1)[0]
        self.assertIn("std::sin(phiRad) * std::sin(thetaRad)", camera)
        self.assertIn("std::cos(phiRad)", camera)
        self.assertIn("std::sin(phiRad) * std::cos(thetaRad)", camera)
        self.assertRegex(camera, r"camera\.position\s*=\s*target\s*\+\s*\(direction\s*\*\s*scenario\.orbit\.radius\)")
        self.assertRegex(camera, r"const JPH::Vec3 forward\s*=\s*-direction\s*;")
        self.assertNotRegex(camera, r"\(target\s*-\s*camera\.position\)\.Normalized\(")
        self.assertRegex(camera, r"camera\.yaw\s*=\s*JPH::RadiansToDegrees\(std::atan2\(forward\.GetZ\(\),\s*forward\.GetX\(\)\)\)")
        self.assertIn("std::isfinite(camera.yaw)", HARNESS)
        self.assertIn("std::isfinite(camera.pitch)", HARNESS)

        # The upstream Sponza scenario has target=(0,1,0) and
        # theta=90, phi=90 (default), radius=0. Its eye is exactly the target
        # but its view faces -X, not an undefined zero-length lookAt vector.
        theta, phi, radius = math.radians(90), math.radians(90), 0
        direction = (math.sin(phi) * math.sin(theta), math.cos(phi), math.sin(phi) * math.cos(theta))
        eye = tuple(t + radius * d for t, d in zip((0, 1, 0), direction))
        forward = tuple(-d for d in direction)
        self.assertEqual(eye, (0, 1, 0))
        self.assertAlmostEqual(forward[0], -1.0)
        self.assertAlmostEqual(abs(math.degrees(math.atan2(forward[2], forward[0]))), 180.0)
        self.assertTrue(all(math.isfinite(component) for component in forward))

    def test_upstream_vertical_fov_spelling_is_accepted(self) -> None:
        # config-reader.ts calls it verticalFoV; the original local typo was
        # verticalFov. Keep reading both without changing the runner's JSON.
        self.assertIn('GetFloat(*raw, "verticalFoV", GetFloat(*raw, "verticalFov", 45.0f))', LIST_TOOL)
        self.assertIn('GetFloat(root, "verticalFoV", GetFloat(root, "verticalFov", 45.0f))', HARNESS)


if __name__ == "__main__":
    unittest.main()
