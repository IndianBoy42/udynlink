# udynlink/

## Responsibility
Core C library implementing a micro dynamic linker for ARM Cortex-M MCUs. Handles loading, relocation, symbol resolution, module unloading, and C++ global constructor initialization of position-independent binary modules at runtime.

## Design Patterns
- **External Dependency Injection**: All platform-specific operations (malloc/free, printf, symbol resolution, RAM pointer validation) are declared in `udynlink_externals.h` and must be provided by the host firmware.
- **State Machine per Module**: Each `udynlink_module_t` tracks its own load mode and RAM ownership via bit-packed `info` field.
- **Symbol Table Encoding**: Symbol metadata (type, location) is packed into the top 4 bits of the name-offset word in the symbol table, saving space in the binary image.
- **Fixed LOT Base**: Instead of computing `r9` from PC, the eh2k fork uses a fixed memory address (`0x20000000`) that the host must populate with `p_mod->ram_base` before calling any module function.

## Data & Control Flow
1. **Module Loading** (`udynlink_load_module`):
   - Validate signature (`UDLM`) and check for duplicate module names.
   - Allocate or verify RAM region based on load mode (`COPY_ALL`, `COPY_TEXT_DATA`, `XIP`).
   - Copy code/data sections to RAM as required by load mode.
   - Zero out BSS.
   - Apply relocations: for each `(lot_offset, symt_offset)` pair:
     - If `symt_offset` has bit 31 set: `R_ARM_ABS32` data relocation (`*p += &data - value`).
     - If `symt_offset` has bit 30 set: `.text` base relocation (`*p += &code`).
     - Otherwise: resolve symbol from symbol table and write into LOT or data section.
   - Resolve external symbols via `udynlink_external_resolve_symbol`.
2. **Non-Contiguous Image Loading** (`udynlink_load_module_image`):
   - Loads a module from a `udynlink_module_image_t` descriptor where each section (header, relocs, symtab, code, data) can point to a different, non-contiguous buffer.
   - Uses the **same canonical relocation path** as `udynlink_load_module`: validation, RAM allocation, section copy, BSS zeroing, and relocation are all shared helpers.
   - Supports all three load modes: `COPY_ALL`, `COPY_TEXT_DATA`, and `XIP`.
   - Post-load symbol lookup (`udynlink_lookup_symbol`) works because `p_mod->p_header` always points to the contiguous source image regardless of load mode.
3. **C++ Constructor Init** (`udynlink_cpp_init`):
   - Looks up `__init_array` symbol in the loaded module.
   - Writes `p_mod->ram_base` to fixed address `0x20000000`.
   - Calls the constructor array function.
3. **Symbol Lookup** (`udynlink_lookup_symbol`):
   - Iterate symbol table entries, match by name, apply code/data base offset, return resolved address.
4. **Module Unloading** (`udynlink_unload_module`):
   - Free allocated RAM (if not foreign), zero module structure.

## Integration Points
- **Host Firmware** must implement functions from `udynlink_externals.h`:
  - `udynlink_external_malloc` / `udynlink_external_free`
  - `udynlink_external_vprintf` (debug output)
  - `udynlink_external_resolve_symbol` (foreign symbol resolution)
  - `udynlink_external_is_pointer_in_ram` (RAM boundary check)
- **Module Images** are produced by the `scripts/mkmodule` toolchain and consumed by this library.
- **Public API**: `udynlink.h` is the single header consumed by host firmware.
- **Fixed Address Convention**: Host must write `p_mod->ram_base` to `*(uint32_t*)0x20000000` before calling any exported module function (this address is hardcoded in `asm_template.tmpl`).

## Key Files
| File | Purpose |
|------|---------|
| `udynlink.h` | Public API: data structures, error codes, function declarations. |
| `udynlink.c` | Core implementation: load, unload, relocate, resolve, lookup, debug, `udynlink_cpp_init`. |
| `udynlink_externals.h` | Host firmware contract: 5 functions the host MUST implement. |
| `udynlink_thunk.h` | Thunk pool API: pool init, gateway/stub allocation, make_call, find_stub. |
| `udynlink_thunk.c` | Thunk pool implementation: runtime-generated ARM thunks, gateway/stub templates. |

## API Additions (eh2k fork)
- `udynlink_cpp_init(udynlink_module_t*)`: Runs C++ global constructors via `__init_array`.
- `udynlink_error_msg(udynlink_error_t*)`: Converts error enum to human-readable string.
- `udynlink_get_module_name_from_image(const void*)`: Reads module name directly from a base address without loading.
- `udynlink_get_image_size(const void*)`: Computes total size of a module blob from its header.
- `udynlink_get_text_pointer(const udynlink_module_t*)`: Returns pointer to the module's `.text` section in memory.
- `udynlink_module_image_t`, `udynlink_load_module_image()`, `udynlink_image_from_memory()`, `udynlink_validate_header()`, `udynlink_compute_ram_size()`: Non-contiguous image loading primitives replacing the old streaming I/O API.

## Non-Contiguous Image API
- `udynlink_module_image_t`: Descriptor with per-section pointers (`p_header`, `p_relocations`, `p_symtab`, `p_deps_strtab`, `p_code`, `p_data`). Enables loading from SD card, SPI flash, decompressed buffers, or any source where sections are not contiguous.
- `udynlink_image_from_memory(base_addr, out)`: Populates an `image_t` from a standard contiguous UDLM buffer.
- `udynlink_image_from_module(p_mod, out)`: Populates an `image_t` from a loaded module's `p_header`.
- `udynlink_load_module_image(p_mod, image, load_addr, load_size, mode)`: Loads a module from a non-contiguous image descriptor. Supports all three load modes.
- `udynlink_validate_header(hdr)`: Validates signature and ABI version before allocating RAM.
- `udynlink_compute_ram_size(hdr, mode)`: Returns RAM needed for a module.
- `udynlink_get_image_metadata_size(hdr)`: Returns size of metadata before the code section.
- `udynlink_load_apply_relocations(p_mod, hdr, relocs, symtab)`: Low-level relocation primitive for custom loading pipelines.
- `UDYNLINK_SYMBOL(sym)`: Convenience macro for host symbol table entries: `{ #sym, (void*)(uintptr_t)(sym) }`.
