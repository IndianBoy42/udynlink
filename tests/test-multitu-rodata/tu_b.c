/* tu_b.c — second translation unit.
 *
 * Same mechanism as tu_a.c: `b_str` introduces another .LC0 in this TU's
 * constant pool that shares its name with tu_a.c's .LC0 but lives at a
 * different address.  See tu_a.c for the rationale.
 */
#include <stdio.h>

static const char *b_str = "BBBB_FROM_B";

int emit_b(void) {
    puts(b_str);
    return b_str[0];
}
