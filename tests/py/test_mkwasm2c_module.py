"""Unit tests for scripts/mkwasm2c-module (wasm2c plan Phases 2.1+).

These defend the script's codegen decisions without invoking the ARM
toolchain: config-header generation per flag combination (the D1/D3
regressions), export parsing including pointer returns and loud sret
rejection (D6), import parsing and the generated host-side symbol
contract (K2), export-name collision rejection, memory-mode selection,
and the --header-path resolution order (D8, exercised through the real
script and skipped when wabt/arm-gcc are unavailable).
"""

import argparse
import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_SCRIPT = os.path.join(_REPO_ROOT, "scripts", "mkwasm2c-module")

_loader = importlib.machinery.SourceFileLoader("mkwasm2c_module", _SCRIPT)
_spec = importlib.util.spec_from_loader("mkwasm2c_module", _loader)
mkwasm = importlib.util.module_from_spec(_spec)
_loader.exec_module(mkwasm)


# ---------------------------------------------------------------------------
# Fixtures / helpers
# ---------------------------------------------------------------------------

MEM = {"has_memory": True, "has_grow": False, "initial_pages": 2,
       "max_pages": 65536, "is64": False}
MEM_GROW = dict(MEM, has_grow=True)
NOMEM = {"has_memory": False, "has_grow": False, "initial_pages": 0,
         "max_pages": 0, "is64": False}


def make_args(**kw):
    """Args namespace mirroring the script's parser defaults."""
    defaults = dict(memory="auto", custom_page_size=None, trap_handler=None,
                    stack_depth_limit=None, malloc=None, free=None,
                    wrapper_prefix="", public_symbols=None, export_all=False,
                    no_export_wrappers=False, build_flags=None,
                    no_conformance_flags=False, no_verbose=True, no_debug=True)
    defaults.update(kw)
    return argparse.Namespace(**defaults)


def gen_config(tmp_path, mode, mem, **kw):
    path = tmp_path / "wasm_rt_config.h"
    mkwasm.generate_config_header(str(path), mode, mem, make_args(**kw))
    return path.read_text()


SAMPLE_HEADER = """\
/* export: 'add' */
u32 w2c_add_add(w2c_add* instance, u32, u32);

/* export: 'memory' */
wasm_rt_memory_t* w2c_add_memory(w2c_add* instance);
"""

SRET_HEADER = """\
/* export: 'multi' */
void w2c_add_multi(w2c_add* instance, w2c_add_multi_ret_t* ret, u32);
"""


def write_header(tmp_path, text=SAMPLE_HEADER):
    h = tmp_path / "add.h"
    h.write_text(text)
    return str(h)


# ---------------------------------------------------------------------------
# Config header (D1/D3): defines must be generated for every mode/flag
# ---------------------------------------------------------------------------

def test_config_static_mode(tmp_path):
    text = gen_config(tmp_path, "static", MEM)
    assert "#define WASM_RT_STATIC_MEMORY" in text
    assert "#define WASM_RT_INITIAL_PAGES 2" in text


def test_config_dynamic_mode_has_no_memory_defines(tmp_path):
    text = gen_config(tmp_path, "dynamic", MEM_GROW)
    assert "WASM_RT_STATIC_MEMORY" not in text
    assert "WASM_RT_EXTERNAL_MEMORY" not in text


def test_config_external_mode(tmp_path):
    text = gen_config(tmp_path, "external", MEM)
    assert "#define WASM_RT_EXTERNAL_MEMORY" in text


def test_config_none_mode(tmp_path):
    text = gen_config(tmp_path, "none", NOMEM)
    assert "WASM_RT_STATIC_MEMORY" not in text
    assert "WASM_RT_EXTERNAL_MEMORY" not in text


def test_config_custom_page_size(tmp_path):
    text = gen_config(tmp_path, "static", MEM, custom_page_size=1024)
    assert "#define WASM_RT_PAGE_SIZE 1024" in text


def test_config_custom_page_size_requires_memory(tmp_path):
    text = gen_config(tmp_path, "none", NOMEM, custom_page_size=1024)
    assert "WASM_RT_PAGE_SIZE" not in text


def test_config_trap_handler_is_host_resolved(tmp_path):
    text = gen_config(tmp_path, "none", NOMEM, trap_handler="my_trap_handler")
    assert "#define WASM_RT_TRAP_HANDLER my_trap_handler" in text
    assert "resolved at module load time" in text


def test_config_stack_depth(tmp_path):
    text = gen_config(tmp_path, "none", NOMEM, stack_depth_limit=64)
    assert "#define WASM_RT_USE_STACK_DEPTH_COUNT 1" in text
    assert "#define WASM_RT_MAX_CALL_STACK_DEPTH 64" in text


# ---------------------------------------------------------------------------
# Memory-mode selection (D1 auto-refinement)
# ---------------------------------------------------------------------------

def test_mode_auto_static_without_grow():
    assert mkwasm.resolve_memory_mode(make_args(memory="auto"), MEM) == "static"


def test_mode_auto_dynamic_with_grow():
    assert mkwasm.resolve_memory_mode(make_args(memory="auto"), MEM_GROW) == "dynamic"


def test_mode_auto_none_without_memory():
    assert mkwasm.resolve_memory_mode(make_args(memory="auto"), NOMEM) == "none"


def test_mode_forced_static_with_grow_warns(capfd):
    assert mkwasm.resolve_memory_mode(make_args(memory="static"), MEM_GROW) == "static"
    assert "cannot grow" in capfd.readouterr().err


def test_mode_forced_dynamic():
    assert mkwasm.resolve_memory_mode(make_args(memory="dynamic"), MEM) == "dynamic"


def test_mode_external_without_memory_warns(capfd):
    assert mkwasm.resolve_memory_mode(make_args(memory="external"), NOMEM) == "none"
    assert "no linear memory" in capfd.readouterr().err


# ---------------------------------------------------------------------------
# Export parsing (D6): pointer returns, sret rejection, unmatched warnings
# ---------------------------------------------------------------------------

def test_parse_exports_scalar_and_pointer_return(tmp_path):
    exports = {e["wasm_name"]: e for e in mkwasm.parse_exports(write_header(tmp_path))}
    assert exports["add"]["ret"] == "u32"
    # params keep their emitted text; the instance param is dropped by the wrapper generator
    assert exports["add"]["params"] == ["w2c_add* instance", "u32", "u32"]
    # Pointer-returning export (memory) must be parsed, not silently dropped.
    assert exports["memory"]["ret"] == "wasm_rt_memory_t*"


def test_parse_exports_rejects_sret_loudly(tmp_path):
    with pytest.raises(SystemExit):
        mkwasm.parse_exports(write_header(tmp_path, SRET_HEADER))


def test_parse_exports_warns_on_unrecognized_export(tmp_path, capfd):
    text = ("/* export: 'g' */\n"
            "extern const u32 w2c_x_g;\n")
    exports = mkwasm.parse_exports(write_header(tmp_path, text))
    assert exports == []
    assert "not be wrapped" in capfd.readouterr().err


# ---------------------------------------------------------------------------
# Collision detection: wrapper names must not shadow runtime/libc symbols
# ---------------------------------------------------------------------------

def _one_export(name):
    return [{"wasm_name": name, "c_name": "w2c_x_" + name,
             "ret": "u32", "params": ["w2c_x*"]}]


def test_collision_rejects_libc_name():
    with pytest.raises(SystemExit):
        mkwasm.check_export_collisions(_one_export("memcpy"), "")


def test_collision_rejects_runtime_prefix():
    with pytest.raises(SystemExit):
        mkwasm.check_export_collisions(_one_export("wasm_rt_foo"), "")


def test_collision_escape_hatch_prefix():
    mkwasm.check_export_collisions(_one_export("memcpy"), "w_")


# ---------------------------------------------------------------------------
# Bare-metal patching of generated code
# ---------------------------------------------------------------------------

def test_patch_adds_ndebug_and_builtin_memfuncs(tmp_path):
    c = tmp_path / "add.c"
    c.write_text("#include <string.h>\n#include <math.h>\nvoid f(void) {}\n")
    mkwasm.patch_wasm2c_generated(str(c), make_args())
    text = c.read_text()
    assert text.startswith("#define NDEBUG")
    assert "#define memcpy __builtin_memcpy" in text
    assert "#include <math.h>" in text  # kept; --gc-sections drops unused parts


# ---------------------------------------------------------------------------
# D8 regression: --header-path must resolve before the workdir chdir
# ---------------------------------------------------------------------------

def _tools_available():
    return all(shutil.which(t) for t in ("wasm2c", "wat2wasm", "arm-none-eabi-gcc"))


@pytest.mark.skipif(not _tools_available(), reason="wabt / arm-none-eabi-gcc not installed")
def test_header_path_lands_outside_workdir(tmp_path):
    wat = tmp_path / "add.wat"
    wat.write_text('(module (func (export "add") (param i32 i32) (result i32) '
                   "local.get 0 local.get 1 i32.add))")
    outdir = tmp_path / "out"
    outdir.mkdir()
    res = subprocess.run(
        [sys.executable, _SCRIPT, "--gen-c-header", "--header-path", str(outdir),
         "--bin-name", str(outdir / "mod_add.bin"), str(wat)],
        capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stderr
    # Pre-fix this file landed in the (deleted) temp workdir instead.
    assert (outdir / "mod_add_module_data.h").is_file()
    assert (outdir / "mod_add.bin").is_file()


# ---------------------------------------------------------------------------
# Imports (K2 symbol contract): parsing, generated host header, shim env
# ---------------------------------------------------------------------------

IMPORT_HEADER = """\
/* import: 'env' 'host_add' */
u32 w2c_env_host_add(struct w2c_env*, u32, u32);
"""


def test_parse_imports_full_contract(tmp_path):
    imports = mkwasm.parse_imports(write_header(tmp_path, IMPORT_HEADER))
    assert len(imports) == 1
    imp = imports[0]
    assert imp["module"] == "env"
    assert imp["name"] == "host_add"
    assert imp["c_name"] == "w2c_env_host_add"
    # The prototype is emitted verbatim in the generated host header; the
    # leading env parameter must survive it (that is how the host receives
    # its per-module context pointer).
    assert imp["proto"] == "u32 w2c_env_host_add(struct w2c_env*, u32, u32)"


def test_parse_imports_warns_on_unrecognized(tmp_path, capfd):
    text = "/* import: 'env' 'weird' */\nextern const u32 w2c_env_weird;\n"
    assert mkwasm.parse_imports(write_header(tmp_path, text)) == []
    assert "not recognized" in capfd.readouterr().err


def _shim(tmp_path, name, exports, imports):
    mem = {"has_memory": False, "has_grow": False, "initial_pages": 0,
           "max_pages": 0, "is64": False}
    shim = str(tmp_path / "shim.c")
    mkwasm.generate_shim(shim, name, name + ".h", mem, exports, imports,
                         make_args(), "none")
    with open(shim) as f:
        return f.read()


def test_shim_with_imports_defines_env_and_two_arg_instantiate(tmp_path):
    imports = [{"module": "env", "name": "host_add",
                "c_name": "w2c_env_host_add",
                "proto": "u32 w2c_env_host_add(struct w2c_env*, u32, u32)"}]
    exports = [{"wasm_name": "calc", "c_name": "w2c_calc_calc",
                "ret": "u32", "params": ["w2c_calc*", "u32", "u32"]}]
    text = _shim(tmp_path, "calc", exports, imports)
    # The shim is the wasm2c embedder: it defines the env struct + instance
    # and passes both to instantiate; the host gets a context setter.
    assert "struct w2c_env { void* user; };" in text
    assert "static struct w2c_env __wasm_env;" in text
    assert "void calc_set_env_user(void* user)" in text
    assert "wasm2c_calc_instantiate(&__wasm_instance, &__wasm_env);" in text


def test_shim_without_imports_keeps_single_arg_instantiate(tmp_path):
    exports = [{"wasm_name": "add", "c_name": "w2c_add_add",
                "ret": "u32", "params": ["w2c_add*", "u32", "u32"]}]
    text = _shim(tmp_path, "add", exports, [])
    assert "wasm2c_add_instantiate(&__wasm_instance);" in text
    assert "w2c_env" not in text


@pytest.mark.skipif(not _tools_available(), reason="wabt / arm-none-eabi-gcc not installed")
def test_imports_header_generation_end_to_end(tmp_path):
    wat = tmp_path / "calc.wat"
    wat.write_text(
        '(module (import "env" "host_add" (func $host_add (param i32 i32) (result i32)))'
        ' (func (export "calc") (param i32 i32) (result i32)'
        ' local.get 0 local.get 1 i32.mul local.get 1 i32.const 1 i32.add'
        ' call $host_add))')
    outdir = tmp_path / "out"
    outdir.mkdir()
    res = subprocess.run(
        [sys.executable, _SCRIPT, "--gen-c-header", "--header-path", str(outdir),
         "--bin-name", str(outdir / "mod_calc.bin"), str(wat)],
        capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stderr
    # --gen-c-header auto-emits the imports contract for import-bearing
    # modules: prototypes the host implements + the env struct it shares
    # with the shim.
    imports_h = outdir / "mod_calc_imports.h"
    assert imports_h.is_file()
    text = imports_h.read_text()
    assert "u32 w2c_env_host_add(struct w2c_env*, u32, u32);" in text
    assert "struct w2c_env { void* user; };" in text
