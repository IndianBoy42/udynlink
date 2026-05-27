int add(int a, int b) {
    return a + b;
}

int get_magic(void) {
    return 0xCAFE;
}

int test(void) {
    return (add(2, 3) == 5 && add(-1, 1) == 0 && get_magic() == 0xCAFE);
}
