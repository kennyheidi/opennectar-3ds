void hot_r5_for_float(float *xs, int n) {
    for (float t = 0; t < n; t++) {
        xs[0] = t;
    }
}

void hot_r5_index(float *arr, float u) {
    for (int i = 0; i < 4; i++) {
        arr[(int)u] = 1.0f;
    }
}
