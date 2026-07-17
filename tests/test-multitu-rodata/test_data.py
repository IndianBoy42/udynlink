# Regression test for the duplicate-named local symbol (.LC0) collision bug.
#
# When a module is built from more than one translation unit, GCC emits local
# constant-pool labels (.LC0, .LC1, ...) in every TU.  These share names but
# have distinct addresses.  mkmodule used to key symbols by name and silently
# collapse them, assigning every reference to the same name a single LOT slot
# and making each TU read the wrong literal.  This test forces that scenario:
# tu_a.c and tu_b.c each define a static pointer to its own string literal,
# and test() checks that each TU reads its own literal (by first character).

test_data = {
    "desc": "Multi-TU module with duplicate local symbols (.LC0) across TUs",
    "modules": [["mod_multitu_rodata.c", "tu_a.c", "tu_b.c"]],
    "required": [r"^AAAA_FROM_A$", r"^BBBB_FROM_B$", r"^Running test 'mod_multitu_rodata'$"]
}
