#include "udynlink.h"
#include "udynlink_cpp_abi.h"   /* host-side weak stubs + resolver */
#include "mod_virt_dtor_module_data.h"
#include <stdio.h>
#include <string.h>

/* The C++ ABI symbols (`_ZdlPvj`, `__cxa_pure_virtual`, ...) referenced by
 * the module's deleting destructor and abstract-base vtable are bound here:
 * `udynlink_cpp_resolve_abi_symbol` returns the addresses of the weak
 * stubs defined in udynlink_cpp_abi.h. Without this, the loader rejects the
 * module with UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL on its first (and every)
 * load mode.
 *
 * A host that links a real C++ runtime does not need this hook — the strong
 * definitions from libstdc++/picolibc win the link and `&operator delete`
 * below points at them transparently. This path is for C-only hosts. */

extern "C" uintptr_t test_resolve_symbol(const char *name) {
    return udynlink_cpp_resolve_abi_symbol(name);
}

extern "C" int test_qemu(void) {
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        udynlink_error_t err = udynlink_load_module(&mod, mod_virt_dtor_module_data, NULL, 0, (udynlink_load_mode_t)i);
        if (err) {
            printf("load failed mode %d: %s\n", i, udynlink_error_msg(&err));
            return 0;
        }

        udynlink_cpp_init(&mod);
        printf("loaded mode %d\n", i);

        /* Inline run_test_func — test_utils.h has no extern "C" guard and
         * calling it from C++ would trip a name-mangling relocation error. */
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
            printf("'test' symbol not found.\n");
            goto exit;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        UDYNLINK_PREPARE_CALL(&mod);
        if (!p_func()) {
            printf("test() returned 0\n");
            goto exit;
        }

        printf("test ok mode %d\n", i);
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
