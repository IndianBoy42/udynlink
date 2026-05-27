#include <stdio.h>

/*
 * Test module for weak symbol support.
 *
 * Design note: weak_func() is defined in this same compilation unit.
 * GCC therefore emits a PC-relative `bl` for internal calls (the prologue
 * wrapper branches to __wrapped_weak_func).  This means the host CANNOT
 * intercept direct internal calls at load time — only indirect calls via
 * function pointers or external lookups via udynlink_lookup_symbol() can
 * be overridden.  weak_var, on the other hand, is accessed through the
 * LOT and its relocation is fully patchable at load time.
 */

__attribute__((weak)) int weak_func(void) {
    return 42;
}

__attribute__((weak)) int weak_var = 7;

int test(void) {
    /* Reference weak_var so --gc-sections keeps it */
    return weak_var >= 0;
}
