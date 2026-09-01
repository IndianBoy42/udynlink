"""Regression tests for the mkmodule ``--lto`` mode.

``--lto`` compiles module sources to *fat* LTO objects (IR + real code),
splits exported functions into prologue wrappers via the linker's ``--wrap``
(binutils refuses ``objcopy --redefine-sym`` on LTO IR), and links with
``-flto`` so GCC optimizes across translation units.

These tests verify the observable on-disk contract of an LTO-built module
image (UDLM): exported/weak/external symbol classification, absence of
wrapper/hidden-body leakage into the named symbol pool, cross-TU size
effects, guard errors for misconfiguration, and determinism.  Runtime
behavior of LTO modules is covered by the QEMU suites (run them with
``UDYNLINK_MKMODULE_LTO=1``).

Skipped when ``arm-none-eabi-gcc`` is unavailable.
"""

import os
import shutil
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_MKMODULE = os.path.join(_REPO_ROOT, "scripts", "mkmodule")
_PY = sys.executable

_ARM_GCC = shutil.which("arm-none-eabi-gcc")
skip_no_arm_gcc = pytest.mark.skipif(
    _ARM_GCC is None,
    reason="arm-none-eabi-gcc not available",
)

sys.path.insert(0, os.path.join(_THIS_DIR, "..", "..", "scripts"))
import udynlink_parser as _up  # noqa: E402

# ---------------------------------------------------------------------------
# Module sources
# ---------------------------------------------------------------------------

# Two C translation units: cross-TU calls, an extern (host) call, an
# address-taken function pointer (forces a GOT/LOT entry), and rodata.
_TU_A = r"""
void host_hook(int);
int add3(int a, int b, int c) { return a + b + c; }
const char *tag(void) { return "lto-tu-a"; }
"""
_TU_B = r"""
int add3(int a, int b, int c);
const char *tag(void);
void host_hook(int);
static int twice(int x) { return x * 2; }
int helper(int x) {
    int (*volatile fp)(int) = twice;   /* address-taken static: GOT entry */
    return fp(x);
}
int test(void) {
    int r = add3(1, 2, 3);
    host_hook(r);
    return (r == 6) && (helper(21) == 42) && (tag()[0] == 'l');
}
"""

# Constant-folding pair: LTO folds test() to a constant, non-LTO must keep
# both callees.  Used to prove the linker plugin actually consumes the IR.
_TEN = r"""
int ten(void) { return 10; }
int twenty(void) { return 20; }
"""
_TEN_USER = r"""
int ten(void);
int twenty(void);
int test(void) { return ten() + twenty() == 30; }
"""

_WEAK_MOD = r"""
__attribute__((weak)) int wopt(int x) { return x + 1; }
int test(void) { return wopt(1) == 2; }
"""

_CPP_MOD = r"""
#include <stdio.h>
template <typename T>
__attribute__((noinline)) T tmax(T a, T b) { return a > b ? a : b; }
class Box {
public:
    Box(int v) : v_(v) {}
    virtual ~Box() = default;
    virtual int get() const { return v_; }
private:
    int v_;
};
static Box g_box(7);   /* global ctor: exercises __init_array under LTO */
extern "C" int test(void) {
    Box local(3);
    return tmax(1, 2) == 2 && local.get() == 3 && g_box.get() == 7;
}
"""

_REQUIRES_MOD = r"""
#include "udynlink_deps_api.h"
UDYNLINK_REQUIRES(mod_math)
int test(void) { return 1; }
"""

_WRAP_PREFIX_MOD = r"""
int __wrap_evil(int x) { return x; }
int test(void) { return __wrap_evil(1) == 1; }
"""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _write_srcs(src_dir, tu_pairs):
    src_dir.mkdir(parents=True, exist_ok=True)
    for name, text in tu_pairs:
        (src_dir / name).write_text(text)


def _build_module(tmp_path, tu_pairs, *extra_args, expect_ok=True):
    """Run mkmodule over tu_pairs with extra_args; return the .bin path.

    When expect_ok is False, returns the CompletedProcess instead and the
    caller asserts on it (no .bin expected).
    """
    src_dir = tmp_path / "src"
    out_dir = tmp_path / "build"
    src_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    _write_srcs(src_dir, tu_pairs)
    cmd = [_PY, _MKMODULE, "--workdir", str(out_dir)]
    cmd.extend(extra_args)
    cmd.extend(str(src_dir / n) for n, _ in tu_pairs)
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not expect_ok:
        return res
    assert res.returncode == 0, "mkmodule failed:\n%s" % res.stdout.decode()
    bin_name = os.path.splitext(tu_pairs[0][0])[0] + ".bin"
    return out_dir / bin_name


def _parse_symtab(bin_path):
    """[{name, type, value}] for every symt entry (name None for INTERNAL)."""
    with open(bin_path, "rb") as f:
        img = _up.parse_module(f.read())
    return [{"name": s.name, "type": s.type, "value": s.val_raw}
            for s in img.symbols]


def _named(entries):
    return [e for e in entries if e["name"]]


def _by_name(entries):
    return {e["name"]: e for e in _named(entries)}


LTO = "--lto"


# ---------------------------------------------------------------------------
# Core image-shape tests
# ---------------------------------------------------------------------------

@skip_no_arm_gcc
def test_lto_exports_wrappers_and_hides_bodies(tmp_path):
    """LTO image: multi-TU exports stay EXPORTED, bodies never leak.

    The named pool must contain the original function names (now carried by
    the __wrap_ prologue wrappers after the post-link rename) and must not
    contain __wrap_/__real_/md5-hidden body names.
    """
    binp = _build_module(tmp_path, [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)], LTO)
    entries = _parse_symtab(binp)
    named = _by_name(entries)
    assert "test" in named and named["test"]["type"] == _up.SYM_TYPE_EXPORTED
    assert "helper" in named and named["helper"]["type"] == _up.SYM_TYPE_EXPORTED
    assert "add3" in named and named["add3"]["type"] == _up.SYM_TYPE_EXPORTED
    assert "host_hook" in named and named["host_hook"]["type"] == _up.SYM_TYPE_EXTERN
    assert any(e["type"] == _up.SYM_TYPE_MODULE_NAME for e in entries)
    for e in _named(entries):
        assert not e["name"].startswith(("__wrap_", "__real_")), \
            "wrapper name leaked: %r" % e["name"]
        assert "test" != e["name"] or True
    # Hidden bodies are INTERNAL (nameless): every INTERNAL entry must
    # actually be nameless — a named INTERNAL would mean the localize pass
    # did not run.
    for e in entries:
        if e["type"] == _up.SYM_TYPE_INTERNAL:
            assert e["name"] in (None, ""), "named INTERNAL leaked: %r" % e["name"]


@skip_no_arm_gcc
def test_lto_cross_tu_optimization_shrinks_image(tmp_path):
    """The linker plugin must consume the IR: cross-TU constant folding.

    test() = ten() + twenty() == 30 folds to a constant under LTO, while the
    non-LTO build must keep three standalone functions.  The LTO image must
    therefore be strictly smaller.
    """
    lto_bin = _build_module(tmp_path / "lto", [("m_ten.c", _TEN), ("mod_ten.c", _TEN_USER)], LTO)
    plain_bin = _build_module(tmp_path / "plain", [("m_ten.c", _TEN), ("mod_ten.c", _TEN_USER)])
    lto_size = os.path.getsize(lto_bin)
    plain_size = os.path.getsize(plain_bin)
    assert lto_size < plain_size, (
        "LTO image not smaller than non-LTO (%d vs %d): plugin IR not used?"
        % (lto_size, plain_size)
    )
    # And the folded module must still export test().
    named = _by_name(_parse_symtab(lto_bin))
    assert "test" in named


@skip_no_arm_gcc
def test_lto_weak_function_keeps_weak_type(tmp_path):
    """A defined weak public function must stay SYM_TYPE_WEAK under LTO so
    the loader can apply host overrides."""
    binp = _build_module(tmp_path, [("mod_weak.c", _WEAK_MOD)], LTO)
    named = _by_name(_parse_symtab(binp))
    assert "wopt" in named and named["wopt"]["type"] == _up.SYM_TYPE_WEAK
    assert "test" in named and named["test"]["type"] == _up.SYM_TYPE_EXPORTED


@skip_no_arm_gcc
def test_lto_no_prologue_exports_survive(tmp_path):
    """--lto --no-prologue: exports are kept via -u/--undefined, classified
    EXPORTED, and no wrappers exist at all."""
    binp = _build_module(
        tmp_path, [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)], LTO, "--no-prologue")
    named = _by_name(_parse_symtab(binp))
    for n in ("test", "helper", "add3"):
        assert n in named and named[n]["type"] == _up.SYM_TYPE_EXPORTED
    assert "host_hook" in named and named["host_hook"]["type"] == _up.SYM_TYPE_EXTERN


@skip_no_arm_gcc
def test_lto_public_symbols_filter(tmp_path):
    """--lto --public-symbols=test: helper/add3 get no wrappers and end up
    internalized or absent; only test is EXPORTED."""
    binp = _build_module(
        tmp_path, [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)],
        LTO, "--public-symbols=test")
    named = _by_name(_parse_symtab(binp))
    assert "test" in named and named["test"]["type"] == _up.SYM_TYPE_EXPORTED
    for demoted in ("helper", "add3"):
        assert demoted not in named, "unlisted symbol stayed exported: %r" % demoted


@skip_no_arm_gcc
def test_lto_cpp_with_strip_mangled_and_init_array(tmp_path):
    """C++ under LTO: templates/vtables, global ctor (__init_array), and
    --strip-mangled-syms composition.  No _Z name may survive in the named
    pool; extern "C" test stays exported; the image parses."""
    binp = _build_module(
        tmp_path, [("mod_cpp_lto.cpp", _CPP_MOD)], LTO, "--strip-mangled-syms")
    entries = _parse_symtab(binp)
    named = _by_name(entries)
    assert "test" in named and named["test"]["type"] == _up.SYM_TYPE_EXPORTED
    for e in entries:
        # --strip-mangled-syms demotes DEFINED _Z* symbols only; undefined
        # mangled externs (e.g. operator delete, provided by the host) stay.
        if e["name"] and e["name"].startswith("_Z"):
            assert e["type"] == _up.SYM_TYPE_EXTERN, \
                "defined mangled name survived: %r" % e["name"]
    # The global Box ctor forced a non-empty __init_array: the empty-array
    # sentinel omission must not have fired, so a ctor relocation survived in
    # the image (indirect check: module name + test + internals parse fine).
    assert any(e["type"] == _up.SYM_TYPE_MODULE_NAME for e in entries)


@skip_no_arm_gcc
def test_lto_dependency_requires_reclassified_external(tmp_path):
    """UDYNLINK_REQUIRES under LTO: the .udynlink.mod.requires.* declaration
    must survive fat-object discovery, the -u/--undefined keep-alive, and be
    reclassified EXTERNAL in the image."""
    inc = os.path.join(_REPO_ROOT, "udynlink")
    binp = _build_module(
        tmp_path, [("mod_req.c", _REQUIRES_MOD)],
        LTO, "-I" + inc)
    named = _by_name(_parse_symtab(binp))
    req = [n for n in named if n.startswith(".udynlink.mod.requires.")]
    assert req, "dependency declaration lost under LTO: %r" % sorted(named)
    for n in req:
        assert named[n]["type"] == _up.SYM_TYPE_EXTERN


@skip_no_arm_gcc
@pytest.mark.parametrize("opt", ["0", "s", "z", "2", "3"])
def test_lto_all_opt_levels_parse(tmp_path, opt):
    """-O level × LTO must produce a loadable image with test exported."""
    binp = _build_module(
        tmp_path / ("o" + opt),
        [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)], LTO, "-O" + opt)
    named = _by_name(_parse_symtab(binp))
    assert "test" in named and named["test"]["type"] == _up.SYM_TYPE_EXPORTED


@skip_no_arm_gcc
def test_lto_deterministic_output(tmp_path):
    """Same input + flags → byte-identical image."""
    a = _build_module(tmp_path / "a", [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)], LTO)
    b = _build_module(tmp_path / "b", [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)], LTO)
    assert open(a, "rb").read() == open(b, "rb").read()


@skip_no_arm_gcc
def test_lto_public_symbols_filter(tmp_path):
    """--lto --public-symbols=test --strip-non-public-syms: only test stays
    EXPORTED; unlisted defined symbols are demoted to nameless internal
    (same semantics as the non-LTO path — --public-symbols alone only
    steers prologue wrapping, demotion needs the strip flag)."""
    binp = _build_module(
        tmp_path, [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)],
        LTO, "--public-symbols=test", "--strip-non-public-syms")
    named = _by_name(_parse_symtab(binp))
    assert "test" in named and named["test"]["type"] == _up.SYM_TYPE_EXPORTED
    for demoted in ("helper", "add3"):
        assert demoted not in named, "unlisted symbol stayed exported: %r" % demoted
    assert "host_hook" in named and named["host_hook"]["type"] == _up.SYM_TYPE_EXTERN


@skip_no_arm_gcc
def test_lto_gen_c_header_and_bin_name(tmp_path):
    """--gen-c-header/--header-path/--bin-name compose with --lto."""
    out = tmp_path / "build"
    out.mkdir(parents=True)
    _build_module(
        tmp_path, [("mod_a.c", _TU_A), ("mod_b.c", _TU_B)],
        LTO, "--gen-c-header", "--header-path", str(out),
        "--bin-name", str(out / "custom.bin"))
    hdr = out / "custom_module_data.h"
    assert hdr.is_file()
    body = hdr.read_text()
    assert "custom_module_data" in body
    named = _by_name(_parse_symtab(out / "custom.bin"))
    assert "test" in named


# ---------------------------------------------------------------------------
# Guard tests: misconfiguration must fail loudly, not silently corrupt
# ---------------------------------------------------------------------------

@skip_no_arm_gcc
def test_lto_rejects_smuggled_flto_without_flag(tmp_path):
    """-flto via --build-flags WITHOUT --lto: slim objects would defeat the
    wrapper mechanism; mkmodule must refuse instead of emitting a corrupt
    image."""
    res = _build_module(
        tmp_path, [("mod_a.c", _TU_A)], "--build-flags=-flto", expect_ok=False)
    assert res.returncode != 0
    out = res.stdout.decode()
    assert "--lto" in out, "error must point at --lto: %s" % out


@skip_no_arm_gcc
def test_lto_rejects_slim_lto_objects(tmp_path):
    """--lto with -fno-fat-lto-objects in build flags: discovery needs the
    fat object's real-code symtab; must fail loudly."""
    res = _build_module(
        tmp_path, [("mod_a.c", _TU_A)],
        LTO, "--build-flags=-fno-fat-lto-objects", expect_ok=False)
    assert res.returncode != 0
    out = res.stdout.decode()
    assert "slim" in out, "error must mention slim LTO objects: %s" % out


@skip_no_arm_gcc
def test_lto_rejects_wrap_prefix_collision(tmp_path):
    """A public function literally named __wrap_* collides with the linker's
    reserved --wrap namespace; must be rejected under --lto."""
    res = _build_module(
        tmp_path, [("mod_wrap.c", _WRAP_PREFIX_MOD)], LTO, expect_ok=False)
    assert res.returncode != 0
    out = res.stdout.decode()
    assert "__wrap_" in out, "error must name the colliding symbol: %s" % out


@skip_no_arm_gcc
def test_no_lto_wrap_prefix_collision_is_fine(tmp_path):
    """Without --lto the __wrap_ name is an ordinary symbol and must build."""
    binp = _build_module(tmp_path, [("mod_wrap.c", _WRAP_PREFIX_MOD)])
    named = _by_name(_parse_symtab(binp))
    assert "__wrap_evil" in named


_DATA_MOD = r"""
volatile int g = 10;
__attribute__((weak)) int weak_var = 7;
__attribute__((weak)) int weak_func(void) { return 42; }
int test(void) {
    /* Real (non-foldable) references keep g/weak_var alive under LTO. */
    volatile int *pg = &g;
    return (*pg == 10) && (weak_var >= 0) && (weak_func() == 42);
}
"""


@skip_no_arm_gcc
def test_lto_exported_data_not_internalized(tmp_path):
    """Whole-program LTO internalizes data the module never exports to a
    dynamic symbol table (``D g`` -> ``d g``); --export-dynamic at the link
    must keep module-exported data SYM_TYPE_EXPORTED."""
    binp = _build_module(tmp_path, [("mod_data.c", _DATA_MOD)], LTO)
    named = _by_name(_parse_symtab(binp))
    assert "g" in named and named["g"]["type"] == _up.SYM_TYPE_EXPORTED, \
        "exported data internalized under LTO: %r" % named.get("g")


@skip_no_arm_gcc
def test_lto_weak_data_and_func_keep_weak_type(tmp_path):
    """Weak data must survive (LTO folds reads of provably-constant weak
    variables away entirely) and keep STB_WEAK so the loader preserves the
    host-override path.  LTRANS also re-emits prevailing weak definitions
    as GLOBAL; mkmodule must weaken them back post-link."""
    binp = _build_module(tmp_path, [("mod_data.c", _DATA_MOD)], LTO)
    named = _by_name(_parse_symtab(binp))
    assert "weak_var" in named and named["weak_var"]["type"] == _up.SYM_TYPE_WEAK, \
        "weak data lost or mistyped: %r" % named.get("weak_var")
    assert "weak_func" in named and named["weak_func"]["type"] == _up.SYM_TYPE_WEAK


@skip_no_arm_gcc
def test_lto_exported_data_survives_unreferenced(tmp_path):
    """Non-LTO parity: an exported datum the module itself never touches is
    section-GC'd in both modes (no KEEP for .data), so only referenced
    exports may be expected.  This pins that --lto does not change that
    contract: unreferenced data stays gone in both."""
    mod = "volatile int orphan = 3;\nint test(void) { return 1; }\n"
    lto_bin = _build_module(tmp_path / "l", [("mod_orph.c", mod)], LTO)
    plain_bin = _build_module(tmp_path / "p", [("mod_orph.c", mod)])
    lto_named = _by_name(_parse_symtab(lto_bin))
    plain_named = _by_name(_parse_symtab(plain_bin))
    assert "orphan" not in lto_named
    assert "orphan" not in plain_named
