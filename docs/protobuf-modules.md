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

Requires `protoc`, the nanopb generator plugin (`uv pip install nanopb`), the
vendored nanopb runtime (`third_party/nanopb`), and the usual
`arm-none-eabi-gcc`. Tools are located via `--protoc` / `--plugin` /
`--nanopb-dir` flags or the `UDYNLINK_PROTOC` / `UDYNLINK_NANOPB_PLUGIN` /
`UDYNLINK_NANOPB_DIR` environment variables.

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

# with an embeddable C array header for the host firmware
python3 scripts/proto2module --struct SensorReading --gen-c-header proto/sensor.proto
```

Outputs, next to the `.proto` (or in `--out-dir`):

| File | Purpose |
|---|---|
| `<proto>.bin` (actually `<proto>_mod.bin`) | The UDLM image |
| `<proto>.pb.c` / `<proto>.pb.h` | nanopb-generated codec + struct definition (host includes the `.pb.h`) |
| `<proto>_mod.c` | Generated `parse`/`write` wrapper (the module source) |
| `<proto>_api.h` | Host-side prototypes for the module ABI |

`--struct` takes the nanopb C type name: `package` + `Message` joined with
`_` (e.g. `acme_sensor_TempReading`). If the file has exactly one message,
`--struct` is optional. A file with several messages without `--struct` is
an error. Extra arguments after `--` are forwarded to `mkmodule`.

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
resolver use `udynlink_dep_resolve_func()` — it generates a per-function stub
(10 B) + per-module gateway (18 B) from the thunk pool that switches `r9`
around the call. See [Host Guide — Integrating the Dependency System]
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

Measured on `cortex-m4`, `-Os`, `--public-symbols <prefix>_parse,<prefix>_write`
+ `--strip-non-public-syms` (the pipeline's default, export prefix = proto
basename), runtime host-side.

| Module | Image (flash) | RAM (XIP) | RAM (COPY_TEXT_DATA) |
|---|---|---|---|
| Empty 2-function stub module | 112 B | 0 B | 32 B |
| `SensorReading` (5 fields, string + repeated) | 476 B | 44 B | 220 B |
| `Big` (22 fields, nested msg, enum, repeated strings) | 704 B | 76 B | 412 B |
| `SensorReading`+`Config`+`Status`, one module | 1084 B | 100 B | 604 B |
| Same codec with nanopb runtime **inside** the module | 8768 B | 368 B | 7592 B |

Breakdown of the 476 B single-struct module: 32 B header + 56 B relocations +
188 B symbol table (2 exported + 4 extern + 3 nameless internals + module
name) + 176 B code (12 B wrapper per export, 56–60 B `sensor_parse`/
`sensor_write` bodies, literal pool) + 24 B data (the `pb_msgdesc_t`). RAM is
5 LOT entries (20 B, one per distinct referenced symbol: 4 runtime fns + the
msgdesc) + 24 B data. The basename export prefix costs ~6 B per exported
symbol; `--export-prefix ''` trims the image by 12 B.

### What "1 module == 1 struct" costs

- **Fixed per-module overhead:** ~172 B flash + ~16 B RAM — the header,
  relocation/symbol tables, prologue wrappers, and LOT that every module
  carries regardless of struct size.
- **Marginal per-struct cost in a shared module:** ~304 B flash + ~28 B RAM.
- **Premium for splitting one shared module into N per-struct modules:**
  ~172 B flash + ~16 B RAM per struct (each new module pays the fixed cost
  again). For the 3-struct example: 3 modules = 1428 B vs 1 module = 1084 B.
- **Scaling:** struct complexity grows the module slowly — 22 fields cost
  only ~228 B more than 5 fields (mostly the bigger `pb_msgdesc_t` table).
  The cost model is dominated by the fixed per-module overhead, so the
  per-struct premium stays roughly constant as messages grow.

### Conclusion

The overhead of 1 module == 1 struct is small (≈172 B flash, ≈16 B RAM per
struct on a typical M-profile MCU) and buys maximum dynamic flexibility —
load only the codecs you use, from flash or external storage, on demand, with
no recompilation of the host. The two real costs are not bytes:

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
  (`just test-f429-single test-protobuf-module`).
- `tests/py/test_proto2module.py` — pipeline unit/integration tests
  (`just test-py-all`), including size-budget assertions that keep the
  1-module-per-struct overhead from regressing.
