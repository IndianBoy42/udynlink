# Relocate an already-loaded module's RAM region without losing runtime state.
# Exercises data preservation (.data/.bss), code-pointer rebase (p_sq),
# data-pointer rebase (p_g), EXTERN-slot non-rebase (printf via test()), the
# XIP code-stays-in-flash invariant, and both foreign and malloc provision paths.

test_data = {
    "desc": "Module RAM relocation (state-preserving)",
    "modules": [["mod_relocate.c"]],
    "required": ["Running test 'mod_relocate'"]
}
