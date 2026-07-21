# scripts/

## Responsibility
Build toolchain for compiling C/C++ source into `udynlink` loadable module images. Includes compilation orchestration, ELF parsing, relocation processing, C++ runtime support, selective symbol exporting, dead-code elimination, and binary image generation for ARM Cortex-M targets.

## Design Patterns
- **Pipeline / Chain of Responsibility**: `mkmodule` orchestrates a fixed sequence: compile → link → process → (optional) generate C header.
- **Template-based Code Generation**: Uses Jinja2 (`asm_template.tmpl`) to emit ARM Thumb-2 assembly prologues that wrap exported functions with `r9` (LOT base) setup.
- **ELF Introspection**: `udynlink_utils.py` uses `pyelftools` to read sections, symbols, and relocations from linked ELF files. The on-disk UDLM binary format itself (header, symbol-table bit packing, relocation encoding, runtime RAM layout) is defined once in `udynlink_parser.py`, which both `mkmodule` (writer) and the test suite (reader) import — no format constants are duplicated across the Python toolchain.
- **Strategy for Language Support**: `mkmodule` detects `.cpp`/`.cxx` files and automatically injects `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` plus links `cpp_init_fini.c` for constructor support.
- **Selective Export Filter**: `--public-symbols` switches from "wrap all globals" to "wrap only specified globals + `__init_array`", reducing image size.

## Data & Control Flow
1. **Compile** (`compile` in `mkmodule`):
   - Invoke `arm-none-eabi-gcc` with position-independent flags (`-fPIE`, `-msingle-pic-base`, `-ffunction-sections`, `-fdata-sections`).
   - For C++ sources: add `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and compile `cpp_init_fini.c` for `__init_array` support.
   - Wrap exported functions: rename originals with MD5 prefix, generate prologue assembly that loads `r9` from fixed address `0x20000000` (see `asm_template.tmpl`), then branches to the wrapped function.
   - Assemble prologue into a second object file.
2. **Link** (`link` in `mkmodule`):
   - Link all objects with `code_before_data.ld` linker script (text → data → bss, origin at 0).
   - Pass `-Wl,--gc-sections` to eliminate unused functions/data, with `KEEP` directives in the linker script to preserve prologues and `.init_array`.
   - Pass `--unresolved-symbols=ignore-in-object-files` so external symbols don't fail at link time.
   - Make wrapped symbols local via `objcopy -L`.
3. **Process** (`process` in `mkmodule`):
   - Extract `.text`, `.data`, `.bss` sections from ELF.
   - Classify symbols as local / exported / external.
   - Process relocations (`R_ARM_GOT_BREL` for LOT, `R_ARM_ABS32` and `R_ARM_TARGET1` for data, `R_ARM_THM_CALL`/`R_ARM_THM_JUMP24` ignored as PC-relative).
   - Deduplicate relocations: only first relocation to a symbol gets a LOT slot; data relocations use a separate offset space.
   - Build module image header + symbol table + relocation list + code + data.
   - Emit `.bin` file (and optionally a C header with byte array).

## Integration Points
- **Depends on**: `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, `pyelftools`, `Jinja2`.
- **Produces**: Binary module images consumed by the `udynlink` runtime loader (`udynlink_load_module`).
- **Linker Script**: `code_before_data.ld` enforces zero-origin layout required by the loader's relocation math.
- **C++ Runtime**: `cpp_init_fini.c` provides a portable `__init_array` constructor runner for C++ global objects.

## Key Files
| File | Purpose |
|------|---------|
| `mkmodule` | Main CLI entry point. Orchestrates compile/link/process pipeline. |
| `udynlink_utils.py` | ELF parsing helpers (symbols, relocations, sections), CLI helpers, MD5 symbol wrapping. |
| `udynlink_parser.py` | **Single source of truth for the UDLM binary module format** (Python). Decodes the on-disk image (header, relocation table, symbol table, code/data) and models the runtime RAM layout per load mode, mirroring `udynlink/udynlink.c` and `udynlink/udynlink.h`. `mkmodule` imports its format constants (`MODULE_SIGNATURE`, `SYM_TYPE_*`, `RELOC_FLAG_*`, `pack_version`, …) instead of re-declaring magic numbers; `tests/py/test_udynlink_parser.py` guards the contract via lossless round-trip parsing of every in-tree sample module. |
| `asm_template.tmpl` | Jinja2 template generating ARM Thumb-2 assembly prologues for exported functions. |
| `code_before_data.ld` | Custom linker script: `.text` at origin 0, followed by `.data` (with `.init_array`), then `.bss`. |
| `cpp_init_fini.c` | C++ global constructor runner (`__preinit_array` + `__init_array`). Compiled and linked automatically for C++ modules. |
