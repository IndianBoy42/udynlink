# WebAssembly Modules (wasm2c)

`scripts/mkwasm2c-module` compiles a WebAssembly binary (or text) module into a
udynlink-loadable module. It runs [wabt](https://github.com/WebAssembly/wabt)'s
`wasm2c` on the input, generates a small bare-metal runtime configuration and an
export shim, and invokes `mkmodule` with the right PIC and conformance flags.

The result is an ordinary UDLM module: the host loads it with
`udynlink_load_module()` and calls exported wasm functions like plain C
functions. No interpreter, no guest stack — the wasm logic is AOT-compiled to
Cortex-M position-independent code.

```
foo.wat ──wat2wasm──▶ foo.wasm
                         │
                mkwasm2c-module  (pinned wabt, version-checked)
                         │
        ┌────────────────┼──────────────────────────┐
        ▼                ▼                          ▼
  wasm2c → foo.c/.h   wasm_rt_config.h          shim + export wrappers
                         │
              mkmodule (udynlink flags + wasm2c conformance flags)
                         │
              mod_foo.bin (+ optional mod_foo_module_data.h)
```

## Quick start

```bash
just setup-wabt        # installs the pinned wabt (wasm2c + wat2wasm) into tools/
python3 scripts/mkwasm2c-module --gen-c-header \
    --bin-name mod_calc.bin --header-path . calc.wat
```

On the host side:

```c
#include "udynlink.h"
#include "mod_calc_module_data.h"

udynlink_module_t mod;
udynlink_load_module(&mod, mod_calc_module_data, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);

udynlink_sym_t sym;
udynlink_lookup_symbol(&mod, "add", &sym);   /* wrapper named after the wasm export */
UDYNLINK_PREPARE_CALL(&mod);
int (*add)(int, int) = (int (*)(int, int))sym.val;
int r = add(30, 70);                          /* r9 is switched by the prologue */
```

By default the module's symbol table contains only the wrapper names (plus the
module-name entry); runtime internals and wasm2c glue are demoted to nameless
internal entries. Pass `--export-all` if you want the full symtab back for
debugging.

## Command-line reference

| Flag | Default | Meaning |
|---|---|---|
| `input` | — | `.wasm` or `.wat` file (`.wat` is assembled with the same pinned wabt) |
| `--target` / `--mcpu` | `cortex-m4` | Same target database as `mkmodule` |
| `-O` | `s` | Optimization level |
| `--bin-name` | `mod_<name>.bin` | Output image path |
| `--gen-c-header` / `--header-path` | off / `.` | Emit `<bin>_module_data.h` for `add_subdirectory`-style hosts |
| `--gen-imports-header` | auto with `--gen-c-header` | Emit `<bin>_imports.h`: the host-side symbol contract for import-bearing modules (below) |
| `--memory` | `auto` | Linear-memory model (below) |
| `--custom-page-size` | 65536 | Shrink the wasm page size (memory must be present; ignored for memory64) |
| `--trap-handler=NAME` | off | Call host-provided `void NAME(wasm_rt_trap_t)` before the fatal halt; the symbol is undefined in the module and resolved at load time |
| `--stack-depth-limit=N` | off | Count wasm call depth and trap `WASM_RT_TRAP_EXHAUSTION` beyond N nested calls instead of overflowing the native stack |
| `--malloc=NAME` / `--free=NAME` | udynlink externals | Redirect the runtime's allocation hooks |
| `--public-symbols` / `--export-all` | wrappers only | Symbol-table contents |
| `--wrapper-prefix=PFX` | none | Prefix generated wrapper names (collision escape hatch) |
| `--no-prologue` | off | Skip prologue wrappers (host must manage r9 itself) |
| `--workdir` / `--keep` | temp dir | Keep intermediates (`.c/.h/.o/.elf`, `wasm_rt_config.h`) for inspection |

wasm2c conformance flags (`-fno-optimize-sibling-calls -frounding-math
-fsignaling-nans`) are always added; `--no-conformance-flags` restores the old
(non-conformant) behavior.

## Linear-memory models

| `--memory=` | Mechanics | Host obligation | Notes |
|---|---|---|---|
| `static` *(default for non-growing modules)* | Buffer baked into module `.bss` (`WASM_RT_INITIAL_PAGES` × page size) | none — no allocator involved | `memory.grow` fails at runtime; RAM cost is visible in the module header (`bss_size`) |
| `dynamic` *(default when the module grows)* | Runtime allocates via `wasm_rt_malloc` hooks (default: `udynlink_external_malloc/free`) | host allocator | grow up to max_pages; realloc hook receives the old size |
| `external` | Host registers a buffer before the first call | call `mod_set_memory(void* buf, size_t capacity_bytes)` | capacity must cover initial pages; growth within the registered capacity needs no reallocation; `WASM_RT_TRAP_OOM` if unset or too small |

`auto` picks `static` unless the module contains `memory.grow`, in which case it
picks `dynamic`. Forcing `static` on a growing module is allowed but `memory.grow`
will fail at runtime (a warning is printed at build time).

Table (funcref/externref) allocations always go through the same hook family,
are zero-filled after allocation, and trap with `WASM_RT_TRAP_OOM` when the
allocator fails — an unfilled table slot is a null funcref, never garbage.

## Importing host functions (symbol contract)

Wasm imports become **undefined symbols** in the module; the udynlink loader
binds them at load time from host-provided functions named exactly as wasm2c
names them (`w2c_<env>_<name>`). No glue, no indirection, no per-import RAM.

For a module with imports the tool emits **`<bin>_imports.h`** (also
automatically with `--gen-c-header`):

```c
struct w2c_env { void* user; };

/* import: 'env' 'host_add' */
u32 w2c_env_host_add(struct w2c_env*, u32, u32);
```

The host firmware implements each listed function with exactly this signature
and returns its address from `udynlink_external_resolve_symbol()` (the
QEMU test's `test_resolve_symbol` is the example pattern). The first
parameter is the import environment: wasm2c leaves `struct w2c_env` opaque
and the generated shim defines it as a single context word, mirrored in the
imports header so host and module agree on the layout. The host attaches
per-module context through the exported `<mod>_set_env_user()` wrapper and
reads it as `((struct w2c_env*)env)->user` inside the import.

An import the host cannot resolve **fails the load** with
`UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` naming the symbol — a module missing its
dependencies never loads half-bound.

`mkhostsyms` synergy: feed the symbol names from `<bin>_imports.h` to
`scripts/mkhostsyms` to build an O(1) host-side resolution table.

## Trap policy

`wasm_rt_trap()` runs the policy in this order:

1. `--trap-handler=NAME` (if configured): calls the host-provided handler.
2. weak `wasm_rt_trap_handler` (host may override for logging).
3. fatal `bkpt` loop.

Traps are *not* recoverable in this release: a trapping module halts the core.
Recoverable traps (host-registered `longjmp` recovery points, opt-in baked
wrapper recovery) are planned — see
`.opencode/plans/wasm2c-to-udynlink.md` (Phase 2.3). If a module must not be
able to halt the system today, run it on a dedicated RTOS task or pin it behind
a watchdog.

## Runtime layout

| File | Role |
|---|---|
| `udynlink/wasm2c_runtime/wasm-rt.h` | Public runtime header; picks up `wasm_rt_config.h` via `__has_include`, so per-module settings reach every translation unit |
| `udynlink/wasm2c_runtime/wasm-rt-udynlink.c` | Bare-metal runtime: memory models, tables, traps, string builtins |

Both are compiled into the module; nothing is vendored into user projects. The
runtime has no libc dependency: `memcpy`/`memset`/`memmove`/`memcmp` are
provided as weak builtins-based implementations.

### `wasm_rt_config.h`

Generated per module into the workdir (kept by `--workdir`/`--keep`):

```c
#define WASM_RT_STATIC_MEMORY        /* or nothing (dynamic) / WASM_RT_EXTERNAL_MEMORY */
#define WASM_RT_INITIAL_PAGES 2
/* #define WASM_RT_PAGE_SIZE 1024            (--custom-page-size) */
/* #define WASM_RT_TRAP_HANDLER my_handler   (--trap-handler) */
/* #define WASM_RT_USE_STACK_DEPTH_COUNT 1   (--stack-depth-limit) */
/* #define WASM_RT_MAX_CALL_STACK_DEPTH 64 */
```

## Testing

The QEMU suite builds wasm modules *through the script* (see
`tests/test-wasm2c-add`, `-fac`, `-hello`, `-imports`): `tests/test_data.py` entries with a
`"wasm": "foo.wat"` field are handled by `tests/test_driver.py`, which invokes
`scripts/mkwasm2c-module` and then builds the host firmware as usual. Script
internals (config-header generation, export parsing, import parsing and the
generated symbol contract, collision checks, memory-mode selection,
header-path resolution) are covered by
`tests/py/test_mkwasm2c_module.py` (`just test-py`).

wabt is pinned (`just setup-wabt`): wasm2c's emitted-code shape is load-bearing
for the script's parsers, so `mkwasm2c-module` warns when the installed wasm2c
is older than the tested minimum.

## Limitations

- Traps are fatal-only (Phase 2.3 adds recovery).
- Multi-value/sret exports are rejected with a named error.
- memory64, SIMD, threads, exceptions, GC: out of scope.
- This is **not** a hostile-code sandbox: module code is native code that calls
  host imports with full privilege. The wasm memory bounds checks are a safety
  net for *mistakes*, not a defense against malice.
