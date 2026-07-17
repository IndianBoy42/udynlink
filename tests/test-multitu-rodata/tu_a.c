/* tu_a.c — first translation unit.
 *
 * `a_str` is a static pointer to a string literal.  GCC emits the literal as
 * a local constant-pool label (.LC0) local to this TU.  When a second TU
 * (tu_b.c) is linked into the same module it produces its own .LC0 with the
 * same name but a distinct address.  mkmodule must keep the two .LC0 symbols
 * distinct and assign them separate LOT slots, otherwise emit_a() would read
 * tu_b's literal at runtime.
 */
#include <stdio.h>

static const char *a_str = "AAAA_FROM_A";

int emit_a(void) {
    puts(a_str);
    return a_str[0];
}
