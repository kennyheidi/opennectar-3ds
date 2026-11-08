float hot_r4_bad(float x) {
    for (int i = 0; i < 8; i++) {
        x += 0.5;
    }
    return x;
}

float hot_r4_ok(float x) {
    for (int i = 0; i < 8; i++) {
        x += 0.5f;
    }
    return x;
}

double hot_r4_double(double x) {
    for (int i = 0; i < 8; i++) {
        x += 1.0;
    }
    return x;
}
