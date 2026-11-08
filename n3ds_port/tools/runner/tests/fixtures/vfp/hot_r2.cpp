void hot_r2(float *xs, int n, float f) {
    for (int i = 0; i < n; i++) {
        int x = (int)f;
        xs[i] = (float)x;
    }
}

void hot_r2_hoisted(float *xs, int n, float f) {
    int x = (int)f;
    for (int i = 0; i < n; i++) {
        xs[i] = (float)x;
    }
}
