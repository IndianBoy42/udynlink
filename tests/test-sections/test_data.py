# Multi-region placement: the module tags a 32-byte-aligned data buffer into
# the "alt" section and two functions into the "altcode" section (mkmodule
# --section). The host's allocator callback routes every non-NULL section
# name into a static secondary pool that the platform linker script places in
# the board's second RAM region (mps2_an386: BRAM @ 0x20000000). On
# stm32f429_discovery the pool is placed in normal RAM instead of CCM RAM:
# the legacy xPack QEMU model does not back 0x10000000 with usable memory, so
# a section placed there never round-trips (see the comment in that
# platform's sections.ld). Verifies section-table reporting
# (count/classes/align), that both tagged sections land inside the pool with
# the declared alignment, host<->module data round-trips through pool memory,
# and that unload frees exactly the sections load allocated.

test_data = {
    "desc": "Multi-region placement: tagged data + code sections in a second RAM region",
    "modules": [["mod_sections.c"]],
    "mkmodule_args": "--section alt:align=32 --section altcode",
    "required": [
        r"^sections: table reports alt \(data, align 32\)$",
        r"^sections: alt in secondary pool, 32-aligned$",
        r"^sections: altcode in secondary pool$",
        r"^sections: alt_buf round-trip OK$",
        r"^sections: free accounting balanced \(2/2\)$",
    ],
}
