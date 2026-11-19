#pragma once
namespace ZHLN::COM { struct IEngine; }
// Standalone consumer's lifecycle/capture helper. All dependencies are installed.
[[nodiscard]] bool RunInstalledCubeSmoke(void (*createScene)(ZHLN::COM::IEngine&), const char* ppmPath);
