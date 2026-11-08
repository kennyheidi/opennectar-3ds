#pragma once
namespace ts {
inline float fastSin(float rad) {
    const float idx = rad * 162.974f;
    int   i = (int)idx;
    float f = idx - (float)i;
    if (f < 0.0f) { --i; f += 1.0f; }
    return f;
}
inline float fastCos(float rad) { return fastSin(rad + 1.570796f); }
inline float fastAtan2(float y, float x) {
    const float ax = x < 0.0f ? -x : x;
    const float ay = y < 0.0f ? -y : y;
    if (ax == 0.0f && ay == 0.0f) return 0.0f;
    const float z = ay / ax;
    return z;
}
}
