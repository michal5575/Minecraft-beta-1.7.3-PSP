#pragma once
#include "config.h"

namespace mh {

extern float SIN_TABLE[65536];
void init();

inline float sin(float v) { return SIN_TABLE[(int)(v * 10430.378f) & 65535]; }
inline float cos(float v) { return SIN_TABLE[(int)(v * 10430.378f + 16384.0f) & 65535]; }

inline int floorr(real v) {
    int i = (int)v;
    return v < (real)i ? i - 1 : i;
}

inline int floorf_(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

constexpr float PI = 3.1415927f;

}  // namespace mh
