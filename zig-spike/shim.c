// C shim that calls Zig functions
extern int hello(int arg);
extern int get_counter(void);
extern void set_counter(int val);

int c_hello(int arg) {
    return hello(arg);
}
int c_get_counter(void) {
    return get_counter();
}
void c_set_counter(int val) {
    set_counter(val);
}
