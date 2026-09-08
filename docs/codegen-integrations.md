# Writing Codegen Integrations

udynlink turns "domain artifact → loadable module" into a repeatable pattern.
Three integrations ship today:

| Integration | Input | Module exports | Host-side runtime |
|---|---|---|---|
| `scripts/proto2module` | `.proto` (nanopb) | `<pfx>_parse` / `<pfx>_write` per message | nanopb runtime (~10 KB), 4 externs |
| `scripts/mkwasm2c-module` | `.wasm` / `.wat` | wasm2c wrapper functions | wasm2c bare-metal runtime, import externs |
| `scripts/sm2module` | JSON state chart | `<pfx>_step` / `<pfx>_state_name` / `<pfx>_event_name` / `<pfx>_event_id` | none (self-contained) or action hooks |

This guide explains the pattern, when it fits, and how to write your own
integration on top of the shared helper (`scripts/codegen_common.py` — a
documented, stable-ish surface; changes are announced in this guide).
Read `scripts/sm2module` (~330 lines, no external tooling) as the reference
implementation.

## The pattern

A codegen integration compiles an artifact into three pieces:

1. **Data tables in the module** (`.rodata`/`.text`): the per-instance
   payload — a message schema, bytecode constants, a transition matrix.
   Position-independent by construction: plain byte arrays, no host pointers.
2. **A thin generated wrapper** exporting a fixed, prefixed C ABI
   (`<prefix>_parse`, `<prefix>_step`, ...). mkmodule wraps every export in
   an assembly prologue that preserves the caller's `r9` (see
   [How It Works](how-it-works.md) for the PIC model).
3. **A closed extern set**: runtime functions the module calls but does not
   define. The loader binds them by name via
   `udynlink_external_resolve_symbol()` at load time.

Plus **two contract artifacts** the pipeline emits so host and module can
never drift:

- a **host-side ABI header** (`<name>_api.h`, `<name>_contract.h`):
  prototypes for the module's exports and — for runtime-split modules — the
  hook prototypes the host must implement. Generated in lockstep with the
  module source, always from the same run.
- the **extern symbol list**, which is derivable from the module's symbol
  table (the QEMU tests assert it exactly).

### Two flavors

| | Runtime-split (proto, wasm) | Fully self-contained (sm2module without actions) |
|---|---|---|
| Module size | ~0.5 KB + schema | data + logic |
| Host integration | link the runtime, resolve N extern names | none beyond the loader |
| Use when | the runtime is ≥ a few KB (codec, VM) | the "runtime" is a few hundred bytes |

Self-contained modules have **zero externs**: nothing to wire, nothing to
resolve — `udynlink_load_module` is the entire host-side story. Prefer that
flavor whenever the shared logic is small; split the runtime out only when
its size would be duplicated per module.

## When the pattern fits (shape test)

1. The problem decomposes into **stable runtime + per-instance data**
   (codec ↔ schema, VM ↔ bytecode, matcher ↔ pattern, dispatch ↔ table).
2. The extern set is **small and closed** — every extern costs a LOT entry
   and a resolver hook. Data-only dependencies (tables, variables) also work
   (they land in `.data` relocations), but functions are simpler.
3. The ABI passes **data by pointer** and the struct layouts live in a
   generated header consumed by both sides from the same pipeline run.
   Layout skew (host rebuilt with different generator options) silently
   corrupts memory — the lockstep-generated header is the mitigation, and
   pinning the generator version (see
   [Protobuf Modules](protobuf-modules.md)) is the enforcement.
4. **No hidden state.** The module is a pure function set; mutable state
   lives in caller-owned structs or the host.

It does **not** fit when: the logic needs libc (modules link freestanding —
sm2module hand-rolls its `strcmp` for exactly this reason), the "data" is
not representable as position-independent tables (function-pointer tables
into host code work via the LOT but add extern surface), or the artifact
demands dynamic allocation (possible via `udynlink_external_malloc`, but
reconsider the split).

## Writing an integration

### 1. Choose the ABI and prefix

Export names default to `<artifact basename>_<verb>` — the basename prefix
is load-bearing: cross-module calls (the deps layer) resolve by bare name,
first match wins, so two codec modules must never export the same names.
Always offer `--export-prefix ''` for the single-module case, and validate
every generated identifier with `codegen_common.check_identifiers()`.

Keep the ABI pointer-based and unopinionated (`void *ctx` / caller-provided
buffers). Never `malloc` inside the module.

### 2. Generate tables that need no relocations

This is the one hard-won rule of the whole pattern: **emit data as bytes and
offsets, not as pointer arrays**. A `static const char *const names[]` in a
module compiles to `R_ARM_ABS32` relocations against anonymous section
symbols that mkmodule cannot encode (see the sm2module history: pointer
tables produced garbage LOT entries; the NUL-packed-blob + offset-table
design needs zero relocations). Concretely:

```c
/* GOOD: bytes + offsets, zero relocations */
static const char  names_blob[] = "closed\0open\0locked\0";
static const uint16_t name_offs[] = { 0, 7, 12 };
static const uint8_t  trans[3][3] = { ... };   /* 0xFF = none */

/* BAD: pointer array -> .data + R_ARM_ABS32 vs section symbols */
static const char *const names[] = { "closed", "open", "locked" };
```

Rule of thumb: the module's only relocations should be (a) the extern
function names and (b) named `static const` byte arrays that live in one
section. Verify with `scripts/udynlink_parser.py` after a build — every
`EXTERN` must be an intended extern and there should be **no** local-data
relocations against section symbols.

### 3. Build on `scripts/codegen_common.py`

```python
#!/usr/bin/env python3
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codegen_common as cc

# resolve external tools: flag > env var > pinned path > PATH
protoc = cc.find_tool(args.protoc, "MYTOOL", ["/opt/pinned/mytool", "mytool"],
                      "mytool not found; ...")
cc.check_identifiers(["my_parse", "my_write"], "export name")

# emit <name>_api.h / <name>_mod.c with your templates, then:
bin_path = cc.build_module(
    [wrapper, generated_c],
    public_symbols=["my_parse", "my_write"],
    strip_non_public=True,              # wrappers are the API; internals stay nameless
    build_flags="-I%s -I%s" % (inc_dir, out_dir),
    target=args.target, module_name=args.module_name,
    workdir=out_dir, gen_c_header=args.gen_c_header, header_path=out_dir,
    extra_args=user_args_after_dashdash,  # forwarded verbatim to mkmodule
    verbose=args.verbose)
cc.report_size(bin_path, args.module_name or "my_mod")
```

API summary (see the module docstrings for details):

| Function | Purpose |
|---|---|
| `find_tool(value, env, candidates, hint)` | tool resolution: flag → env var → pinned path/PATH |
| `sanitize_identifier(s)` / `check_identifiers([...])` | C-identifier hygiene |
| `api_header_guard(base)` | include-guard text |
| `run_tool(cmd, what, verbose)` | subprocess with loud failures |
| `build_module(sources, ...)` | mkmodule invocation; returns the `.bin` path; sorts `public_symbols`, honors `--bin-name` inside `extra_args` |
| `report_size(bin, name)` | image bytes + RAM per load mode (informational) |

CLI conventions to mirror (users learn one shape): positional artifact last;
`--out-dir`, `--export-prefix`, `--target`, `--gen-c-header`, `--verbose`;
everything after `--` forwarded to mkmodule. Validate the positional's
extension and fail with a hint about `--` — a stray unknown flag before it
misassigns argparse tokens (see proto2module/sm2module for the exact
wording).

### 4. Test it like the built-ins

Three layers, all present for each integration:

1. **Pipeline unit/integration tests** (`tests/py/test_<tool>.py`): parse
   the produced `.bin` with `udynlink_parser.parse_module` and assert — the
   export set, the exact extern set (empty for self-contained), and a size
   budget so per-module overhead cannot regress. Include the validation
   error paths (bad input, duplicate entries, stray flags).
2. **QEMU round-trip test** (`tests/test-<name>/`): commit the generated
   files so the test needs no codegen tooling; `test_data.py` lists sources
   and `--public-symbols`; the host loads in all three modes and round-trips
   real data. Copy `tests/test-statechart-module/` as the template.
3. **CI**: pytest runs on every push (`tests/py`), so nothing extra is
   needed as long as your tests skip cleanly without your external tools.

### 5. Host-integration notes (the r9 contract)

Module code is PIC: `r9` must hold the module's LOT base during execution.
`UDYNLINK_PREPARE_CALL(p_mod)` sets it — but `r9` is caller-owned, and
*any* host call in between (even `printf`, a symbol lookup, or a callback
into the host) may clobber it. Place `UDYNLINK_PREPARE_CALL(p_mod)`
**immediately before every module invocation**, not once at the top of the
caller:

```c
UDYNLINK_PREPARE_CALL(p_mod);
next = step(s, ev, &ctx);      /* correct */
printf("state: %s", ...);      /* host code may trash r9 */
UDYNLINK_PREPARE_CALL(p_mod);  /* re-arm before the next module call */
next = step(s, ev2, &ctx);
```

(The assembly prologue only *preserves* the caller's `r9` around the call;
it does not set it.) Host functions called *from* module code (action hooks)
are safe to return through: r9 is callee-saved in the host's ABI, so a hook
that clobbers r9 restores it on return. This cost is exactly what the
`udynlink_call.h` / `udynlink_thunk.h` layers automate — see
[Integrating as a Host](integrating-as-host.md).

## Candidate integrations

Patterns that fit the shape test today:

- **Self-contained**: compiled decision/rule tables, CRC/checksum codecs,
  1D/2D calibration lookups with interpolation, generated DSP control-law
  blocks, protocol framing state machines, regex → NFA tables + matcher.
- **Runtime-split**: Lua/MicroPython bytecode chunks (VM host-side once),
  device-description tables (Modbus maps, CAN DBCs) + a generic host
  accessor, compiled templates + a small interpreter, alternate codec
  stacks (CBOR/flatbuffers-style) reusing the proto pipeline shape.

When evaluating a new one, build the table generator first and check step 2
(zero section-symbol relocations) before writing any wrapper code — that is
where integrations succeed or die.
