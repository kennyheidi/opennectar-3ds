float hot_r8(float scale, int q16_fx) {
    float acc = 0.0f;
    for (int i = 0; i < 8; i++) {
        acc += q16_fx * scale;
    }
    return acc;
}
