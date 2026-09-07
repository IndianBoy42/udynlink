#include "udynlink.h"
#include "mod_wasm2c_hello_module_data.h"
#include "test_utils.h"
#include <stdio.h>

/* Initial member of wasm_rt_memory_t (wasm-rt.h).  The host never includes
 * the wasm runtime; it only inspects the pointer returned by the exported
 * 'memory' wrapper, so reading through the first member is ABI-safe. */
typedef struct {
    unsigned char *data;
} wasm_mem_probe_t;

int test_qemu(void) {
    const char *exported_syms[] = {"memory", "get_len", "get_char", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_wasm2c_hello_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        /* Static linear memory (1 page of .bss) + the data segment must be
         * visible in the module's RAM footprint. */
        CHECK_RAM_SIZE(&mod, 65536);
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        {
            udynlink_sym_t sym;

            /* get_len: scalar export reading the data segment */
            if (udynlink_lookup_symbol(&mod, "get_len", &sym) == NULL)
                goto exit;
            UDYNLINK_PREPARE_CALL(&mod);
            unsigned int (*get_len)(void) = (unsigned int (*)(void))sym.val;
            unsigned int len = get_len();
            printf("hello: get_len() = %u\n", len);
            if (len != 12)
                goto exit;

            /* get_char: bounds-checked loads from linear memory */
            if (udynlink_lookup_symbol(&mod, "get_char", &sym) == NULL)
                goto exit;
            UDYNLINK_PREPARE_CALL(&mod);
            unsigned int (*get_char)(unsigned int) =
                (unsigned int (*)(unsigned int))sym.val;
            unsigned char c0 = get_char(0);
            UDYNLINK_PREPARE_CALL(&mod);
            unsigned char c11 = get_char(11);
            printf("hello: data[0] = '%c'\n", c0);
            if (c0 != 'h' || c11 != 'd')
                goto exit;

            /* memory: pointer-returning export (wasm_rt_memory_t*); the
             * data segment must be initialized in the static buffer. */
            if (udynlink_lookup_symbol(&mod, "memory", &sym) == NULL)
                goto exit;
            UDYNLINK_PREPARE_CALL(&mod);
            void *(*memory)(void) = (void *(*)(void))sym.val;
            wasm_mem_probe_t *mem = (wasm_mem_probe_t *)memory();
            if (mem == NULL || mem->data == NULL ||
                mem->data[0] != 'h' || mem->data[11] != 'd') {
                printf("hello: 'memory' wrapper returned bad data\n");
                goto exit;
            }
        }
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
