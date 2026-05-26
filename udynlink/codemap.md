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
2. **Streaming Module Loading** (`udynlink_load_module_from_stream`):
   - Same validation and relocation logic as the memory-mapped path, but reads module data through a `udynlink_io_t` callback interface instead of directly dereferencing memory.
   - Requires a caller-provided work buffer (minimum 64 bytes) for chunked I/O reads.
   - Supports `COPY_ALL` and `COPY_TEXT_DATA` only; XIP returns `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED`.
   - For COPY_TEXT_DATA, the streaming loader internally uses a COPY_ALL-style RAM layout (header+metadata+code+data after LOT) so that `udynlink_lookup_symbol` works correctly after loading.
   - On-demand symbol resolution: reads individual symbol entries and names from the stream during relocation processing, avoiding pre-loading the entire symbol table.
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

## API Additions (eh2k fork)
- `udynlink_cpp_init(udynlink_module_t*)`: Runs C++ global constructors via `__init_array`.
- `udynlink_error_msg(udynlink_error_t*)`: Converts error enum to human-readable string.
- `udynlink_get_module_name_from_image(const void*)`: Reads module name directly from a base address without loading.
- `udynlink_get_image_size(const void*)`: Computes total size of a module blob from its header.
- `udynlink_get_text_pointer(const udynlink_module_t*)`: Returns pointer to the module's `.text` section in memory.

## Streaming I/O API
- `udynlink_io_t`: Struct with `read(pv_ctx, buf, num_bytes, offset)` and `get_size(pv_ctx)` callbacks plus a `pv_ctx` user pointer. Enables loading from SD card, SPI flash, network streams, or any non-memory-mapped source.
- `udynlink_load_module_from_stream(p_mod, p_io, load_addr, load_size, load_mode, work_buf, work_buf_size)`: Loads a module from a streaming source. COPY_ALL and COPY_TEXT_DATA only.
- `udynlink_get_ram_requirements(base_addr, mode)`: Returns RAM needed for a memory-mapped module.
- `udynlink_get_ram_requirements_stream(p_io, mode)`: Returns RAM needed for a streaming module (reads header from stream).
- `udynlink_get_stream_metadata_size(p_io)`: Returns size of module metadata before the code section (= `sizeof(header) + num_rels*8 + symt_size`).
- `UDYNLINK_STREAM_BUF_SIZE`: Compile-time default (512 bytes, matches FatFS sector size). Minimum work buffer is 64 bytes.
- `UDYNLINK_ERR_LOAD_IO_ERROR`: Error code for I/O read failures from stream callbacks.
- `UDYNLINK_SYMBOL(sym)`: Convenience macro for host symbol table entries: `{ #sym, (void*)(uintptr_t)(sym) }`.
