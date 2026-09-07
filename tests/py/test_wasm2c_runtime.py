"""Host-native tests for the bare-metal wasm2c runtime (wasm-rt-udynlink.c).

Compiles the runtime with the host C compiler and drives wasm_rt_trap
control flow directly — the tiers QEMU cannot exercise:

- fatal tier: the handler is invoked with the trap code (the halting loop
  itself is unreachable in tests because the handler longjmps out);
- baked --trap-handler dispatch (the D3 regression: the macro path must
  actually reach the host-provided handler);
- recoverable traps: one-shot recovery semantics of wasm_rt_set_recovery
  (consumed by the first trap, NULL disarms, re-arm works).

The fatal-tier handler override lives in a separate TU that does not
include wasm-rt.h: the header's weak declaration would otherwise bleed
into the definition, and two weak symbols link in an unspecified order.
"""

import os
import shutil
import subprocess

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_RUNTIME_C = os.path.join(_REPO_ROOT, "udynlink", "wasm2c_runtime", "wasm-rt-udynlink.c")


def _find_cc():
    for cc in (os.environ.get("CC"), "cc", "gcc", "clang"):
        if cc and shutil.which(cc):
            return cc
    return None


_CC = _find_cc()
pytestmark = pytest.mark.skipif(_CC is None, reason="no host C compiler available")


HOST_PREAMBLE = """\
#include "wasm-rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

/* Host callbacks the runtime's weak hooks delegate to. */
void *udynlink_external_malloc(size_t size) { return malloc(size); }
void udynlink_external_free(void *p) { free(p); }
void udynlink_external_vprintf(const char *s, va_list va) { (void)s; (void)va; }
uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod,
                                           const char *name) {
    (void)p_mod; (void)name;
    return 0;
}
int udynlink_external_is_pointer_in_ram(const void *p) { (void)p; return 1; }
"""

# Strong override of the runtime's weak wasm_rt_trap_handler.  wasm_rt_trap_t
# is ABI-compatible with int (small enum).  The escape jmp_buf and the
# recorded code live here; the main TU reaches them via extern.
HANDLER_TU = """\
#include <setjmp.h>
#include <stdint.h>

typedef int wasm_rt_trap_t;  /* ABI-compatible with the runtime's enum */

jmp_buf g_handler_escape;
wasm_rt_trap_t g_handler_code;
int g_handler_called;

void wasm_rt_trap_handler(wasm_rt_trap_t code) {
    g_handler_code = code;
    g_handler_called = 1;
    longjmp(g_handler_escape, 1);
}
"""



def build_and_run(tmp_path, name, main_tu, extra_defs=(), use_handler_tu=False):
    srcs = []
    src = tmp_path / (name + "_main.c")
    src.write_text(HOST_PREAMBLE + main_tu)
    srcs.append(str(src))
    if use_handler_tu:
        hsrc = tmp_path / (name + "_handler.c")
        hsrc.write_text(HANDLER_TU)
        srcs.append(str(hsrc))
    binary = tmp_path / name
    cmd = [_CC,
           "-I", os.path.join(_REPO_ROOT, "udynlink"),
           "-I", os.path.join(_REPO_ROOT, "udynlink", "wasm2c_runtime")]
    for d in extra_defs:
        cmd.append("-D" + d)
    cmd += [_RUNTIME_C] + srcs + ["-o", str(binary)]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, res.stderr
    res = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, res.stdout + res.stderr
    return res.stdout

FATAL_MAIN = """\
extern jmp_buf g_handler_escape;
extern int g_handler_code;
extern int g_handler_called;
#define HANDLER_ESCAPE g_handler_escape
#define HANDLER_CODE ((wasm_rt_trap_t)g_handler_code)
int main(void) {
    if (setjmp(HANDLER_ESCAPE) == 0) {
        wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
        printf("FAIL: returned from wasm_rt_trap\\n");
        return 1;
    }
    if (HANDLER_CODE != WASM_RT_TRAP_UNREACHABLE) {
        printf("FAIL: handler got code %d\\n", HANDLER_CODE);
        return 1;
    }
    if (wasm_rt_last_trap() != WASM_RT_TRAP_UNREACHABLE) {
        printf("FAIL: wasm_rt_last_trap mismatch\\n");
        return 1;
    }
    printf("fatal-tier handler OK\\n");
    return 0;
}
"""


def test_fatal_tier_invokes_handler(tmp_path):
    out = build_and_run(tmp_path, "fatal", FATAL_MAIN, use_handler_tu=True)
    assert "fatal-tier handler OK" in out


BAKED_MAIN = """\
#include "wasm-rt.h"
#include <setjmp.h>

static jmp_buf g_escape;
static wasm_rt_trap_t g_handler_code = WASM_RT_TRAP_NONE;

/* Declared by wasm-rt.h via the WASM_RT_TRAP_HANDLER macro expansion.
 * The macro-dispatched handler is a plain strong symbol (no weak). */
void test_trap_handler(wasm_rt_trap_t code) {
    g_handler_code = code;
    longjmp(g_escape, 1);
}

int main(void) {
    if (setjmp(g_escape) == 0) {
        wasm_rt_trap(WASM_RT_TRAP_INT_OVERFLOW);
        printf("FAIL: returned from wasm_rt_trap\\n");
        return 1;
    }
    if (g_handler_code != WASM_RT_TRAP_INT_OVERFLOW) {
        printf("FAIL: baked handler got code %d\\n", (int)g_handler_code);
        return 1;
    }
    printf("baked handler OK\\n");
    return 0;
}
"""


def test_baked_trap_handler_macro_dispatch(tmp_path):
    """D3 regression: --trap-handler bakes WASM_RT_TRAP_HANDLER into the
    config; the runtime must call that symbol, not the weak hook."""
    out = build_and_run(tmp_path, "baked", BAKED_MAIN,
                        extra_defs=["WASM_RT_TRAP_HANDLER=test_trap_handler"])
    assert "baked handler OK" in out


RECOVERY_MAIN = """\
extern jmp_buf g_handler_escape;
extern int g_handler_code;
extern int g_handler_called;
#define HANDLER_ESCAPE g_handler_escape
#define HANDLER_CODE ((wasm_rt_trap_t)g_handler_code)

#include "wasm-rt.h"
#include <setjmp.h>

int main(void) {
    jmp_buf jb;

    /* 1. Registered recovery: the trap unwinds to the host frame. */
    if (setjmp(jb) == 0) {
        wasm_rt_set_recovery(&jb);
        wasm_rt_trap(WASM_RT_TRAP_DIV_BY_ZERO);
        printf("FAIL: returned from wasm_rt_trap\\n");
        return 1;
    }
    if (wasm_rt_last_trap() != WASM_RT_TRAP_DIV_BY_ZERO) {
        printf("FAIL: recovered with wrong code\\n");
        return 1;
    }
    printf("recovery OK\\n");

    /* 2. One-shot: the registration was consumed; the next trap must take
     * the fatal tier (handler escape), not the dead frame. */
    if (setjmp(HANDLER_ESCAPE) == 0) {
        wasm_rt_trap(WASM_RT_TRAP_OOB);
        printf("FAIL: returned from wasm_rt_trap\\n");
        return 1;
    }
    if (!g_handler_called || wasm_rt_last_trap() != WASM_RT_TRAP_OOB) {
        printf("FAIL: one-shot semantics\\n");
        return 1;
    }
    printf("one-shot OK\\n");

    /* 3. Re-arming works for another round. */
    if (setjmp(jb) == 0) {
        wasm_rt_set_recovery(&jb);
        wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
        printf("FAIL: returned from wasm_rt_trap\\n");
        return 1;
    }
    if (wasm_rt_last_trap() != WASM_RT_TRAP_UNREACHABLE) {
        printf("FAIL: re-armed recovery\\n");
        return 1;
    }
    printf("re-arm OK\\n");

    /* 4. wasm_rt_set_recovery(NULL) disarms early: fatal tier again. */
    wasm_rt_set_recovery(NULL);
    if (setjmp(HANDLER_ESCAPE) == 0) {
        wasm_rt_trap(WASM_RT_TRAP_UNALIGNED);
        printf("FAIL: disarmed recovery still unwound\\n");
        return 1;
    }
    printf("disarm OK\\n");
    return 0;
}
"""


def test_recovery_one_shot_semantics(tmp_path):
    out = build_and_run(tmp_path, "recover", RECOVERY_MAIN,
                        extra_defs=["WASM_RT_ENABLE_RECOVERY"], use_handler_tu=True)
    for line in ("recovery OK", "one-shot OK", "re-arm OK", "disarm OK"):
        assert line in out
