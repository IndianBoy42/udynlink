#!/usr/bin/env python3
"""
Target database for udynlink module compilation.

Maps target names to compiler/assembler/runtime metadata.
"""

# Architecture tag layout (uint16_t):
#   Bits [3:0]  — core family ID
#   Bit  4      — FPU present
#   Bits [6:5]  — float ABI (00=soft, 01=softfp, 10=hard)
#   Bits [15:7] — reserved

def _make_arch_tag(family, fpu, float_abi):
    """Build architecture tag from components."""
    tag = family & 0x0F
    if fpu:
        tag |= 0x10
    abi_code = {"soft": 0, "softfp": 1, "hard": 2}
    tag |= (abi_code.get(float_abi, 0) & 0x03) << 5
    return tag


TARGETS = {
    "cortex-m0": {
        "mcpu": "cortex-m0",
        "arch": "armv6-m",
        "fpu": None,
        "float_abi": "soft",
        "arch_tag": _make_arch_tag(1, False, "soft"),
        "template": "asm_template_armv6m.tmpl",
    },
    "cortex-m0plus": {
        "mcpu": "cortex-m0plus",
        "arch": "armv6-m",
        "fpu": None,
        "float_abi": "soft",
        "arch_tag": _make_arch_tag(2, False, "soft"),
        "template": "asm_template_armv6m.tmpl",
    },
    "cortex-m3": {
        "mcpu": "cortex-m3",
        "arch": "armv7-m",
        "fpu": None,
        "float_abi": "soft",
        "arch_tag": _make_arch_tag(3, False, "soft"),
        "template": "asm_template_armv7m.tmpl",
    },
    "cortex-m4": {
        "mcpu": "cortex-m4",
        "arch": "armv7e-m",
        "fpu": None,
        "float_abi": "soft",
        "arch_tag": _make_arch_tag(4, False, "soft"),
        "template": "asm_template_armv7m.tmpl",
    },
    "cortex-m4f": {
        "mcpu": "cortex-m4",
        "arch": "armv7e-m",
        "fpu": "fpv4-sp-d16",
        "float_abi": "hard",
        "arch_tag": _make_arch_tag(4, True, "hard"),
        "template": "asm_template_armv7m.tmpl",
    },
    "cortex-m7": {
        "mcpu": "cortex-m7",
        "arch": "armv7e-m",
        "fpu": "fpv5-d16",
        "float_abi": "hard",
        "arch_tag": _make_arch_tag(7, True, "hard"),
        "template": "asm_template_armv7m.tmpl",
    },
    "cortex-m33": {
        "mcpu": "cortex-m33",
        "arch": "armv8-m.main",
        "fpu": None,
        "float_abi": "soft",
        "arch_tag": _make_arch_tag(8, False, "soft"),
        "template": "asm_template_armv8m.tmpl",
    },
    "cortex-m55": {
        "mcpu": "cortex-m55",
        "arch": "armv8.1-m.main",
        "fpu": "auto",
        "float_abi": "hard",
        "arch_tag": _make_arch_tag(9, True, "hard"),
        "template": "asm_template_armv8m.tmpl",
    },
    "cortex-m85": {
        "mcpu": "cortex-m85",
        "arch": "armv8.1-m.main",
        "fpu": "auto",
        "float_abi": "hard",
        "arch_tag": _make_arch_tag(10, True, "hard"),
        "template": "asm_template_armv8m.tmpl",
    },
}

DEFAULT_TARGET = "cortex-m4"


def get_target(name):
    """Return target metadata by name, or raise ValueError."""
    if name not in TARGETS:
        raise ValueError(f"Unknown target '{name}'. Supported: {', '.join(TARGETS.keys())}")
    return TARGETS[name]


def get_target_names():
    """Return list of supported target names."""
    return list(TARGETS.keys())


if __name__ == "__main__":
    # Print target table for reference
    print("| Target | mcpu | arch | fpu | float_abi | arch_tag |")
    print("|--------|------|------|-----|-----------|----------|")
    for name, meta in TARGETS.items():
        print(f"| {name} | {meta['mcpu']} | {meta['arch']} | {meta['fpu'] or '—'} | {meta['float_abi']} | 0x{meta['arch_tag']:02X} |")
