# Protobuf Modules (proto → UDLM)

udynlink can run protobuf codecs as loadable modules: a `.proto` file is
compiled into a standalone UDLM image exporting a `parse` and a `write`
function for one (or more) message types. Because modules are small, the
natural deployment is **1 module == 1 struct**: the host loads only the
codecs for the messages it actually uses, at runtime.

## Module ABI

Every module built by `scripts/proto2module` exports one function pair per
selected message. The names are prefixed with the `.proto` basename by
default so several codec modules can be loaded together without colliding
(`sensor.proto` exports `sensor_parse`/`sensor_write`; pass
`--export-prefix ''` for the bare `parse`/`write`):

```c
/* Decode `in_len` bytes at `in` into a caller-provided struct. 1 on success. */
int sensor_parse(const unsigned char *in, size_t in_len, void *msg);

/* Encode `msg` into `out`; *out_len is capacity in, bytes written out. 1 on success. */
int sensor_write(const void *msg, unsigned char *out, size_t *out_len);
```

By default the module also **preallocates cross-module thunks** for its
exports — an 18-byte gateway slot plus one 10-byte stub slot per exported
function in module `.bss`. When the module is loaded through
`udynlink_dep_load()`, the deps layer fills the slots at load time, so other
modules can call the codecs through `udynlink_dep_resolve_func()` with no
dynamic-pool allocation. Host-side calls via `udynlink_lookup_symbol()` +
`UDYNLINK_CALL` are unaffected. `--plain-exports` reverts to plain functions
only (40 bytes less XIP RAM for a two-function module).

Division of ownership:

| Piece | Lives in |
|---|---|
| Message struct layout (`<proto>.pb.h`) | Host (and module at compile time) |
| Message schema / field tables (`<Msg>_msg`, `_field_info`, `_submsg_info`) | **Module** (`.rodata`/`.data`, XIP-able) |
| nanopb runtime (`pb_decode`, `pb_encode`, `pb_istream_from_buffer`, `pb_ostream_from_buffer`) | Host firmware, exported once |

The host allocates the struct (it includes the generated header), the module
owns the schema. The nanopb runtime is linked into the host firmware once and
resolved by name at module load time — bundling it per module costs ~9 KB
each (see [Overhead](#overhead)).

## Building a Module

Requires `protoc`, the nanopb generator plugin, the vendored nanopb runtime
(`third_party/nanopb`), and the usual `arm-none-eabi-gcc`. Tools are located
via `--protoc` / `--plugin` / `--nanopb-dir` flags or the `UDYNLINK_PROTOC` /
`UDYNLINK_NANOPB_PLUGIN` / `UDYNLINK_NANOPB_DIR` environment variables.

**Keep the generator pinned to the vendored runtime version.** Install the
plugin with `uv pip install nanopb==0.4.9.*` (the vendored runtime is
nanopb-0.4.9.1). The generated codec depends on both the plugin and the
runtime version; mixing a newer generator with the older vendored runtime
changes the emitted field tables and struct layouts. The generated files
committed under `tests/test-protobuf-module/` regenerate byte-identically at
the pinned version — a quick way to check your toolchain pairing:

```bash
python3 scripts/proto2module --struct SensorReading \
    tests/test-protobuf-module/sensor.proto --out-dir /tmp/proto-check
diff /tmp/proto-check/sensor.pb.c tests/test-protobuf-module/sensor.pb.c
```

```bash
# 1 module == 1 struct (the default deployment); exports are prefixed with
# the .proto basename: sensor_parse / sensor_write
just proto2module --struct SensorReading proto/sensor.proto

# equivalent long form
python3 scripts/proto2module --struct SensorReading proto/sensor.proto

# several structs in one module (exports <pfx>_parse_<name>/<pfx>_write_<name>)
python3 scripts/proto2module --struct Alpha --struct Beta proto/multi.proto

# bare parse/write names (single codec module, host-side calls only)
python3 scripts/proto2module --struct SensorReading --export-prefix '' proto/sensor.proto

# custom prefix (overrides the basename default)
python3 scripts/proto2module --struct SensorReading --export-prefix acme proto/sensor.proto

# plain function exports only (skips the preallocated cross-module thunks)
python3 scripts/proto2module --struct SensorReading --plain-exports proto/sensor.proto

# with an embeddable C array header for the host firmware
python3 scripts/proto2module --struct SensorReading --gen-c-header proto/sensor.proto
```

Outputs, next to the `.proto` (or in `--out-dir`):

| File | Purpose |
|---|---|
| `<proto>.bin` (actually `<proto>_mod.bin`) | The UDLM image |
| `<proto>.pb.c` / `<proto>.pb.h` | nanopb-generated codec + struct definition (host includes the `.pb.h`) |
| `<proto>_mod.c` | Generated `parse`/`write` wrapper + thunk declarations (the module source) |
| `<proto>_api.h` | Host-side prototypes: one `parse`/`write` pair per selected message |

`--struct` takes the nanopb C type name: `package` + `Message` joined with
`_` (e.g. `acme_sensor_TempReading`). If the file has exactly one message,
`--struct` is optional. A file with several messages without `--struct` is
an error. Extra arguments are forwarded to `mkmodule`; put them after `--`
for unambiguous parsing:

```bash
python3 scripts/proto2module --struct SensorReading proto/sensor.proto -- -O s
```

A stray unknown flag placed *between* the options and the `.proto`
(e.g. `--struct X -O s sensor.proto`) makes argparse misassign tokens; the
pipeline detects this and exits with a hint pointing at the `--` form
(flags placed after the `.proto` still forward fine, but `--` never
ambiguates).

The output directory doubles as the mkmodule workdir, so intermediate files
(`*.o`, `*.elf`, `*_prologue.o`) are also written there (alongside the
artifacts listed above). They are safe to delete; a firmware tree's
`.gitignore` typically already covers `*.o`/`*.bin`/`*.elf`. Pass
`-- --workdir <dir>` to move the intermediates (and the `.bin`) elsewhere.

## Host Integration

1. Link `pb_common.c`, `pb_encode.c`, `pb_decode.c` into the host firmware
   and resolve the four runtime names in `udynlink_external_resolve_symbol()`
   (the QEMU test host does exactly this in
   `tests/test-protobuf-module/test_qemu.c`):

```c
#include <pb_decode.h>
#include <pb_encode.h>

uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod,
                                           const char *name) {
    if (!strcmp(name, "pb_decode"))              return (uintptr_t)&pb_decode;
    if (!strcmp(name, "pb_encode"))              return (uintptr_t)&pb_encode;
    if (!strcmp(name, "pb_istream_from_buffer")) return (uintptr_t)&pb_istream_from_buffer;
    if (!strcmp(name, "pb_ostream_from_buffer")) return (uintptr_t)&pb_ostream_from_buffer;
    return 0; /* ... host's other symbols ... */
}
```

2. Load, look up, call:

```c
#include "sensor.pb.h"            /* struct definition */
#include "sensor_mod_module_data.h" /* UDLM image (--gen-c-header) */

udynlink_module_t mod;
udynlink_load_module(&mod, sensor_mod_module_data, NULL, 0,
                     UDYNLINK_LOAD_MODE_XIP);

udynlink_sym_t sym;
udynlink_lookup_symbol(&mod, "sensor_parse", &sym);
int (*parse)(const unsigned char *, size_t, void *) =
    (int (*)(const unsigned char *, size_t, void *))sym.val;

SensorReading msg;
uint8_t buf[128];
size_t len = sizeof(buf);
UDYNLINK_PREPARE_CALL(&mod);
parse(buf, len, &msg);
```

The generated `<proto>_api.h` declares the prototypes; the host must not
define `sensor_parse`/`sensor_write` itself (they exist only inside the
module).

**Calling a codec module from another module** (e.g., a dispatcher that routes
messages to per-struct codec modules) requires a cross-module trampoline:
the caller and callee have different `r9` (LOT base) values, so a bare
function pointer would corrupt PIC state. Declare
`UDYNLINK_REQUIRES(<module_name>)` in the calling module and have the host's
resolver use `udynlink_dep_resolve_func()`, which switches `r9` around the
call. Codec modules make this cheap by default: their exports are
**preallocated thunk exports**, so when the module is loaded via
`udynlink_dep_load()` the deps layer fills the module's own gateway/stub
slots and `udynlink_dep_resolve_func()` serves them with zero dynamic-pool
allocation. Modules built with `--plain-exports` — or codec modules loaded
with the plain core loader, which leaves the slots zeroed — fall back to
pool-generated stubs. See [Host Guide — Integrating the Dependency System]
(integrating-as-host.md#integrating-the-dependency-system-udynlink_deps).

**Exports are not namespaced.** Module exports are bare C names, and the deps
layer resolves cross-module references by name with **first-match-wins** over
the registry in load order — no ambiguity detection. Every codec module
exports the same `parse`/`write` names under its prefix, so this is only safe
because the pipeline **defaults the export prefix to the `.proto` basename**
(`sensor.proto` → `sensor_parse`/`sensor_write`): as long as the file names
differ, module-to-module references cannot collide. Only use
`--export-prefix ''` (bare `parse`/`write`) when a single codec module is
loaded and no other module calls it. Host-side calls via
`udynlink_lookup_symbol(p_mod, ...)` are unaffected either way — the module
handle disambiguates.

## Overhead

Measured on `cortex-m4`, `-Os`, pipeline defaults (thunk exports on, export
prefix = proto basename, `--strip-non-public-syms`), runtime host-side.
`--plain-exports` subtracts ~120 B flash and 18 B + 10 B per exported
function of RAM per module (see the breakdown below).

| Module | Image (flash) | RAM (XIP) | RAM (COPY_TEXT_DATA) |
|---|---|---|---|
| `Empty` (0 fields, 2 exports) | 564 B | 84 B | 232 B |
| `SensorReading` (5 fields, string + repeated) | 596 B | 84 B | 260 B |
| `SensorReading`+`Config`, one module | 1044 B | 132 B | 472 B |
| `SensorReading`+`Config`+`Status`, one module | 1440 B | 180 B | 684 B |

The preallocated thunks are the difference from the plain-function numbers
(e.g. `SensorReading` alone: 476 B flash / 44 B XIP / 220 B COPY_TEXT_DATA):
~60 B flash per export for the extra named symbol-table entries (markers +
gateway), and 18 B gateway + 10 B per exported function (+ padding) of
module `.bss` in RAM.

Breakdown of the 596 B single-struct module: 32 B header + 56 B relocations +
308 B symbol table (2 exported functions + 2 thunk-export markers + gateway
+ 4 extern + 3 nameless internals + module name) + 176 B code (12 B wrapper
per export, 56–60 B `sensor_parse`/`sensor_write` bodies, literal pool) +
24 B data (the `pb_msgdesc_t`). RAM is 5 LOT entries (20 B, one per distinct
referenced symbol: 4 runtime fns + the msgdesc) + 24 B data + 40 B `.bss`
thunk pool (18 B gateway + 2 × 10 B stub slots + padding). The basename
export prefix costs ~6 B per exported symbol; `--export-prefix ''` trims the
image by 12 B.

### What "1 module == 1 struct" costs

- **Fixed per-module overhead:** ~230 B flash + ~56 B RAM — the header,
  relocation/symbol tables, prologue wrappers, LOT, and the preallocated
  thunk slots (18 B gateway + 10 B per export) that every module carries
  regardless of struct size.
- **Marginal per-struct cost in a shared module:** ~400 B flash + ~48 B RAM
  (2-struct → 3-struct delta; includes 2 more exported functions' worth of
  thunk slots and symbol names per struct).
- **Premium for splitting one shared module into N per-struct modules:**
  ~110 B flash + ~34 B RAM per struct (each new module pays the fixed cost
  again, minus the shared-module savings). For the 3-struct example:
  3 modules = 1764 B vs 1 module = 1440 B.
- **Scaling:** struct complexity grows the module slowly — 22 fields cost
  only ~230 B more than 5 fields (mostly the bigger `pb_msgdesc_t` table).
  The cost model is dominated by the fixed per-module overhead, so the
  per-struct premium stays roughly constant as messages grow.

### Conclusion

The overhead of 1 module == 1 struct is small (≈260 B flash, ≈50 B RAM per
struct on a typical M-profile MCU, thunks included) and buys maximum dynamic
flexibility — load only the codecs you use, from flash or external storage,
on demand, with no recompilation of the host, and call them from other
modules with no dynamic-pool pressure. The two real costs are not bytes:

1. **The nanopb runtime must be host-side** (one ~10 KB flash cost, shared).
   Bundling it per module costs ~9 KB per module and negates the model.
2. **Integration surface:** the host links the runtime and resolves 4 names;
   each module adds a `udynlink_module_t` handle (24 B) and its image storage.

The mkmodule toolchain fix this analysis surfaced — wrapped function bodies
leaking into the symbol table on multi-file builds — is fixed in
`scripts/mkmodule`; the numbers above include that fix.

## Tests

- `tests/test-protobuf-module` — QEMU integration test: loads a
  `SensorReading` module in all three load modes, resolves the four nanopb
  runtime symbols, and round-trips an encoded message through `write`/`parse`
  (`just test-f429-single test-protobuf-module`). The committed wrapper is
  generated with thunk exports on (the pipeline default) and loaded with the
  plain core loader, verifying the zeroed thunk slots don't affect direct
  calls.
- `tests/py/test_proto2module.py` — pipeline unit/integration tests
  (`just test-py-all`), including size-budget assertions that keep the
  1-module-per-struct overhead from regressing, multi-struct API-header
  coverage (one `parse`/`write` pair per selected message), thunk-export
  vs. `--plain-exports` symbol-shape assertions, and the argument-validation
  behavior.
- CI (`.github/workflows/ci.yml`) runs the pytest suite with `protoc` and
  `nanopb==0.4.9.1` installed, so the pipeline tests gate on every push; the
  QEMU test builds from the committed generated files and needs no protoc.
