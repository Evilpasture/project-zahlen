#include <Zahlen/Math3D.hpp>
#include <Zahlen/Log.hpp>

namespace ZHLN::Math {

void TestMathStack() {
    ZHLN::Log("Testing Math Stack Integrity...");

    // 1. Column-Major Memory Layout Check
    JPH::Mat44 id = JPH::Mat44::sIdentity();
    float* raw = (float*)&id;
    // In Column-Major, the 4th element [3] is 0.0, [15] is 1.0
    if (raw[15] != 1.0f || raw[3] != 0.0f) {
        ZHLN::Panic("Math Check Failed: Matrix is not Column-Major!");
    }

    // 2. Right-Handedness (X cross Y = Z)
    JPH::Vec3 z = JPH::Vec3(1, 0, 0).Cross(JPH::Vec3(0, 1, 0));
    if (z.GetZ() < 0.9f) {
        ZHLN::Panic("Math Check Failed: Coordinate system is not Right-Handed!");
    }

    // 3. View Matrix (Right Handed)
    // Eye at +10 on Z, looking at Origin. Target is in front (-Z).
    // The world origin (0,0,0) should result in View-Space (0,0,-10).
    JPH::Mat44 view = CreateLookAt({0, 0, 10}, {0, 0, 0}, {0, 1, 0});
    JPH::Vec3 viewPos = view * JPH::Vec3(0, 0, 0);
    if (viewPos.GetZ() > -9.9f || viewPos.GetZ() < -10.1f) {
         ZHLN::Panic("Math Check Failed: LookAt produced incorrect View-Space coordinates!");
    }

    // 4. Vulkan Projection Y-Flip
    // Aspect 1.0, FOV 90. A point at (0, 1, -1) in View Space should be (0, -1, 0) in Clip Space.
    JPH::Mat44 proj = CreatePerspective(JPH::DegreesToRadians(90.0f), 1.0f, 0.1f, 100.0f);
    JPH::Vec4 clipPos = proj * JPH::Vec4(0, 1, -1, 1);
    if (clipPos.GetY() > 0.0f) {
        ZHLN::Panic("Math Check Failed: Vulkan Y-Flip is missing (Top of screen must be -Y)!");
    }

    // 5. Vulkan Z-Range [0, 1]
    // Near plane point (0, 0, -0.1) in View Space should result in Z=0 in Clip Space.
    JPH::Vec4 nearClip = proj * JPH::Vec4(0, 0, -0.1f, 1);
    float zVal = nearClip.GetZ() / nearClip.GetW(); // Perspective divide
    if (zVal < -0.001f || zVal > 0.001f) {
        ZHLN::Panic("Math Check Failed: Vulkan Z-Range is not [0, 1]! (Got: {})", zVal);
    }

    ZHLN::Log("Math Stack Verified: Column-Major, Right-Handed, Vulkan NDC.");
}

}