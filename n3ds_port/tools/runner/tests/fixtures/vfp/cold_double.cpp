double init_tables(double seed) {
    double acc = 0.5;
    for (int i = 0; i < 8; i++) {
        acc += seed;
    }
    return acc;
}
