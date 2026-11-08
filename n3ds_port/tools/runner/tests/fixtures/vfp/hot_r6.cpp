float sinf(float);
float fastSin(float);

float hot_r6_libm(float x) {
    float acc = 0.0f;
    for (int i = 0; i < 8; i++) {
        acc += sinf(x);
    }
    return acc;
}

float hot_r6_lut(float x) {
    float acc = 0.0f;
    for (int i = 0; i < 8; i++) {
        acc += fastSin(x);
    }
    return acc;
}
