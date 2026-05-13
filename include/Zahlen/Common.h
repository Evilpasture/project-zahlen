#pragma once

#if defined(_WIN32)
#define ZHLN_EXPORT __declspec(dllexport)
#else
#define ZHLN_EXPORT [[gnu::visibility("default"), gnu::used]]
#endif

// Forward declaration of the engine handle used across all headers
struct ZHLN_Engine;