"""Regression tests for the C++ symbol-table filter flags in mkmodule.

Each flag (``--strip-hidden-syms``, ``--strip-mangled-syms``,
``--strip-weak-sym-names``, ``--strip-non-public-syms``) is exercised
end-to-end against a templated C++ module.  The tests parse the resulting
``.bin`` symbol table using the same on-disk format ``udynlink.c`` expects
and assert that the right symbols stay/leave the named pool.  They are
skipped when ``arm-none-eabi-gcc`` is unavailable.
"""

import os
import shutil
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_MKMODULE = os.path.join(_REPO_ROOT, "scripts", "mkmodule")
_UTILS_PATH = os.path.join(_REPO_ROOT, "scripts", "udynlink_utils.py")
_PY = sys.executable

# A templated C++ module with a per-symbol STV_HIDDEN helper and an
# extern "C" entry point.  Mirrors tests/test-cpp-symbol-filter/mod_cpp_filter.cpp
# but inlined here so this test file is self-contained.
_MODULE_SRC = r"""
#include <stdio.h>

template <typename T>
__attribute__((noinline, visibility("hidden")))
int internal_templated(T v) {
    int sum = 0;
    for (int i = 0; i < 4; ++i) sum += (int)v + i;
    return sum;
}

extern "C" int run_filter_test(int arg) {
    int (*volatile fp)(int) = internal_templated<int>;
    int r = fp(arg);
    printf("filtered %d\n", r);
    return 0;
}

extern "C" int test(void) {
    return run_filter_test(7) == 0;
}
"""

_ARM_GCC = shutil.which("arm-none-eabi-gcc")
skip_no_arm_gcc = pytest.mark.skipif(
    _ARM_GCC is None,
    reason="arm-none-eabi-gcc not available",
)


# ---------------------------------------------------------------------------
# .bin symbol table parser — delegated to the standalone parser library.
# ---------------------------------------------------------------------------
#
# The on-disk symbol-table format is defined once in
# ``scripts/udynlink_parser.py`` (the single source of truth, mirroring
# udynlink/udynlink.c and the mkmodule writer).  These constants are re-exported
# here only so existing test call sites keep working without touching their
# ``_SYM_TYPE_*`` references; the parsing itself goes through
# ``udynlink_parser.parse_module``.

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_THIS_DIR, "..", "..", "scripts"))
import udynlink_parser as _up

_SYM_OFFSET_MASK    = _up.SYM_NAME_OFFSET_MASK
_SYM_INFO_SHIFT     = _up.SYM_INFO_SHIFT
_SYM_TYPE_MASK      = _up.SYM_INFO_TYPE_MASK
_SYM_TYPE_INTERNAL  = _up.SYM_TYPE_INTERNAL
_SYM_TYPE_EXPORTED  = _up.SYM_TYPE_EXPORTED
_SYM_TYPE_EXTERN    = _up.SYM_TYPE_EXTERN
_SYM_TYPE_MODULE_NAME = _up.SYM_TYPE_MODULE_NAME
_SYM_TYPE_WEAK      = _up.SYM_TYPE_WEAK


def _parse_symtab(bin_path):
    """Return a list of dicts describing every entry in the module's symt.

    Each dict has ``name`` (or ``None`` for INTERNAL), ``type`` (int 0..4)
    and ``value`` (int).  Delegates to ``udynlink_parser.parse_module`` so the
    format is decoded exactly once across the Python toolchain.
    """
    with open(bin_path, "rb") as f:
        data = f.read()
    img = _up.parse_module(data)
    return [
        {"name": s.name, "type": s.type, "value": s.val_raw}
        for s in img.symbols
    ]


def _named_entries(entries):
    """Return entries that have a non-empty name (i.e. non-INTERNAL)."""
    return [e for e in entries if e["name"]]

def _build_module(tmp_path, *extra_args):
    """Drive mkmodule on the templated C++ source, return the .bin path."""
    src_dir = tmp_path / "src"
    src_dir.mkdir(parents=True)
    src = src_dir / "mod_cpp_filter.cpp"
    src.write_text(_MODULE_SRC)
    out_dir = tmp_path / "build"
    out_dir.mkdir(parents=True)
    cmd = [_PY, _MKMODULE, "--workdir", str(out_dir), "--header-path", str(out_dir)]
    cmd.extend(extra_args)
    cmd.append(str(src))
    subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return out_dir / "mod_cpp_filter.bin"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


@skip_no_arm_gcc
def test_strip_hidden_syms_demotes_stv_hidden_named(tmp_path):
    """--strip-hidden-syms demotes the per-symbol STV_HIDDEN helper from
    the named pool and shrinks the symt while keeping extern "C" exports."""
    baseline = _build_module(tmp_path / "base")
    filtered = _build_module(tmp_path / "filt", "--strip-hidden-syms")
    base_entries = _parse_symtab(baseline)
    filt_entries = _parse_symtab(filtered)
    base_named = {e["name"] for e in _named_entries(base_entries)}
    filt_named = {e["name"] for e in _named_entries(filt_entries)}
    # The wrapped name carries the mangled mangled template symbol as a suffix.
    assert any("_Z" in n and "internal_templated" in n for n in base_named), (
        "pre-flag build must contain the STV_HIDDEN templated helper; got %r" % base_named
    )
    # With the flag, no STV_HIDDEN named symbol remains.
    for n in filt_named:
        assert not (n.startswith("__") and "_Z" in n and "internal_templated" in n), (
            "filtered build still carries a wrapped hidden mangled name: %r" % n
        )
    # Exports must remain.
    assert "test" in filt_named
    assert "run_filter_test" in filt_named
    # Table must shrink.
    base_size = os.path.getsize(baseline)
    filt_size = os.path.getsize(filtered)
    assert filt_size < base_size, "filter did not shrink module .bin: %d -> %d" % (base_size, filt_size)


@skip_no_arm_gcc
def test_strip_mangled_syms_removes_all_z_names(tmp_path):
    """--strip-mangled-syms demotes every defined _Z* symbol and the
    prologue wrapper that previously hid them is also suppressed, so
    no mangled name appears in the named pool."""
    # The helloworld-cpp-style module is needed to get a name pool with
    # wrapped-mangled entries; we re-use the same source.
    filtered = _build_module(tmp_path / "filt", "--strip-mangled-syms")
    entries = _parse_symtab(filtered)
    named = [e["name"] for e in _named_entries(entries)]
    for n in named:
        assert not n.startswith("_Z"), "mangled name survived: %r" % n
        # Wrapped names carry the original as a suffix; they must not appear
        # as the only form of a mangled symbol.
        assert not (n.startswith("__") and "_Z" in n), (
            "wrapped mangled name still in named pool: %r" % n
        )


@skip_no_arm_gcc
def test_strip_weak_sym_names_demotes_defined_weaks(tmp_path):
    """--strip-weak-sym-names demotes defined STB_WEAK entries; the named
    pool should contain no weak entries of any kind."""
    filtered = _build_module(tmp_path / "filt", "--strip-weak-sym-names")
    entries = _parse_symtab(filtered)
    for e in entries:
        assert e["type"] != _SYM_TYPE_WEAK, (
            "weak entry survived: name=%r" % e["name"]
        )


@skip_no_arm_gcc
def test_strip_non_public_syms_keeps_only_listed(tmp_path):
    """--strip-non-public-syms with --public-symbols=run_filter_test keeps
    run_filter_test as EXPORTED and demotes every other defined STB_GLOBAL."""
    filtered = _build_module(
        tmp_path / "filt",
        "--public-symbols=run_filter_test",
        "--strip-non-public-syms",
    )
    entries = _parse_symtab(filtered)
    named = {e["name"]: e for e in _named_entries(entries)}
    assert "run_filter_test" in named, "the listed symbol was demoted"
    assert named["run_filter_test"]["type"] == _SYM_TYPE_EXPORTED
    # `test` was not in --public-symbols, so it must not appear as EXPORTED.
    assert "test" not in named, "non-listed EXPORTED survived: test"


@skip_no_arm_gcc
def test_default_behavior_unchanged(tmp_path):
    """Without any of the new flags the build is byte-identical to a
    pre-flag reference; the demotion block must be a no-op when no
    flag is set."""
    base1 = _build_module(tmp_path / "a")
    base2 = _build_module(tmp_path / "b")
    assert open(base1, "rb").read() == open(base2, "rb").read()


@skip_no_arm_gcc
def test_composition_all_four_flags(tmp_path):
    """All four flags at once must not break module compilation or
    symbol-table invariants: extern "C" exports stay, externs stay,
    the module name stays, the named pool has no mangled/weak entries."""
    filtered = _build_module(
        tmp_path / "filt",
        "--public-symbols=test",
        "--strip-hidden-syms",
        "--strip-non-public-syms",
        "--strip-mangled-syms",
        "--strip-weak-sym-names",
    )
    entries = _parse_symtab(filtered)
    named = [e["name"] for e in _named_entries(entries)]
    # Listed symbol survives.
    assert "test" in named
    # run_filter_test was not in --public-symbols: demoted (no name in pool).
    assert "run_filter_test" not in named
    # No mangled names in the named pool.
    for n in named:
        assert not n.startswith("_Z"), "mangled name survived: %r" % n
    # No weak entries.
    for e in entries:
        assert e["type"] != _SYM_TYPE_WEAK, "weak entry survived: %r" % e["name"]
    # The module name entry (type 3) is always present.
    assert any(e["type"] == _SYM_TYPE_MODULE_NAME for e in entries)
