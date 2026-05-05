// src/engine/Resources.c
const unsigned char ZHLN_Resource_BasicVertSpv[] = {
#embed "../../resources/shaders/basic.hlsl.VSMain.spv"
};
const unsigned int ZHLN_Resource_BasicVertSpv_Len = sizeof(ZHLN_Resource_BasicVertSpv);

const unsigned char ZHLN_Resource_BasicFragSpv[] = {
#embed "../../resources/shaders/basic.hlsl.PSMain.spv"
};
const unsigned int ZHLN_Resource_BasicFragSpv_Len = sizeof(ZHLN_Resource_BasicFragSpv);