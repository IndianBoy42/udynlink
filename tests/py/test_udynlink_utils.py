"""Regression tests for udynlink_utils ELF symbol/relocation reading.

These guard against the duplicate-named local symbol (.LC0) collision bug:
when a module is built from multiple translation units, GCC emits local
constant-pool labels (.LC0, .LC1, ...) in every TU.  These share a name but
have distinct st_value/st_shndx.  ``get_symbols_in_elf`` must preserve all of
them (not collapse by name into a dict), and ``get_relocations_in_elf`` must
carry enough identity (symtab + sym_idx) for mkmodule to assign each reference
a distinct LOT slot.

Integration tests (marked ``@pytest.mark.integration``) drive the real
``mkmodule`` toolchain (``--stop-after-link``) to produce an ELF that contains
the duplicate .LC0 symbols, then assert on the utils readers.  They are skipped
when ``arm-none-eabi-gcc`` is unavailable.
"""

import os
import shutil
import subprocess
import sys
import types

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_UTILS_PATH = os.path.join(_REPO_ROOT, "scripts", "udynlink_utils.py")
_MKMODULE = os.path.join(_REPO_ROOT, "scripts", "mkmodule")
_PY = sys.executable

udynlink_utils = types.ModuleType("udynlink_utils")
udynlink_utils.__file__ = _UTILS_PATH
with open(_UTILS_PATH) as _f:
    _code = compile(_f.read(), _UTILS_PATH, "exec")
    exec(_code, udynlink_utils.__dict__)

get_symbols_in_elf = udynlink_utils.get_symbols_in_elf
get_relocations_in_elf = udynlink_utils.get_relocations_in_elf

_ARM_GCC = shutil.which("arm-none-eabi-gcc")
skip_no_arm_gcc = pytest.mark.skipif(
    _ARM_GCC is None,
    reason="arm-none-eabi-gcc not available",
)

# Two TUs, each with a static pointer to its own string literal.  Each TU's
# constant pool introduces a .LC0; the linked ELF therefore contains two
# same-named local symbols at distinct addresses.  Mirrors the
# tests/test-multitu-rodata QEMU regression test.
_TU_A = (
    '#include <stdio.h>\n'
    'static const char *a_str = "AAAA_FROM_A";\n'
    'int emit_a(void) { puts(a_str); return a_str[0]; }\n'
)
_TU_B = (
    '#include <stdio.h>\n'
    'static const char *b_str = "BBBB_FROM_B";\n'
    'int emit_b(void) { puts(b_str); return b_str[0]; }\n'
)
_MOD = (
    '#include <stdio.h>\n'
    'extern int emit_a(void);\n'
    'extern int emit_b(void);\n'
    'int test(void) { printf("Running test \'%s\'\\n", "mod"); '
    'return (emit_a()==\'A\') && (emit_b()==\'B\'); }\n'
)


def _build_linked_elf(tmp_path):
    """Drive mkmodule --stop-after-link to produce a multi-TU ELF.

    mkmodule's linker script and prologue wrappers are what actually cause
    GCC's per-TU .LC0 labels to survive into the linked ELF, so we reuse the
    real toolchain rather than approximating it.
    """
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    (src_dir / "tu_a.c").write_text(_TU_A)
    (src_dir / "tu_b.c").write_text(_TU_B)
    (src_dir / "mod.c").write_text(_MOD)
    work = tmp_path / "work"
    work.mkdir()
    env = dict(os.environ, UDYNLINK_WORKDIR=str(work))
    cmd = [_PY, _MKMODULE, "--stop-after-link", "--no-debug",
           "--target", "cortex-m4", "--module-name", "mod",
           "--bin-name", str(work / "mod.bin"),
           str(src_dir / "mod.c"), str(src_dir / "tu_a.c"), str(src_dir / "tu_b.c")]
    subprocess.run(cmd, check=True, env=env, cwd=str(src_dir),
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    elf = work / "mod.elf"
    assert elf.exists(), "mkmodule did not produce mod.elf"
    return str(elf)


@skip_no_arm_gcc
def test_get_symbols_preserves_duplicate_local_names(tmp_path):
    """get_symbols_in_elf must return a list keeping every .LC0 entry.

    Previously it keyed symbols by name in a dict and silently kept only the
    last .LC0, collapsing every reference onto one address.
    """
    elf = _build_linked_elf(tmp_path)
    syms = get_symbols_in_elf(elf)
    assert isinstance(syms, list)
    lc0 = [s for s in syms if s["name"] == ".LC0" and s["bind"] == "STB_LOCAL"]
    assert len(lc0) >= 2, "expected >=2 .LC0 local symbols; got %d" % len(lc0)
    # Distinct string literals in different TUs -> distinct addresses.
    values = {s["value"] for s in lc0}
    assert len(values) >= 2, ".LC0 entries collapsed to one value: %r" % values
    # Each entry carries unique (symtab, idx) identity.
    keys = {(s["symtab"], s["idx"]) for s in lc0}
    assert len(keys) == len(lc0), "symbol (symtab, idx) keys are not unique"


@skip_no_arm_gcc
def test_relocations_carry_symbol_identity(tmp_path):
    """get_relocations_in_elf must carry symtab + sym_idx so mkmodule can
    assign distinct LOT slots to same-named local symbols."""
    elf = _build_linked_elf(tmp_path)
    rels = get_relocations_in_elf(elf)
    got_brel = [r for r in rels if r["type"] == "R_ARM_GOT_BREL" and r["name"] == ".LC0"]
    assert len(got_brel) >= 2, "expected >=2 GOT_BREL relocs for .LC0; got %d" % len(got_brel)
    for r in got_brel:
        assert "sym_idx" in r and "symtab" in r
    # Two .LC0 references from different TUs must have distinct identity and
    # distinct target values.
    ident = {(r["symtab"], r["sym_idx"]) for r in got_brel}
    assert len(ident) >= 2, "relocs for .LC0 collapsed to one identity: %r" % ident
    values = {r["value"] for r in got_brel}
    assert len(values) >= 2, "relocs for .LC0 collapsed to one value: %r" % values
