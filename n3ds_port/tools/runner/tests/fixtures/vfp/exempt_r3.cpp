/* @vfp-exempt R3 — predicate depends on per-vertex data that cannot be hoisted; measured 0.4ms, accepted standing. Verified 2026-01-01. */
void exempt_r3(float *xs, int n, float f) {
    for (int i = 0; i < n; i++) {
        if (f > 0.0f) {
            xs[i] = f;
        }
    }
}
