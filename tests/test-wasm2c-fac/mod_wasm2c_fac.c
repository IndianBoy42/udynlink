#include "fac.h"

/* udynlink module entry point: instantiate and test recursive factorial */
int test(void) {
    w2c_fac instance;
    wasm2c_fac_instantiate(&instance);
    u32 result = w2c_fac_fac(&instance, 5);
    return (result == 120) ? 1 : 0;
}
