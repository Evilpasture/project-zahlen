// src/engine/Resources.c
#include <stdint.h>

// Provide the paths relative to this file
// Note: We use 'const' so these are stored in the .rodata (read-only) section of the EXE
const unsigned char ZHLN_Resource_BasicVertSpv[] = {
#embed "../../resources/shaders/basic.vert.spv"
};
const unsigned int ZHLN_Resource_BasicVertSpv_Len = sizeof(ZHLN_Resource_BasicVertSpv);

const unsigned char ZHLN_Resource_BasicFragSpv[] = {
#embed "../../resources/shaders/basic.frag.spv"
};
const unsigned int ZHLN_Resource_BasicFragSpv_Len = sizeof(ZHLN_Resource_BasicFragSpv);

const char ZHLN_Resource_BasicMetal[] = {
#embed "../../resources/shaders/basic.metal"
, 0 // Null terminator for C-string usage
};
const unsigned int ZHLN_Resource_BasicMetal_Len = sizeof(ZHLN_Resource_BasicMetal);