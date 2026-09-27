#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard the sample composition root: spawning a character does not install its controller.

This is a source-level check because the sample's own main() owns the install
order; testing the character controller in isolation cannot detect an omitted
call in that executable.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ProceduralSampleWiringTest(unittest.TestCase):
    def test_controller_installed_before_scene_and_player(self) -> None:
        source = (ROOT / "samples/ProceduralAnimationSample.cpp").read_text()
        self.assertIn("#include <CharacterController/CharacterController.hpp>", source)
        main = source.split("auto main(", 1)[1]

        def statement_position(statement: str) -> int:
            match = re.search(r"^\s*" + re.escape(statement) + r"\s*$", main, re.MULTILINE)
            self.assertIsNotNone(match, f"missing statement: {statement}")
            return match.start()

        controller = statement_position("ZHLN::Character::Install(*engine);")
        camera = statement_position("ZHLN::CameraRig::Install(*engine);")
        scene = statement_position("engine->InitializeDefaultScene();")
        player_spawn = re.search(r"^\s*const ZHLN::Entity player\s*=\s*ZHLN::Locomotion::SpawnCharacter\(", main, re.MULTILINE)
        self.assertIsNotNone(player_spawn, "sample no longer spawns a character")
        self.assertLess(controller, camera)
        self.assertLess(camera, scene)
        self.assertLess(scene, player_spawn.start())

    def test_sample_links_the_controller_it_installs(self) -> None:
        cmake = (ROOT / "samples/CMakeLists.txt").read_text()
        match = re.search(r"^set\(ZHLN_SAMPLE_EXTRAS_ProceduralAnimationSample\s+([^)]*)\)", cmake, re.MULTILINE)
        self.assertIsNotNone(match, "missing sample extras dependency list")
        self.assertEqual(match.group(1).split().count("zahlen_character_controller"), 1)


if __name__ == "__main__":
    unittest.main()
