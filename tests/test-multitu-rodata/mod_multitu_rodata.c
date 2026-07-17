/* mod_multitu_rodata.c — module entry point.
 *
 * Built from three translation units (this file plus tu_a.c and tu_b.c) to
 * exercise the duplicate-local-symbol (.LC0) code path in mkmodule.  test()
 * calls into both helper TUs and verifies that each one reads its OWN string
 * literal (by checking the first character): with the .LC0 collision bug,
 * emit_a() would return 'B' instead of 'A' and the test would fail.
 */
#include <stdio.h>

extern int emit_a(void);
extern int emit_b(void);

int test(void) {
    printf("Running test '%s'\n", "mod_multitu_rodata");
    int ra = emit_a();
    int rb = emit_b();
    return (ra == 'A') && (rb == 'B');
}
