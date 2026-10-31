#pragma once
namespace ZHLN { class Engine; }
// Standalone consumer's lifecycle/capture helper. All dependencies are installed.
[[nodiscard]] bool RunInstalledCubeSmoke(void (*createScene)(ZHLN::Engine&), const char* ppmPath);
