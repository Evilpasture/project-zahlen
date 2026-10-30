#include <Zahlen/Zahlen.hpp>

using namespace ZHLN;

int main() {
    return App().OnStart([](Engine& engine) { PrefabFactory::CreateBox(engine, JPH::Vec3(1.0f, 1.0f, 1.0f)); }).Run() ? 0 : 1;
}
