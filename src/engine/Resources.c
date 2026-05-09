// src/engine/Resources.c

const unsigned char ZHLN_Resource_BasicVertSpv[] = {
// No path needed! The compiler finds it in the Build Directory we provided to CMake.
#embed "basic.hlsl.VSMain.spv"
};
const unsigned int ZHLN_Resource_BasicVertSpv_Len = sizeof(ZHLN_Resource_BasicVertSpv);

const unsigned char ZHLN_Resource_BasicFragSpv[] = {
#embed "basic.hlsl.PSMain.spv"
};
const unsigned int ZHLN_Resource_BasicFragSpv_Len = sizeof(ZHLN_Resource_BasicFragSpv);
