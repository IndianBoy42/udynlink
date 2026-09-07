#include "udynlink.h"
#include "mod_wasm2c_trap_module_data.h"
#include "wasm2c_runtime/wasm-rt.h"
#include "test_utils.h"
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
/* The baked wrappers call setjmp inside the module and the runtime calls
 * longjmp; both are host-resolved at load time. */
uintptr_t test_resolve_symbol(const char *name) {
    if (!strcmp(name, "setjmp"))
        return (uintptr_t)&setjmp;
    if (!strcmp(name, "longjmp"))
        return (uintptr_t)&longjmp;
    return 0;
}

typedef wasm_rt_trap_t (*last_trap_t)(void);

int test_qemu(void) {
    const char *exported_syms[] = {"bomb", "divzero", "spiral", "safe",
                                   "wasm_rt_last_trap", NULL};
    udynlink_module_t mod;
    udynlink_sym_t sym_lt, sym;
    last_trap_t last_trap;
    u32 r;
    int res = 0;

    memset(&mod, 0, sizeof(mod));

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (test_load_module(&mod, mod_wasm2c_trap_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (udynlink_lookup_symbol(&mod, "wasm_rt_last_trap", &sym_lt) == NULL)
            goto exit;
        last_trap = (last_trap_t)sym_lt.val;

        /* Trapped wrappers return the 0 sentinel; wasm_rt_last_trap()
         * says which trap fired.  The sentinel is ambiguous by design —
         * hosts wanting a clean error channel use --recoverable-traps. */
        if (udynlink_lookup_symbol(&mod, "bomb", &sym) == NULL)
            goto exit;
        UDYNLINK_PREPARE_CALL(&mod);
        r = ((u32 (*)(void))sym.val)();
        UDYNLINK_PREPARE_CALL(&mod);
        if (r != 0 || last_trap() != WASM_RT_TRAP_UNREACHABLE)
            goto exit;
        printf("wrapper recovered: unreachable\n");

        if (udynlink_lookup_symbol(&mod, "divzero", &sym) == NULL)
            goto exit;
        UDYNLINK_PREPARE_CALL(&mod);
        r = ((u32 (*)(u32, u32))sym.val)(1, 0);
        UDYNLINK_PREPARE_CALL(&mod);
        if (r != 0 || last_trap() != WASM_RT_TRAP_DIV_BY_ZERO)
            goto exit;
        printf("wrapper recovered: div by zero\n");

        /* Runaway recursion exhausts the wasm depth limit, not the native
         * stack; the wrapper unwinds it and reports exhaustion. */
        if (udynlink_lookup_symbol(&mod, "spiral", &sym) == NULL)
            goto exit;
        UDYNLINK_PREPARE_CALL(&mod);
        r = ((u32 (*)(u32))sym.val)(1000);
        UDYNLINK_PREPARE_CALL(&mod);
        if (r != 0 || last_trap() != WASM_RT_TRAP_EXHAUSTION)
            goto exit;
        printf("wrapper recovered: exhaustion\n");

        /* Well-behaved calls keep working after recovered traps. */
        if (udynlink_lookup_symbol(&mod, "safe", &sym) == NULL)
            goto exit;
        UDYNLINK_PREPARE_CALL(&mod);
        if (((u32 (*)(void))sym.val)() != 42)
            goto exit;
        printf("post-trap call OK: 42\n");

        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
