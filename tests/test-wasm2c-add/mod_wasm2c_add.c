#include "add.h"

/* udynlink module entry point: instantiate wasm module and call exported add */
int test(void) {
    w2c_add instance;
    wasm2c_add_instantiate(&instance);
    u32 result = w2c_add_add(&instance, 30, 70);
    return (result == 100) ? 1 : 0;
}
