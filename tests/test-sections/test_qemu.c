/* Multi-region placement test: a sectioned module (mkmodule --section) whose
 * tagged sections must land in the host's secondary pool — the board's real
 * second RAM region (MPS2: BRAM @ 0x20000000, F429: CCMRAM @ 0x10000000,
 * wired to the .altpool section of the shared firmware). Verifies the
 * section table the loader reports, that both tagged sections (data and
 * code) resolve into the pool with the declared alignment, that data
 * round-trips between host and module through the pool memory, that the
 * loader rejects misaligned/refused placements with the new error codes,
 * and that unload frees exactly what load allocated. */
#include "udynlink.h"
#include "mod_sections_module_data.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Secondary-pool introspection from the shared firmware (src/main.c). */
extern size_t test_altpool_alloc_count(void);
extern size_t test_altpool_free_count(void);
extern const void *test_altpool_base(void);
extern size_t test_altpool_size(void);
extern void test_altpool_misalign_next(void);
extern void test_altpool_fail_next(void);

static int in_secondary_pool(const volatile void *p) {
    const volatile uint8_t *base = (const volatile uint8_t *)test_altpool_base();
    return ((const volatile uint8_t *)p >= base) &&
           ((const volatile uint8_t *)p < base + test_altpool_size());
}

static int test_mode(udynlink_load_mode_t mode) {
    udynlink_module_t mod;
    udynlink_sym_t sym;
    udynlink_section_info_t info;
    size_t alt_idx = (size_t)-1, altcode_idx = (size_t)-1;
    int res = 0;
    size_t allocs0 = test_altpool_alloc_count();
    size_t frees0 = test_altpool_free_count();

    if (test_load_module(&mod, mod_sections_module_data, NULL, 0, mode)) {
        printf("Load failed for mode %d\n", (int)mode);
        return 0;
    }

    /* Section table: the three main sections occupy indices 0/1/2 (lowest
     * VAs, per the format contract), then the two tagged ones. */
    size_t nsec = udynlink_get_section_count(mod.p_header);
    if (nsec != 5) {
        printf("section count %u != 5\n", (unsigned)nsec);
        goto exit;
    }
    for (size_t i = 0; i < nsec; i++) {
        if (udynlink_get_section_info(mod.p_header, i, &info) != UDYNLINK_OK) {
            printf("get_section_info(%u) failed\n", (unsigned)i);
            goto exit;
        }
        if (i < 3) {
            uint8_t want = (i == 0) ? UDYNLINK_SEC_CLASS_CODE
                         : (i == 1) ? UDYNLINK_SEC_CLASS_DATA
                                    : UDYNLINK_SEC_CLASS_BSS;
            if (info.sec_class != want) {
                printf("main section %u wrong class %u\n", (unsigned)i, info.sec_class);
                goto exit;
            }
            continue;
        }
        if (info.name == NULL) {
            printf("tagged section %u unnamed\n", (unsigned)i);
            goto exit;
        }
        if (!strcmp(info.name, "alt"))
            alt_idx = i;
        else if (!strcmp(info.name, "altcode"))
            altcode_idx = i;
        else {
            printf("unexpected section '%s'\n", info.name);
            goto exit;
        }
    }
    if (alt_idx == (size_t)-1 || altcode_idx == (size_t)-1) {
        printf("tagged sections missing from table\n");
        goto exit;
    }

    if (udynlink_get_section_info(mod.p_header, alt_idx, &info) != UDYNLINK_OK ||
        info.sec_class != UDYNLINK_SEC_CLASS_DATA || info.align != 32u ||
        (info.size % 4u) != 0 || info.size < 16u * sizeof(uint32_t) || info.flags != 0) {
        printf("alt section info mismatch\n");
        goto exit;
    }
    printf("sections: table reports alt (data, align 32)\n");

    if (udynlink_get_section_info(mod.p_header, altcode_idx, &info) != UDYNLINK_OK ||
        info.sec_class != UDYNLINK_SEC_CLASS_CODE || info.size == 0 || info.flags != 0) {
        printf("altcode section info mismatch\n");
        goto exit;
    }

    {
        void *alt_base = udynlink_get_section_base(&mod, alt_idx);
        void *altcode_base = udynlink_get_section_base(&mod, altcode_idx);
        if (alt_base == NULL || !in_secondary_pool(alt_base)) {
            printf("alt base %p not in secondary pool\n", alt_base);
            goto exit;
        }
        if ((uintptr_t)alt_base & 31u) {
            printf("alt base %p not 32-aligned\n", alt_base);
            goto exit;
        }
        printf("sections: alt in secondary pool, 32-aligned\n");
        if (altcode_base == NULL || !in_secondary_pool(altcode_base)) {
            printf("altcode base %p not in secondary pool\n", altcode_base);
            goto exit;
        }
        printf("sections: altcode in secondary pool\n");

        /* The host looks the tagged buffer up by symbol: the value must be
         * the runtime address inside the pool, 32-aligned like its section. */
        if (udynlink_lookup_symbol(&mod, "alt_buf", &sym) == NULL) {
            printf("alt_buf symbol not found\n");
            goto exit;
        }
        volatile uint32_t *buf =
            (volatile uint32_t *)(uintptr_t)udynlink_get_symbol_value(&mod, "alt_buf");
        if (!in_secondary_pool(buf) || ((uintptr_t)buf & 31u)) {
            printf("alt_buf runtime address %p outside pool/misaligned\n", buf);
            goto exit;
        }

        /* Module -> host: module code (altcode region) fills the tagged
         * data buffer (alt region); the host reads the pool memory. */
        int (*add)(int) = (int (*)(int))udynlink_get_symbol_value(&mod, "alt_add");
        UDYNLINK_PREPARE_CALL(&mod);
        if (add(42) != 42) {
            printf("alt_add(42) returned wrong value\n");
            goto exit;
        }
        if (buf[0] != 42u || buf[15] != 57u) {
            printf("alt_buf contents wrong after module write\n");
            goto exit;
        }

        /* Host -> module: host writes the pool; the module sums it back. */
        for (int i = 0; i < 16; i++)
            buf[i] = (uint32_t)(i * 3);
        uint32_t (*sum_fn)(void) = (uint32_t (*)(void))udynlink_get_symbol_value(&mod, "alt_sum");
        UDYNLINK_PREPARE_CALL(&mod);
        if (sum_fn() != 360u) {
            printf("alt_sum mismatch after host write\n");
            goto exit;
        }
        printf("sections: alt_buf round-trip OK\n");
    }

    test_unload_module(&mod);
    {
        size_t da = test_altpool_alloc_count() - allocs0;
        size_t df = test_altpool_free_count() - frees0;
        if (da != 2 || df != 2) {
            printf("free accounting unbalanced: %u allocs, %u frees\n",
                   (unsigned)da, (unsigned)df);
            goto exit;
        }
    }
    printf("sections: free accounting balanced (2/2)\n");

    res = 1;
exit:
    test_unload_module(&mod);
    return res;
}

/* Failure-injection: placement-validation paths of the new error codes.
 * One mode each, then the real load above runs — the knobs reset on use. */
static int test_placement_rejection(void) {
    udynlink_module_t mod;
    udynlink_error_t err;

    memset(&mod, 0, sizeof(mod));
    test_altpool_misalign_next();
    err = udynlink_load_module(&mod, mod_sections_module_data, NULL, 0,
                               UDYNLINK_LOAD_MODE_COPY_ALL);
    if (err != UDYNLINK_ERR_LOAD_SECTION_UNALIGNED) {
        printf("misaligned section not rejected (err %d)\n", (int)err);
        udynlink_unload_module(&mod);
        return 0;
    }
    printf("sections: misaligned placement rejected\n");

    memset(&mod, 0, sizeof(mod));
    test_altpool_fail_next();
    err = udynlink_load_module(&mod, mod_sections_module_data, NULL, 0,
                               UDYNLINK_LOAD_MODE_COPY_ALL);
    if (err != UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED) {
        printf("refused section not rejected (err %d)\n", (int)err);
        udynlink_unload_module(&mod);
        return 0;
    }
    printf("sections: refused placement rejected\n");
    return 1;
}

int test_qemu(void) {
    if (!test_placement_rejection())
        return 0;
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_mode((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
