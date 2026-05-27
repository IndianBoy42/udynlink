#include "hello.h"

/* udynlink module entry point: instantiate and test linear memory data segment */
int test(void) {
    w2c_hello instance;
    wasm2c_hello_instantiate(&instance);

    /* Check get_len returns 12 ("hello, world") */
    if (w2c_hello_get_len(&instance) != 12)
        return 0;

    /* Verify a few characters from the data segment */
    if (w2c_hello_get_char(&instance, 0) != 'h')
        return 0;
    if (w2c_hello_get_char(&instance, 5) != ',')
        return 0;
    if (w2c_hello_get_char(&instance, 11) != 'd')
        return 0;

    return 1;
}
