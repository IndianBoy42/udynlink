"""Host-native tests for the bare-metal wasm2c runtime (wasm-rt-udynlink.c).

Compiles the runtime with the host C compiler and drives wasm_rt_trap
control flow directly — the tiers QEMU cannot exercise:

- fatal tier: the handler is invoked with the trap code (the halting loop
  itself is unreachable in tests because the handler longjmps out);
- baked --trap-handler dispatch (the D3 regression: the macro path must
  actually reach the host-provided handler);
- recoverable traps: one-shot recovery semantics of wasm_rt_set_recovery
  (consumed by the first trap, NULL disarms, re-arm works);
- the realloc old-size contract (the D4 regression) with a guard page.

Weak-hook overrides live in separate TUs that do not include wasm-rt.h:
the header's weak declaration would otherwise bleed into the definition,
and two weak symbols link in an unspecified order.
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

/* Host callbacks the runtime's weak hooks delegate to (section = NULL: the
 * runtime owns no tagged sections; alignment 8 = widest thing it stores). */
void *udynlink_external_malloc(size_t size, const char *section,
                               size_t align, uint32_t flags) {
    (void)section; (void)align; (void)flags;
    return malloc(size);
}
void udynlink_external_free(void *p, const char *section,
                            size_t align, uint32_t flags) {
    (void)section; (void)align; (void)flags;
    free(p);
}
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

# Strong override of the runtime's weak wasm_rt_mem_free: the realloc test
# hands the runtime an mmap'd old buffer that must never reach free().
FREE_NOOP_TU = """\
#include <stddef.h>

void wasm_rt_mem_free(void *p) { (void)p; }
"""

HANDLER_MAIN_DECLS = """\
extern jmp_buf g_handler_escape;
extern int g_handler_code;
extern int g_handler_called;
#define HANDLER_ESCAPE g_handler_escape
#define HANDLER_CODE ((wasm_rt_trap_t)g_handler_code)
"""


def build_and_run(tmp_path, name, main_tu, extra_defs=(), extra_tus=()):
    srcs = []
    src = tmp_path / (name + "_main.c")
    src.write_text(HOST_PREAMBLE + main_tu)
    srcs.append(str(src))
    for i, content in enumerate(extra_tus):
        esrc = tmp_path / (name + "_tu%d.c" % i)
        esrc.write_text(content)
        srcs.append(str(esrc))
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


FATAL_MAIN = HANDLER_MAIN_DECLS + """\
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
    out = build_and_run(tmp_path, "fatal", FATAL_MAIN, extra_tus=[HANDLER_TU])
    assert "fatal-tier handler OK" in out


BAKED_MAIN = """\
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


RECOVERY_MAIN = HANDLER_MAIN_DECLS + """\
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
                        extra_defs=["WASM_RT_ENABLE_RECOVERY"],
                        extra_tus=[HANDLER_TU])
    for line in ("recovery OK", "one-shot OK", "re-arm OK", "disarm OK"):
        assert line in out


REALLOC_MAIN = """\
#include <sys/mman.h>
#include <unistd.h>

/* Allocate `usable` bytes ending exactly at a PROT_NONE guard page: any
 * read past the old buffer size faults instead of silently succeeding. */
static uint8_t *guarded_alloc(size_t usable) {
    long ps = sysconf(_SC_PAGESIZE);
    uint8_t *pages = mmap(NULL, 2 * (size_t)ps, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) { perror("mmap"); exit(1); }
    if (mprotect(pages + ps, (size_t)ps, PROT_NONE) != 0) { perror("mprotect"); exit(1); }
    return pages + ps - usable;
}

int main(void) {
    /* D4 regression: the realloc fallback must copy exactly old_size bytes
     * from the old buffer — the pre-fix code copied new_size and read past
     * the end (here: straight into the guard page). */
    uint8_t *old = guarded_alloc(16);
    for (int i = 0; i < 16; i++)
        old[i] = (uint8_t)(0xA0 ^ i);

    void *n = wasm_rt_mem_realloc(old, 16, 64);
    if (n == NULL) {
        printf("FAIL: realloc returned NULL\\n");
        return 1;
    }
    for (int i = 0; i < 16; i++) {
        if (((uint8_t *)n)[i] != (uint8_t)(0xA0 ^ i)) {
            printf("FAIL: contents not preserved at %d\\n", i);
            return 1;
        }
    }
    printf("realloc old-size contract OK\\n");
    return 0;
}
"""


def test_realloc_fallback_copies_exactly_old_size(tmp_path):
    """D4 regression at the TU level: growing a memory buffer must not read
    past the old allocation (guard page faults on any overread)."""
    out = build_and_run(tmp_path, "realloc", REALLOC_MAIN,
                        extra_tus=[FREE_NOOP_TU])
    assert "realloc old-size contract OK" in out
