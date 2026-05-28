int hello(int arg) {
    return arg + 42;
}
static int counter = 0;
int get_counter(void) {
    counter += 1;
    return counter;
}
void set_counter(int val) {
    counter = val;
}
