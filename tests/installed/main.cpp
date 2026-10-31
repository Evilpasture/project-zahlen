// This standalone consumer owns its scene; it uses installed public APIs only.
#include <Zahlen/Zahlen.hpp>
#include "InstalledCubeSmoke.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    return RunInstalledCubeSmoke([](ZHLN::Engine& engine) {
        ZHLN::PrefabFactory::CreateBox(engine, {1.0f, 1.0f, 1.0f});
    }, argv[1]) ? 0 : 1;
}
