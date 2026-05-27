extern int mod_b_get_value(void);

int mod_a_get_value(void) {
    return 1;
}

int test(void) {
    return 42 + mod_b_get_value();
}
