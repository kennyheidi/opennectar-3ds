float hot_r7_inner(float *xs, int n, float a, float b) {
    for (int i = 0; i < n; i++) {
        xs[i] = a / b;
    }
    return xs[0];
}

float hot_r7_outer(float *xs, int n, float a, float b) {
    float r = a / b;
    for (int i = 0; i < n; i++) {
        xs[i] = r;
    }
    return r;
}
