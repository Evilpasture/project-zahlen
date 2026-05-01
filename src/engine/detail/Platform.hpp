#pragma once

#if __cplusplus < 202302L
#error Project-Zahlen requires a compiler that supports C++23.
#endif

#ifdef _MSC_VER
#define ZHLN_RESTRICT __restrict
#else
#define ZHLN_RESTRICT __restrict__
#endif
