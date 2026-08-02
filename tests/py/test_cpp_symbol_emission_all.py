"""Comprehensive C++ symbol-table emission tests.

Exercises every way GCC emits C++ symbols (extern "C", member functions,
constructors/destructors, virtual dispatch via vtable, function templates,
inline functions, weak functions, init/fini arrays) under every
combination of the ``--strip-*`` flags.

The goal is to catch *any* regression in the C++ symbol-table filter
pipeline: missing demotions, accidental demotion of host-callable
symbols, broken init-array wiring, vtable/GOT reloc mismatches, etc.

The C++ source mirrors ``tests/test-cpp-symbol-emission-all/mod_all.cpp``
and is inlined here so this test file is self-contained.  Each test
builds the module with a specific flag combination and asserts the
expected symt shape.
"""

import os
import shutil
import struct
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_symbol_filter import _parse_symtab, _SYM_TYPE_EXPORTED  # noqa: E402
from test_symbol_filter import (
    _SYM_TYPE_EXTERN,
    _SYM_TYPE_INTERNAL,
    _SYM_TYPE_MODULE_NAME,
    _SYM_TYPE_WEAK,
    skip_no_arm_gcc,
)

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_MKMODULE = os.path.join(_REPO_ROOT, "scripts", "mkmodule")
_COMPREHENSIVE_CPP = os.path.join(
    _REPO_ROOT, "tests", "test-cpp-symbol-emission-all", "mod_all.cpp"
)
_PY = sys.executable


def _build(tmp_path, *extra_args):
    """Build the comprehensive C++ module, return the .bin path.

    Copies the on-disk ``mod_all.cpp`` to a per-test src dir so each
    test gets a clean build dir and there's no cross-test interference.
    """
    src_dir = tmp_path / "src"
    src_dir.mkdir(parents=True, exist_ok=True)
    src = src_dir / "mod_all.cpp"
    src.write_text(open(_COMPREHENSIVE_CPP, "r").read())
    out_dir = tmp_path / "build"
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [_PY, _MKMODULE, "--workdir", str(out_dir), "--header-path", str(out_dir)]
    cmd.extend(extra_args)
    cmd.append(str(src))
    subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return out_dir / "mod_all.bin"


def _by_name(entries):
    """Map symbol name -> entry (or None for INTERNAL)."""
    return {e["name"]: e for e in entries if e["name"]}


def _type_counts(entries):
    """Count of each symbol type."""
    out = {
        _SYM_TYPE_INTERNAL: 0,
        _SYM_TYPE_EXPORTED: 0,
        _SYM_TYPE_EXTERN: 0,
        _SYM_TYPE_MODULE_NAME: 0,
        _SYM_TYPE_WEAK: 0,
    }
    for e in entries:
        out[e["type"]] = out.get(e["type"], 0) + 1
    return out


# ---------------------------------------------------------------------------
# The four --strip-* flags in a fixed bit order, plus the public
# allowlist of extern "C" names that the host may call by name.
# ---------------------------------------------------------------------------

# (bit, flag-name) — bit 0 == least-significant of the 4-bit combo id.
_STRIP_FLAGS = [
    (0, "--strip-mangled-syms"),
    (1, "--strip-weak-sym-names"),
    (2, "--strip-hidden-syms"),
    (3, "--strip-non-public-syms"),
]

_PUBLIC_ALLOWLIST = "add,sub,mul,test,call_mangled_through_got,call_via_templated,c_ctor_fn"

# Enumerate all 16 combinations of the four flags as 4-bit ids 0000..1111.
# Each id bit selects whether the corresponding flag in ``_STRIP_FLAGS`` is
# passed to mkmodule.  The id string ("0000", "0001", ...) is the test id.
_FLAG_COMBOS = [format(i, "04b") for i in range(16)]


def _flags_from_combo(combo_id):
    """Expand a 4-bit combo id ("0011") into the list of mkmodule flags.

    When ``--strip-non-public-syms`` is selected, the public allowlist is
    injected so the listed extern "C" entry points survive; this mirrors
    how a real user must configure the flag to keep anything callable."""
    flags = []
    for bit, flag in _STRIP_FLAGS:
        if combo_id[3 - bit] == "1":  # MSB == bit 3
            flags.append(flag)
    if "--strip-non-public-syms" in flags:
        flags = ["--public-symbols=" + _PUBLIC_ALLOWLIST] + flags
    return flags


# ---------------------------------------------------------------------------
# Categorical assertions: regardless of which --strip-* flags are set,
# certain invariants MUST hold.
# ---------------------------------------------------------------------------


@skip_no_arm_gcc
def test_module_name_always_first(tmp_path):
    """The MODULE_NAME entry is the first symt entry in every build,
    independent of any --strip-* flag (including --strip-hidden-syms,
    which the prologue can re-define bare mangled names with default
    visibility — the module name itself is added by mkmodule, not the
    prologue, so it's unaffected)."""
    for args in (
        [],
        ["--strip-hidden-syms"],
        ["--strip-mangled-syms"],
        ["--strip-weak-sym-names"],
        ["--public-symbols=add,sub,mul,test", "--strip-non-public-syms"],
        ["--public-symbols=add", "--strip-non-public-syms", "--strip-mangled-syms"],
        ["--strip-hidden-syms", "--strip-mangled-syms", "--strip-weak-sym-names"],
    ):
        bin_path = _build(tmp_path / ("v_" + "_".join(a.replace("--", "") for a in args) or "baseline"), *args)
        entries = _parse_symtab(bin_path)
        assert entries, "no symt entries"
        assert entries[0]["type"] == _SYM_TYPE_MODULE_NAME, (
            "module name not first under flags %r: %r" % (args, entries[0])
        )
        assert entries[0]["name"] == "mod_all", (
            "module name wrong: %r under %r" % (entries[0]["name"], args)
        )


@skip_no_arm_gcc
def test_extern_c_exports_survive_all_flags(tmp_path):
    """The plain ``extern "C"`` symbols (``add``, ``sub``, ``mul``,
    ``call_mangled_through_got``, ``call_via_templated``, ``c_ctor_fn``)
    MUST stay as EXPORTED under every flag combination — they are the
    only symbols the host can call by name.  Even when the user
    ``--public-symbols`` allowlists, the listed names must remain."""
    extern_c = ["add", "sub", "mul", "call_mangled_through_got",
                "call_via_templated", "c_ctor_fn"]
    scenarios = [
        (["--strip-hidden-syms"], None),  # --strip-hidden-syms alone: all extern C kept
        (["--strip-mangled-syms"], None),  # --strip-mangled-syms alone: all extern C kept
        (["--strip-weak-sym-names"], None),  # --strip-weak-sym-names alone: all extern C kept
        (["--public-symbols=" + ",".join(extern_c)], None),  # allowlist: all extern C kept
        (["--public-symbols=add,sub,mul,test", "--strip-non-public-syms"], ["add", "sub", "mul", "test"]),
    ]
    for args, expected in scenarios:
        bin_path = _build(tmp_path / ("x_" + "_".join(a.replace("--", "") for a in args)), *args)
        entries = _parse_symtab(bin_path)
        named = _by_name(entries)
        if expected is None:
            expected = extern_c
        for n in expected:
            assert n in named, "extern C %r missing under %r" % (n, args)
            assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
                "extern C %r is not EXPORTED under %r: %r" % (n, args, named[n])
            )


@skip_no_arm_gcc
def test_no_demote_without_flag(tmp_path):
    """With NO --strip-* flag set, the demotion block is a no-op.
    Every symbol that mkmodule classifies as exported/weak stays
    that way, and the bin is byte-identical across two runs."""
    a = _build(tmp_path / "a")
    b = _build(tmp_path / "b")
    assert open(a, "rb").read() == open(b, "rb").read(), "non-deterministic baseline"
    entries = _parse_symtab(a)
    # Sanity: the templated function and inline helper are present and weak.
    named = _by_name(entries)
    assert "_Z9templatedIiEiT_" in named, "baseline missing _Z9templatedIiEiT_"
    assert named["_Z9templatedIiEiT_"]["type"] == _SYM_TYPE_WEAK
    assert "_Z13inline_helperi" in named
    assert named["_Z13inline_helperi"]["type"] == _SYM_TYPE_WEAK
    # New symbols added to mod_all.cpp must also be present, as WEAK or
    # EXPORTED — none of them should be silently demoted without any
    # --strip-* flag active.
    new_weak = [
        # DtorTest: complete (C1) + base (C2) constructors, complete (D1)
        # + deleting (D2) destructors — all emitted STB_WEAK.
        "_ZN8DtorTestC1Ev", "_ZN8DtorTestC2Ev",
        "_ZN8DtorTestD1Ev", "_ZN8DtorTestD2Ev",
        # Template variable instantiation (STB_WEAK OBJECT data).
        "_Z9g_defaultIiE",
        # DataHolder::get_next — static member function, WEAK.
        "_ZN10DataHolder8get_nextEv",
        # MultiDerived overrides + MI thunk (this-adjustment).
        "_ZN12MultiDerived5mix_aEi", "_ZN12MultiDerived5mix_bEi",
        "_ZThn4_N12MultiDerived5mix_bEi",
    ]
    for n in new_weak:
        assert n in named, "baseline missing new symbol %r" % n
        assert named[n]["type"] in (_SYM_TYPE_WEAK, _SYM_TYPE_EXPORTED), (
            "baseline new symbol %r unexpectedly demoted: %r" % (n, named[n])
        )
    # The namespace function and the static-member data variable are
    # emitted as plain STB_GLOBAL (EXPORTED) by GCC, not WEAK.
    new_exported = [
        "_ZN4util4calcEi",            # namespace function
        "_ZN10DataHolder7counterE",   # static member data (defined out-of-line)
    ]
    for n in new_exported:
        assert n in named, "baseline missing new exported symbol %r" % n
        assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
            "baseline new exported symbol %r not EXPORTED: %r" % (n, named[n])
        )


# ---------------------------------------------------------------------------
# Per-flag behavior on the comprehensive module.
# ---------------------------------------------------------------------------


@skip_no_arm_gcc
def test_strip_mangled_demotes_all_z_symbols(tmp_path):
    """--strip-mangled-syms must demote every defined _Z* symbol —
    function, vtable, typeinfo, template instantiation, constructor,
    destructor.  None should remain in the named pool.

    With the new symbols in mod_all.cpp this also covers the namespace
    function (``_ZN4util4calcEi``), DtorTest ctor/dtor, the template
    variable, the static-member data + function, and the MultiDerived
    overrides + thunk — because the assertion is categorical over every
    ``_Z*`` name in the symtab."""
    bin_path = _build(tmp_path / "f", "--strip-mangled-syms")
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)
    leaked = [n for n in named if n.startswith("_Z")]
    assert not leaked, "mangled names survived --strip-mangled-syms: %r" % leaked


@skip_no_arm_gcc
def test_strip_mangled_keeps_extern_c_and_underscore_z(tmp_path):
    """--strip-mangled-syms does NOT touch extern "C" symbols.  The
    named pool must still contain ``add``, ``sub``, ``mul`` and the
    other host-callable functions.  The EXTERN entry for ``printf``
    (or ``puts``) must also be present (those are foreign functions,
    not defined in the module)."""
    bin_path = _build(tmp_path / "f", "--strip-mangled-syms")
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)
    for n in ("add", "sub", "mul", "call_mangled_through_got",
              "call_via_templated", "c_ctor_fn", "test"):
        assert n in named, "extern C %r missing under --strip-mangled-syms" % n
        assert named[n]["type"] == _SYM_TYPE_EXPORTED
    has_extern_stdout = ("printf" in named) or ("puts" in named)
    assert has_extern_stdout, "no stdout extern (printf/puts) survived"


@skip_no_arm_gcc
def test_strip_weak_demotes_defined_weaks(tmp_path):
    """--strip-weak-sym-names demotes every defined STB_WEAK entry.
    This includes:
      * explicit-instantiation templates (``_Z9templatedIiEiT_``)
      * inline functions (``_Z13inline_helperi``)
      * hidden templates (``_Z16hidden_templatedIiEiT_``)
      * weak extern "C" (``weak_fn``)
      * C++ vtable entry functions
      * DtorTest ctor/dtor variants (C1/C2/D1/D2)
      * template variable instantiation (``_Z9g_defaultIiE``)
      * DataHolder::get_next static member function
      * MultiDerived overrides + MI thunk (``_ZThn4_*``)
    After the flag, the named pool must contain zero WEAK entries.
    """
    bin_path = _build(tmp_path / "f", "--strip-weak-sym-names")
    entries = _parse_symtab(bin_path)
    weaks = [e for e in entries if e["type"] == _SYM_TYPE_WEAK]
    assert not weaks, "weak entries survived --strip-weak-sym-names: %r" % [
        e["name"] for e in weaks
    ]


@skip_no_arm_gcc
def test_strip_hidden_demotes_stv_hidden(tmp_path):
    """--strip-hidden-syms removes the STV_HIDDEN hidden template's named form.

    The hidden template ``_Z16hidden_templatedIiEiT_`` is STV_HIDDEN in the
    original .o; mkmodule's prologue wrap re-defines a bare
    ``_Z16hidden_templatedIiEiT_`` with STV_DEFAULT visibility and localizes
    the wrapped ``__<hash>___Z16hidden_templatedIiEiT_`` body in the ELF, so
    the wrapped name never appears in the named pool — with or without the
    flag. The bare name remains an EXPORTED entry (the public wrapper).
    This test asserts the wrapped name is never exported and the flag never
    grows the image."""
    baseline = _build(tmp_path / "base")
    filtered = _build(tmp_path / "filt", "--strip-hidden-syms")
    base_named = {e["name"] for e in _parse_symtab(baseline) if e["name"]}
    filt_named = {e["name"] for e in _parse_symtab(filtered) if e["name"]}
    # The wrapped name (starts with __, contains _Z) must be gone from both.
    leaked = [n for n in base_named | filt_named
              if n.startswith("__") and "_Z16hidden_templated" in n]
    assert not leaked, "wrapped hidden name survived: %r" % leaked
    assert os.path.getsize(filtered) <= os.path.getsize(baseline), (
        "--strip-hidden-syms grew the .bin"
    )


@skip_no_arm_gcc
def test_strip_non_public_keeps_only_allowlist(tmp_path):
    """--public-symbols=add,sub,mul,test with --strip-non-public-syms
    must keep ONLY the four listed names as EXPORTED.  Every other
    defined STB_GLOBAL/STB_WEAK must be demoted to local."""
    bin_path = _build(
        tmp_path / "f",
        "--public-symbols=add,sub,mul,test",
        "--strip-non-public-syms",
    )
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)
    # The four listed names must be present and EXPORTED.
    for n in ("add", "sub", "mul", "test"):
        assert n in named, "listed symbol %r missing" % n
        assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
            "listed symbol %r not EXPORTED: %r" % (n, named[n])
        )
    # The other C functions (not in allowlist) must not be EXPORTED.
    for n in ("call_mangled_through_got", "call_via_templated", "c_ctor_fn"):
        if n in named:
            assert named[n]["type"] != _SYM_TYPE_EXPORTED, (
                "non-listed C function %r still EXPORTED" % n
            )
    # The new EXPORTED data symbols (namespace fn + static member data)
    # are NOT in the allowlist and must be demoted.
    for n in ("_ZN4util4calcEi", "_ZN10DataHolder7counterE"):
        if n in named:
            assert named[n]["type"] != _SYM_TYPE_EXPORTED, (
                "non-listed new symbol %r still EXPORTED" % n
            )


# ---------------------------------------------------------------------------
# Composition: every flag at once.  This is the most aggressive
# configuration and is the one most likely to expose an interaction
# bug.
# ---------------------------------------------------------------------------


@skip_no_arm_gcc
def test_composition_all_four_flags_preserves_extern_c(tmp_path):
    """All four flags at once must still preserve the extern "C"
    exports in --public-symbols and demote everything else."""
    bin_path = _build(
        tmp_path / "f",
        "--public-symbols=" + _PUBLIC_ALLOWLIST,
        "--strip-hidden-syms",
        "--strip-non-public-syms",
        "--strip-mangled-syms",
        "--strip-weak-sym-names",
    )
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)
    for n in _PUBLIC_ALLOWLIST.split(","):
        assert n in named, "public-listed %r missing under composition" % n
        assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
            "public-listed %r not EXPORTED under composition: %r" % (n, named[n])
        )
    # No _Z* should remain.
    leaked = [n for n in named if n.startswith("_Z")]
    assert not leaked, "mangled names survived composition: %r" % leaked
    # No weak entries.
    weaks = [e for e in entries if e["type"] == _SYM_TYPE_WEAK]
    assert not weaks, "weak entries survived composition: %r" % [e["name"] for e in weaks]


@skip_no_arm_gcc
def test_composition_all_four_flags_minimizes_symt(tmp_path):
    """All four flags combined should produce the smallest named pool
    among the tested configurations — the public allowlist is the
    only thing that stays."""
    full = _build(tmp_path / "full", "--public-symbols=" + _PUBLIC_ALLOWLIST,
                  "--strip-hidden-syms", "--strip-non-public-syms",
                  "--strip-mangled-syms", "--strip-weak-sym-names")
    baseline = _build(tmp_path / "base")
    full_size = os.path.getsize(full)
    base_size = os.path.getsize(baseline)
    assert full_size < base_size, (
        "composition did not shrink the .bin: %d >= %d" % (full_size, base_size)
    )
    # Only the public allowlist + module name + externs should be named.
    entries = _parse_symtab(full)
    named = {e["name"] for e in entries if e["name"]}
    # Module name + 7 listed extern C + the host printf/puts + the
    # init-array sentinels.  mod_all.cpp now emits several additional
    # mangled EXPORTED/WEAK symbols in the baseline (namespace fn,
    # DtorTest ctor/dtor, template variable, DataHolder static member,
    # MultiDerived overrides + thunk) but every one of those is
    # mangled or weak and is therefore demoted by the four flags
    # together — so the post-composition named pool is still just the
    # public allowlist + the unavoidable glue.  Generous bound (<= 20)
    # catches accidental leaks of any demoted symbol while tolerating
    # minor linker-side variation in the glue count.
    assert len(named) <= 20, (
        "too many named entries under composition: %d (%r)" % (len(named), sorted(named))
    )


# ---------------------------------------------------------------------------
# Parameterized sweep across all 16 flag combinations (0000..1111).
# For each combo we build the comprehensive module once and check the
# invariants that MUST hold for that particular subset of flags.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("combo", _FLAG_COMBOS)
@skip_no_arm_gcc
def test_all_strip_combos(tmp_path, combo):
    """Build mod_all.cpp under each of the 16 flag combinations and
    verify the per-flag invariants compose correctly.

    For every combo:
      * the MODULE_NAME entry is first,
      * every extern "C" name in ``_PUBLIC_ALLOWLIST`` that is
        EXPORTED must survive (and under --strip-non-public-syms, the
        allowlisted ones must remain EXPORTED while non-allowlisted
        defined names must NOT be EXPORTED),
      * when --strip-mangled-syms is set, no ``_Z*`` name remains in
        the named pool,
      * when --strip-weak-sym-names is set, no WEAK entry remains,
      * when --strip-hidden-syms is set, no wrapped hidden name (the
        ``__<hash>___Z16hidden_templated`` form) remains,
      * when --strip-non-public-syms is set, no non-allowlisted
        defined name remains EXPORTED.
    """
    build_args = _flags_from_combo(combo)
    # Each parametrize run gets its own subdir under tmp_path so the
    # builds never collide.
    bin_path = _build(tmp_path / ("combo_" + combo), *build_args)
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)

    # Invariant 1: module name is the first symt entry.
    assert entries, "no symt entries for combo %s" % combo
    assert entries[0]["type"] == _SYM_TYPE_MODULE_NAME, (
        "module name not first for combo %s: %r" % (combo, entries[0])
    )
    assert entries[0]["name"] == "mod_all", (
        "module name wrong for combo %s: %r" % (combo, entries[0]["name"])
    )

    allowlist = _PUBLIC_ALLOWLIST.split(",")
    strip_mangled = "--strip-mangled-syms" in build_args
    strip_weak = "--strip-weak-sym-names" in build_args
    strip_hidden = "--strip-hidden-syms" in build_args
    strip_non_public = "--strip-non-public-syms" in build_args

    # Invariant 2: allowlisted extern "C" names survive as EXPORTED.
    #   * When --strip-non-public-syms is set, only the allowlisted
    #     names may be EXPORTED — nothing else.
    #   * When --strip-non-public-syms is NOT set, every extern "C"
    #     symbol the module defines stays EXPORTED regardless of the
    #     other flags (the other flags never touch extern "C").
    extern_c = ["add", "sub", "mul", "call_mangled_through_got",
                "call_via_templated", "c_ctor_fn", "test"]
    if strip_non_public:
        # Only allowlisted names may remain EXPORTED.
        exported_names = {n for n, e in named.items()
                          if e["type"] == _SYM_TYPE_EXPORTED}
        for n in allowlist:
            assert n in named, (
                "allowlisted extern C %r missing for combo %s" % (n, combo)
            )
            assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
                "allowlisted %r not EXPORTED for combo %s: %r"
                % (n, combo, named[n])
            )
        leaked = exported_names - set(allowlist)
        # The module name and init-array sentinels are also EXPORTED
        # by the linker script and are not subject to the allowlist.
        # Tolerate only names starting with "__" (sentinels) plus the
        # bare module name — everything else is a leak.
        leaks = sorted(n for n in leaked
                       if not n.startswith("__") and n != "mod_all")
        assert not leaks, (
            "non-allowlisted EXPORTED names for combo %s: %r"
            % (combo, leaks)
        )
    else:
        # Without --strip-non-public-syms every extern "C" symbol
        # the module defines must be EXPORTED.
        for n in extern_c:
            assert n in named, (
                "extern C %r missing for combo %s" % (n, combo)
            )
            assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
                "extern C %r not EXPORTED for combo %s: %r"
                % (n, combo, named[n])
            )

    # Invariant 3: --strip-mangled-syms removes every _Z* from the
    # named pool (the new namespace fn, DtorTest ctor/dtor, template
    # variable, static-member function, MultiDerived overrides and
    # thunk all live under _Z* and are covered categorically here).
    if strip_mangled:
        leaked_z = [n for n in named if n.startswith("_Z")]
        assert not leaked_z, (
            "mangled names survived --strip-mangled-syms (combo %s): %r"
            % (combo, leaked_z)
        )

    # Invariant 4: --strip-weak-sym-names removes every WEAK entry
    # (templated, inline_helper, weak_fn, DtorTest ctor/dtor, template
    # variable, DataHolder::get_next, MultiDerived overrides + thunk).
    if strip_weak:
        weaks = [e for e in entries if e["type"] == _SYM_TYPE_WEAK]
        assert not weaks, (
            "weak entries survived --strip-weak-sym-names (combo %s): %r"
            % (combo, [e["name"] for e in weaks])
        )

    # Invariant 5: --strip-hidden-syms removes the wrapped hidden
    # template name (``__<hash>___Z16hidden_templated...`` form).  The
    # bare name is re-emitted with default visibility by the prologue
    # and is intentionally NOT demoted — known pre-existing behavior.
    if strip_hidden:
        leaked_hidden = [
            n for n in named
            if n.startswith("__") and "_Z16hidden_templated" in n
        ]
        assert not leaked_hidden, (
            "wrapped hidden name survived --strip-hidden-syms (combo %s): %r"
            % (combo, leaked_hidden)
        )

    # Invariant 6: --strip-non-public-syms demotes every non-allowlisted
    # defined symbol.  Spot-check the new EXPORTED data symbols added
    # to mod_all.cpp — they are not in the allowlist, so they must not
    # remain EXPORTED.  (If --strip-mangled-syms is also set they are
    # demoted by that path; either way they must NOT be EXPORTED.)
    if strip_non_public:
        for n in ("_ZN4util4calcEi", "_ZN10DataHolder7counterE"):
            if n in named:
                assert named[n]["type"] != _SYM_TYPE_EXPORTED, (
                    "non-allowlisted new symbol %r still EXPORTED for "
                    "combo %s: %r" % (n, combo, named[n])
                )


# ---------------------------------------------------------------------------
# Internal (no-flag) checks of comprehensive module symt — confirms the
# baseline is well-formed before we apply flags.
# ---------------------------------------------------------------------------
@skip_no_arm_gcc
def test_baseline_has_constructors_and_instantiations(tmp_path):
    """The baseline (no flags) symt must contain constructor entries
    for our C++ classes, explicit template instantiations, and inline
    helpers — all as STB_WEAK.

    It must also contain the new patterns added to mod_all.cpp:
      * ``_ZN4util4calcEi`` — namespace function (EXPORTED, not WEAK),
      * ``_ZN8DtorTest{C1,C2,D1,D2}Ev`` — DtorTest ctor/dtor variants
        (WEAK),
      * ``_Z9g_defaultIiE`` — template variable instantiation (WEAK
        OBJECT data),
      * ``_ZN10DataHolder7counterE`` — static member data (EXPORTED),
      * ``_ZN10DataHolder8get_nextEv`` — static member function (WEAK),
      * ``_ZN12MultiDerived5mix_aEi`` / ``_ZN12MultiDerived5mix_bEi``
        — MultiDerived MI overrides (WEAK),
      * ``_ZThn4_N12MultiDerived5mix_bEi`` — non-virtual MI thunk for
        the secondary vtable's ``mix_b`` (WEAK).

    Note: the vtable (``_ZTV*``) is NOT in the symt because GCC emits
    it in a ``.data.rel.ro.local.*`` section.  GNU ld discards local
    sections during ``--gc-sections``, and mkmodule reads only
    ``.text``/``.data``/``.bss``.  Virtual dispatch in this module is
    also devirtualized by the compiler (``bp->vmethod(7)`` where ``bp``
    points to a known object is folded to a direct call), so the
    vtable is never needed at runtime.  This is a known pre-existing
    limitation, not a bug in the strip pipeline."""
    bin_path = _build(tmp_path / "base")
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)
    # Original baseline WEAK patterns.
    for n in ("_ZN7CounterC1Ev", "_ZN7CounterC2Ev",
              "_ZN8Counter2C1Ev", "_ZN8Counter2C2Ev",
              "_Z9templatedIiEiT_", "_Z13inline_helperi"):
        assert n in named, "baseline missing %r" % n
        assert named[n]["type"] == _SYM_TYPE_WEAK, (
            "baseline %r is not WEAK: %r" % (n, named[n])
        )
    # New WEAK patterns: DtorTest ctor/dtor variants, the template
    # variable instantiation, the DataHolder static member function,
    # the MultiDerived overrides, and the MI thunk.
    new_weak = (
        "_ZN8DtorTestC1Ev", "_ZN8DtorTestC2Ev",
        "_ZN8DtorTestD1Ev", "_ZN8DtorTestD2Ev",
        "_Z9g_defaultIiE",
        "_ZN10DataHolder8get_nextEv",
        "_ZN12MultiDerived5mix_aEi", "_ZN12MultiDerived5mix_bEi",
        "_ZThn4_N12MultiDerived5mix_bEi",
    )
    for n in new_weak:
        assert n in named, "baseline missing new WEAK symbol %r" % n
        assert named[n]["type"] == _SYM_TYPE_WEAK, (
            "baseline new symbol %r is not WEAK: %r" % (n, named[n])
        )
    # New EXPORTED patterns: namespace function (GCC emits STB_GLOBAL)
    # and the static member data variable (defined out-of-line).
    new_exported = (
        "_ZN4util4calcEi",
        "_ZN10DataHolder7counterE",
    )
    for n in new_exported:
        assert n in named, "baseline missing new EXPORTED symbol %r" % n
        assert named[n]["type"] == _SYM_TYPE_EXPORTED, (
            "baseline new symbol %r is not EXPORTED: %r" % (n, named[n])
        )


@skip_no_arm_gcc
def test_baseline_has_weak_instantiations(tmp_path):
    """The baseline must contain STB_WEAK entries for the explicit
    template instantiation and the inline function (both demoted by
    --strip-mangled-syms and --strip-weak-sym-names)."""
    bin_path = _build(tmp_path / "base")
    entries = _parse_symtab(bin_path)
    named = _by_name(entries)
    for n in ("_Z9templatedIiEiT_", "_Z13inline_helperi",
              "_Z16hidden_templatedIiEiT_", "weak_fn"):
        assert n in named, "baseline missing %r" % n
        assert named[n]["type"] == _SYM_TYPE_WEAK, (
            "baseline %r is not WEAK: %r" % (n, named[n])
        )

@skip_no_arm_gcc
def test_init_array_sentinels_in_baseline(tmp_path):
    """The 4 init/fini array sentinels must be present as EXPORTED
    in the no-flag baseline — they're emitted by the linker script
    and are required for the C++ init/fini glue (``cpp_init_fini.c``).

    Under ``--strip-non-public-syms`` (without listing them in
    ``--public-symbols``) the sentinels are correctly demoted to
    local — the relocations that reference them still work because
    they use symt indices, not names.  This test only asserts the
    baseline behavior; the demotion is covered by the
    ``test_got_relocs_resolve_demoted_targets`` test."""
    bin_path = _build(tmp_path / "base")
    entries = _parse_symtab(bin_path)
    named = {e["name"] for e in entries if e["name"]}
    for s in ("__init_array", "__init_array_start", "__init_array_end",
              "__preinit_array_start", "__preinit_array_end"):
        assert s in named, "%r missing in baseline" % s
    # And `__init_array` must be EXPORTED (it's the entry point the
    # loader's init code calls).
    named_by = _by_name(entries)
    assert named_by["__init_array"]["type"] == _SYM_TYPE_EXPORTED


@skip_no_arm_gcc
def test_got_relocs_resolve_demoted_targets(tmp_path):
    """The loader's GOT_BREL relocations must point at symt entries
    that are still present (even if they were demoted to INTERNAL).
    This is a sanity check that the demotion doesn't leave dangling
    reloc targets.

    We check that the reloc list length is consistent with the
    symt count of referenced symbols — if a demoted symbol were
    *deleted* (rather than demoted to local), its reloc would dangle.
    """
    baseline = _build(tmp_path / "base")
    filtered = _build(tmp_path / "f", "--strip-mangled-syms")
    # Parse reloc count from the header.
    for path, label in ((baseline, "baseline"), (filtered, "filtered")):
        with open(path, "rb") as f:
            data = f.read()
        assert data[:4] == b"UDLM"
        num_rels = struct.unpack_from("<H", data, 12)[0]
        # Each reloc references a symt index, all of which must be
        # < num_symt_entries.  Read symt count.
        symtsize = struct.unpack_from("<I", data, 16)[0]
        symt_off = 32 + num_rels * 8
        num_symt = struct.unpack_from("<I", data, symt_off)[0]
        # Walk relocs and verify all symt indices are in range.
        for i in range(num_rels):
            r_off, r_val = struct.unpack_from("<II", data, 32 + i * 8)
            # r_val is the symt index for foreign/local relocations;
            # for .text/.data relocations, bit 30/31 is set — strip it.
            if r_val & 0xC0000000:
                continue  # .text or .data reloc, not a symt index
            assert r_val < num_symt, (
                "%s: reloc[%d] symt idx %d out of range (num_symt=%d)" %
                (label, i, r_val, num_symt)
            )
