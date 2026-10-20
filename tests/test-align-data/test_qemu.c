/* Regression: a SECTIONED image whose `.data` needs 8-byte alignment.
 *
 * Depending on -O codegen the linker either places .data right after .text or
 * leaves a `.text`->`.data` alignment gap. The encoder must fold any such gap
 * into the CODE payload, keeping `.data`'s payload at its own VA; pre-pending
 * it to `.data` instead shifts every data relocation by the gap, which the
 * loader rejects (BAD_RELOCATION_TABLE) or silently mis-patches. This test
 * loads the real image in all three modes and evaluates the relocated `.data`
 * pointer through the module's own code, so a misplaced payload shows up as a
 * wrong value rather than as a load error only.
 *
 * Module entry points are called with UDYNLINK_CALL, which sets/restores r9
 * (a plain function-pointer call leaves the host's r9 in place and faults). */
#include "udynlink.h"
#include "udynlink_call.h"
#include "mod_align_data_module_data.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>

#define EXPECTED_MARKER 0x5A5A1234u

int test_qemu(void) {
    udynlink_module_t mod;
    int ok = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        const char *exported[] = {"test", "align_add", "check_pair", "get_marker",
                                  "call_tbl", NULL};
        udynlink_func_t h_pair, h_marker, h_add, h_tbl;
        udynlink_error_t err;

        err = test_load_module(&mod, mod_align_data_module_data, NULL, 0,
                               (udynlink_load_mode_t)i);
        if (err != UDYNLINK_OK) {
            printf("align-data: load failed in mode %d (%s)\n", i,
                   udynlink_error_msg((udynlink_error_t *)(intptr_t)err));
            goto exit;
        }
        if (!check_exported_symbols(&mod, exported))
            goto exit;

        if (udynlink_resolve_func(&mod, "check_pair", &h_pair) != UDYNLINK_OK ||
            udynlink_resolve_func(&mod, "get_marker", &h_marker) != UDYNLINK_OK ||
            udynlink_resolve_func(&mod, "align_add", &h_add) != UDYNLINK_OK ||
            udynlink_resolve_func(&mod, "call_tbl", &h_tbl) != UDYNLINK_OK) {
            printf("align-data: resolve failed in mode %d\n", i);
            goto exit;
        }

        /* Evaluated by the module: the ABS32 site inside .data plus the
         * 8-byte-aligned table. */
        if (UDYNLINK_CALL(&h_pair, int, ()) != 1) {
            printf("align-data: relocated .data pointer / aligned table wrong in mode %d\n", i);
            goto exit;
        }

        /* Calling THROUGH the relocated function-pointer table in .data: a
         * relocation applied one word off makes this jump to garbage. */
        if (UDYNLINK_CALL(&h_tbl, int, (0)) != 11 ||
            UDYNLINK_CALL(&h_tbl, int, (1)) != 22) {
            printf("align-data: relocated .data function-pointer table wrong in mode %d\n", i);
            goto exit;
        }

        /* Tagged data reachable, initializer intact. */
        if (UDYNLINK_CALL(&h_marker, uint32_t, ()) != EXPECTED_MARKER) {
            printf("align-data: tagged section marker wrong in mode %d\n", i);
            goto exit;
        }
        if (UDYNLINK_CALL(&h_add, int, (2, 3)) != 5) {
            printf("align-data: align_add returned a wrong value in mode %d\n", i);
            goto exit;
        }

        printf("align-data: loads and relocates in every load mode\n");
        printf("align-data: .data pointer and 8-byte-aligned table intact\n");
        printf("align-data: relocated .data function-pointer table OK\n");
        printf("align-data: tagged section reachable\n");
        udynlink_unload_module(&mod);
    }
    ok = 1;
exit:
    udynlink_unload_module(&mod);
    return ok;
}
