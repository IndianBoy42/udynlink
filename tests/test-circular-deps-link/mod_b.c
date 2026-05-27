extern int mod_a_get_value(void);

int mod_b_get_value(void) {
    return 2;
}

int test(void) {
    return 10 + mod_a_get_value();
}
