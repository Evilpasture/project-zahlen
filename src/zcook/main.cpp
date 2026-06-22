// src/zcook/main.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Cook.hpp"
#include "threading/TaskSystem.hpp"
#include "threading/Thread.hpp"

#include <cstdio>
#include <print>
#include <string_view>

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::println(
			stderr,
			"[zcook] ERROR: Missing command-line action. Usage:\n"
			"  zcook <command> [options]\n\n"
			"Available Commands:\n"
			"  mesh  - Deduplicates standard uncompressed floats into static mesh arrays.\n"
			"  tex   - Copy and format static assets into standard asset structures.\n"
			"  glb   - Compiles internal scenes and layouts into standard glTF GLB containers.\n"
			"  anim  - Compiles animations into binary format for runtime playback.\n"
			"  pak   - Compact files listed in a manifest file into single custom pak indexes.");
		return 1;
	}

	std::string_view cmd = argv[1];
	if (cmd != "mesh" && cmd != "tex" && cmd != "glb" && cmd != "anim" && cmd != "pak") {
		std::println(stderr, "[zcook] ERROR: Unsupported action subcommand '{}'.", cmd);
		return 1;
	}

	ZHLN::Fiber::InitMainThread();
	ZHLN::TaskSystem::Init(0);

	int result = 0;
	if (cmd == "mesh")
		result = ZHLN::CookMesh(argc - 2, argv + 2);
	else if (cmd == "anim")
		result = ZHLN::CookAnimation(argc - 2, argv + 2);
	else if (cmd == "tex")
		result = ZHLN::CookTexture(argc - 2, argv + 2);
	else if (cmd == "glb")
		result = ZHLN::CookGLB(argc - 2, argv + 2);
	else if (cmd == "pak")
		result = ZHLN::PackArchive(argc - 2, argv + 2);

	ZHLN::TaskSystem::Shutdown();

	return result;
}
