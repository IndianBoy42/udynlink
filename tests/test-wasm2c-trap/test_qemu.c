#include "udynlink.h"
#include "mod_wasm2c_trap_module_data.h"
#include "wasm2c_runtime/wasm-rt.h"
#include "test_utils.h"
#include <stdio.h>
#include <setjmp.h>
#include <string.h>

/* Recoverable-trap module: the runtime references longjmp and the host
 * provides it — resolved at load time like any other import. */
uintptr_t test_resolve_symbol(const char *name) {
    if (!strcmp(name, "longjmp"))
        return (uintptr_t)&longjmp;
    return 0;
}

typedef void (*set_recovery_t)(jmp_buf *);
typedef wasm_rt_trap_t (*last_trap_t)(void);

/* Arm the one-shot recovery point, call into the module, and expect the
 * trap to unwind back here with the expected code. */
#define EXPECT_TRAP(set_recovery, call_expr, expected_code, msg)              \
    do {                                                                      \
        if (setjmp(jb) == 0) {                                                \
            UDYNLINK_PREPARE_CALL(&mod);                                      \
            set_recovery(&jb);                                                \
            UDYNLINK_PREPARE_CALL(&mod);                                      \
            call_expr;                                                        \
            printf("FAIL: module call returned without trapping\n");          \
            goto exit;                                                        \
        }                                                                     \
        UDYNLINK_PREPARE_CALL(&mod);                                          \
        if (last_trap() != (expected_code))                                   \
            goto exit;                                                        \
        printf(msg "\n");                                                     \
    } while (0)

int test_qemu(void) {
    const char *exported_syms[] = {"bomb", "divzero", "spiral", "safe",
                                   "wasm_rt_set_recovery", "wasm_rt_last_trap", NULL};
    udynlink_module_t mod;
    udynlink_sym_t sym_sr, sym_lt, sym;
    jmp_buf jb;
    set_recovery_t set_recovery;
    last_trap_t last_trap;
    int res = 0;

    memset(&mod, 0, sizeof(mod));

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (test_load_module(&mod, mod_wasm2c_trap_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (udynlink_lookup_symbol(&mod, "wasm_rt_set_recovery", &sym_sr) == NULL ||
            udynlink_lookup_symbol(&mod, "wasm_rt_last_trap", &sym_lt) == NULL)
            goto exit;
        set_recovery = (set_recovery_t)sym_sr.val;
        last_trap = (last_trap_t)sym_lt.val;

        /* The module's own unreachable. */
        if (udynlink_lookup_symbol(&mod, "bomb", &sym) == NULL)
            goto exit;
        EXPECT_TRAP(set_recovery, ((u32 (*)(void))sym.val)(),
                    WASM_RT_TRAP_UNREACHABLE, "trap recovered: unreachable");

        /* Integer division by zero (explicit wasm2c check, not SIGFPE). */
        if (udynlink_lookup_symbol(&mod, "divzero", &sym) == NULL)
            goto exit;
        EXPECT_TRAP(set_recovery, ((u32 (*)(u32, u32))sym.val)(1, 0),
                    WASM_RT_TRAP_DIV_BY_ZERO, "trap recovered: div by zero");

        /* Runaway recursion: --stack-depth-limit=64 turns it into a
         * recoverable WASM_RT_TRAP_EXHAUSTION instead of a native stack
         * overflow. */
        if (udynlink_lookup_symbol(&mod, "spiral", &sym) == NULL)
            goto exit;
        EXPECT_TRAP(set_recovery, ((u32 (*)(u32))sym.val)(1000),
                    WASM_RT_TRAP_EXHAUSTION, "trap recovered: exhaustion");

        /* The device keeps running: a well-behaved call works after three
         * recovered traps (proves the depth counter and recovery state are
         * properly reset). */
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
