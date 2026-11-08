void hot_r3(float *xs, int n, float f) {
    for (int i = 0; i < n; i++) {
        if (f > 0.0f) {
            xs[i] = f;
        }
    }
}

void hot_r3_int_pred(float *xs, int n, int pred, float f) {
    for (int i = 0; i < n; i++) {
        if (pred) {
            xs[i] = f;
        }
    }
}
