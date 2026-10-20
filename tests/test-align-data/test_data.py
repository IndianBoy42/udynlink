# Sectioned image whose `.data` needs 8-byte alignment, so the linker starts
# it after a `.text`->`.data` gap (see mod_align_data.c). Regression: the
# encoder must pad the CODE payload up to .data's VA instead of prepending
# the gap to .data — otherwise every data relocation lands one word off and
# the loader refuses the image (UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE).
# Loads in all three load modes and verifies the ABS32 pointer inside .data
# plus the aligned doubles read back correctly.

test_data = {
    "desc": "Sectioned image with a .text->.data alignment gap: relocations and aligned data stay correct",
    "modules": [["mod_align_data.c"]],
    "mkmodule_args": "--section alt",
    "required": [
        r"^align-data: loads and relocates in every load mode$",
        r"^align-data: .data pointer and 8-byte-aligned table intact$",
        r"^align-data: relocated .data function-pointer table OK$",
        r"^align-data: tagged section reachable$",
    ],
}
