/* Sectioned module that reproduces the `.text`->`.data` alignment-gap bug.
 *
 * `pair` is 8-byte aligned and first in `.data`, while the module linker
 * script starts `.data` with `ALIGN(4)`: the linker therefore places `.data`
 * after a gap. `mkmodule` must fold that gap into the CODE payload and keep
 * `.data`'s payload at `.data`'s VA; pre-pending it to `.data` instead shifts
 * every data relocation by the gap, so the loader either rejects the image
 * (UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE) or patches the wrong word.
 *
 * `tbl` holds function pointers, which are relocations whose *target words
 * live in `.data`* — exactly what the bug misplaced. `call_tbl()` calls
 * through them, so a mis-relocated table jumps to garbage instead of
 * returning 11/22. */
#include <stdint.h>
#include <stdio.h>
#include "udynlink_section.h"

/* First in .data and 8-aligned: forces the `.text`->`.data` gap. */
__attribute__((aligned(8))) uint64_t pair[2] = {0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};

/* A relocated pointer inside .data (also a data-section relocation site). */
static uint64_t *data_ptr = &pair[1];

static int f1(void) { return 11; }
static int f2(void) { return 22; }

/* Function pointers in .data: R_ARM_ABS32 relocations targeting .data words. */
static int (*const tbl[2])(void) = {f1, f2};

UDYNLINK_SECTION("alt") volatile uint32_t alt_marker = 0x5A5A1234u;

int test(void) {
    printf("Running test 'mod_align_data'\n");
    return 1;
}

/* Entry points are called through the LOT, so the module never relies on
 * cross-region calls. */
int align_add(int a, int b) {
    alt_marker++;
    return a + b;
}

/* 1 when the relocated .data pointer and the aligned table are intact. */
int check_pair(void) {
    if (data_ptr != &pair[1])
        return 0;
    if ((uintptr_t)pair & 7u)
        return 0;
    if (*data_ptr != 0x99AABBCCDDEEFF00ull)
        return 0;
    return (uint64_t)(pair[0] ^ 0x1122334455667788ull) == 0;
}

/* Calls through the relocated function-pointer table in .data. */
int call_tbl(int i) {
    return tbl[i & 1]();
}

uint32_t get_marker(void) {
    return alt_marker;
}
