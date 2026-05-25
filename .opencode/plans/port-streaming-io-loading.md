# Plan: Port TethysRT Streaming I/O Loading to udynlink

## Goal
Port the storage-agnostic streaming I/O module loading interface from TethysRT into udynlink, enabling loading from SD card, SPI flash, network streams, or any non-memory-mapped source without pre-buffering the entire module image.

## Context Summary

### udynlink Current State
- **`udynlink/udynlink.h`** (221 lines): Public API. `udynlink_load_module()` takes `const void *base_addr` — requires entire module image in contiguous, memory-mapped RAM/flash.
- **`udynlink/udynlink.c`** (467 lines): Core loader. Directly dereferences `base_addr` for header, relocations, symbol table, code, and data. Uses `memcpy` for COPY_ALL / COPY_CODE / XIP modes. Relies on pointer arithmetic (`p_header + sizeof(...)`, `p_rels++`) throughout. Contains raw `(uint32_t)ptr` casts that warn on 64-bit hosts.
- **`udynlink/udynlink_externals.h`** (38 lines): Host must provide `malloc`, `free`, `vprintf`, `resolve_symbol`, `is_pointer_in_ram`.
- **Load modes**: `COPY_ALL` (header+code+data→RAM), `COPY_CODE` (code+data→RAM), `XIP` (data→RAM only, execute code from flash). Streaming is fundamentally incompatible with `XIP` because the code must be memory-mapped.
- **Module format**: Custom binary (`UDLM` signature) — not ELF. Header → relocations (num_rels × 8 bytes) → symbol table (symt_size bytes) → code → data.
- **Tests**: QEMU-based integration tests in `tests/test-*/`. Each test compiles a module via `scripts/mkmodule`, copies `test_qemu.c` to `tests/qemu_host/src/`, builds `test1.elf`, runs QEMU, checks for `*** TEST OK ***`.

### TethysRT Reference (already cloned to `/tmp/TethysRT`)
- Uses `tethys_io_t` struct with `read(pv_ctx, buf, num_bytes, offset)` and `get_size(pv_ctx)` callbacks.
- `read` is **offset-based random access**, not sequential streaming.
- Internal 512-byte static buffer (`TETHYS_IO_BUFFER_SIZE`) for chunking section copies.
- `tethys_get_module_size()` reads just the ELF header and section headers to compute RAM needs without loading.
- The loader itself does **no allocation**; host pre-allocates RAM.

## Architecture Decisions

### 1. I/O Callback Interface
```c
typedef int32_t (*udynlink_read_cb_t)(void *pv_ctx, void *buf, uint32_t num_bytes, uint32_t offset);
typedef int32_t (*udynlink_get_size_cb_t)(void *pv_ctx);

typedef struct {
    udynlink_read_cb_t     read;
    udynlink_get_size_cb_t get_size;
    void                  *pv_ctx;
} udynlink_io_t;
```
- Returns `int32_t`: bytes read / size, or `-1` on error.
- Random-access `offset` from image start (same as TethysRT). This is required because the loader reads header, then jumps to relocation table, then symbol table, then code/data — not sequentially.

### 2. No Internal Malloc for Metadata
The streaming loader **must not** internally allocate memory for header, relocations, or symbol table. Instead, the caller provides a working buffer whose size they control. The loader reads from the stream into this buffer in chunks.

### 3. On-Demand Symbol Resolution
Since the symbol table is not pre-loaded into RAM, symbol entries and names are read from the stream on-demand during relocation processing:
- For each relocation, read the `(lot_offset, symt_offset)` pair from the stream.
- If the relocation references a symbol (not a data/code offset), read the symbol entry (`name_off`, `val`) from the symtab region of the stream.
- For exported/extern/name symbols, read the name string from the stream in small batches until the null terminator is found.
- If the caller provides a large enough working buffer, the loader can batch-read multiple entries to minimize `read()` callbacks.

### 4. User-Provided Working Buffer API
```c
// Load module from streaming I/O source.
// work_buf / work_buf_size: caller-provided scratch memory. Minimum 64 bytes.
//   The loader reads metadata and copies sections through this buffer.
udynlink_error_t udynlink_load_module_stream(udynlink_module_t *p_mod,
                                             const udynlink_io_t *p_io,
                                             void *load_addr, uint32_t load_size,
                                             udynlink_load_mode_t load_mode,
                                             void *work_buf, uint32_t work_buf_size);

// Returns the exact working buffer size needed to read header+relocs+symtab in one go.
// If the caller provides at least this many bytes, the loader minimizes read callbacks.
// For smaller buffers, the loader falls back to reading metadata in smaller chunks.
uint32_t udynlink_get_stream_work_buf_size(const udynlink_io_t *p_io);
```
- `udynlink_load_module_stream()` supports `COPY_ALL` and `COPY_CODE` only. Returns `UDYNLINK_ERR_LOAD_UNABLE_TO_XIP` for `XIP` mode.
- Existing `udynlink_load_module()` remains unchanged — memory-mapped path is untouched.

### 5. Buffer Size
- Compile-time constant: `#ifndef UDYNLINK_STREAM_BUF_SIZE` → **default 512** (matches common FatFS page size).
- The user-provided `work_buf` can be as small as 64 bytes; the loader adapts by reading in smaller chunks.
- The compile-time default is for documentation and host convenience, not a hard minimum.

### 6. Internal Streaming Helpers
The streaming path uses small internal helpers that operate on the `udynlink_io_t` and the caller's `work_buf`:

```c
// Read exactly len bytes from offset into dest via p_io, using work_buf if needed.
static int32_t stream_read_exact(const udynlink_io_t *p_io, void *dest,
                                 uint32_t offset, uint32_t len,
                                 void *work_buf, uint32_t work_buf_size);

// Read a null-terminated string from offset into dest (max len bytes).
static int32_t stream_read_string(const udynlink_io_t *p_io, char *dest,
                                  uint32_t offset, uint32_t max_len,
                                  void *work_buf, uint32_t work_buf_size);

// Read the module header from the stream
static int32_t stream_read_header(const udynlink_io_t *p_io, udynlink_module_header_t *p_header);
```

`stream_read_exact()` is the core primitive: if `len <= work_buf_size`, it reads into `work_buf` then `memcpy`s to `dest`. If `len > work_buf_size`, it loops in `work_buf_size` chunks. This handles both small (64B) and large (512B+) work buffers.

**Symbol name reading** uses `stream_read_string()`: it reads the name in `work_buf_size` (or smaller) chunks, scanning for `\0`. If the work buffer is ≥ the name length, it reads the name in a single callback.

### 7. `uintptr_t` Cleanup
All raw `(uint32_t)ptr` casts in `udynlink.c` are replaced with `(uint32_t)(uintptr_t)ptr` or intermediate `uintptr_t` variables. This suppresses 64-bit host compiler warnings. The actual 32-bit ARM values are unaffected.

### 8. Error Code Addition
- Add `UDYNLINK_ERR_LOAD_IO_ERROR` for generic read/get_size failure from the stream callback.
- `UDYNLINK_ERR_LOAD_UNABLE_TO_XIP` already exists and is reused for streaming XIP rejection.

### 9. Test Strategy
- **Mock stream**: A `udynlink_io_t` implementation that wraps the existing memory-mapped module data array but reads through the callback in small chunks. This exercises the streaming path without needing actual SD card / flash hardware.
- Test `COPY_ALL` and `COPY_CODE` modes via streaming.
- Test with both small (64B) and large (512B) work buffers.
- Test `udynlink_get_ram_requirements_stream()` and `udynlink_get_stream_work_buf_size()`.
- Add a new test directory: `tests/test-streaming-load/`.

## Task Breakdown

### Task 1: `uintptr_t` cleanup in `udynlink.c` and `udynlink.h`
- Replace all raw `(uint32_t)ptr` with `(uint32_t)(uintptr_t)ptr` or `uintptr_t` intermediates.
- Add `stdint.h` / `stddef.h` includes if needed.
- **Agent**: `quick` (focused mechanical refactoring)
- **Outcome**: No 64-bit cast warnings; all existing tests still pass.

### Task 2: Add streaming I/O types and API declarations to `udynlink.h`
- Add `udynlink_io_t`, callback typedefs, compile-time `UDYNLINK_STREAM_BUF_SIZE` (default 512).
- Add `udynlink_load_module_stream()` declaration.
- Add `udynlink_get_ram_requirements()`, `udynlink_get_ram_requirements_stream()`, `udynlink_get_stream_work_buf_size()` declarations.
- Add `UDYNLINK_ERR_LOAD_IO_ERROR` to error codes.
- **Agent**: `quick` (small, focused header edit)
- **Outcome**: `udynlink.h` compiles, no logic changes yet.

### Task 3: Implement `udynlink_load_module_stream()`
- Read header from stream into a local struct.
- Validate signature, version, architecture tag.
- Compute RAM size via `udynlink_get_ram_size_for_header()`.
- Allocate/load RAM (host `malloc` or provided `load_addr`).
- Stream-copy code and data sections to RAM using `work_buf` in chunks.
- Process relocations by reading pairs from stream into `work_buf`.
- Resolve symbols on-demand: read symtab entry and name string from stream.
- For `COPY_ALL`, copy header+relocs+symtab to RAM as part of the main copy so `p_mod->p_header` is RAM-resident.
- For `COPY_CODE`, keep `p_mod->p_header` NULL (or point to a RAM copy of just the header) — the loader does not need header access after load except for `udynlink_lookup_symbol()`, which is rarely used at runtime. Actually, `udynlink_lookup_symbol()` and `udynlink_get_module_name()` require header access. For `COPY_CODE`, we should copy just the header to RAM if `lookup_symbol` is needed. But the user said no internal malloc. So for `COPY_CODE` streaming, `p_mod->p_header` can point into the original flash... but there is no flash in streaming. 
  - **Resolution**: For `COPY_CODE` streaming, the caller must either accept that `lookup_symbol` won't work after load, OR we document that `COPY_CODE` + streaming requires the caller to provide extra RAM for the header. For now, copy the header to the start of the allocated RAM (before code/data). This costs `sizeof(udynlink_module_header_t)` bytes of RAM in `COPY_CODE` mode for streaming. This is acceptable.
- **Agent**: `agent` (complex implementation)
- **Outcome**: New function works; ready for testing.

### Task 4: Implement query functions (`udynlink_get_ram_requirements`, `udynlink_get_stream_work_buf_size`)
- `udynlink_get_ram_requirements(const void *base_addr, udynlink_load_mode_t mode)`: read header from mapped memory, compute size.
- `udynlink_get_ram_requirements_stream(const udynlink_io_t *p_io, udynlink_load_mode_t mode)`: read header from stream, compute size.
- `udynlink_get_stream_work_buf_size(const udynlink_io_t *p_io)`: read header, return `sizeof(header) + num_rels*8 + symt_size`.
- **Agent**: `quick` (straightforward math)
- **Outcome**: Query functions return correct sizes.

### Task 5: Implement `UDYNLINK_SYMBOL` macro in `udynlink_externals.h`
- `#define UDYNLINK_SYMBOL(sym) { #sym, (void *)(sym) }`
- **Agent**: `quick` (trivial one-liner)
- **Outcome**: Macro available for host symbol tables.

### Task 6: Create QEMU integration test for streaming load
- New test directory: `tests/test-streaming-load/`.
- `test_qemu.c`: uses a mock `udynlink_io_t` that wraps `mod_hello_module_data` with configurable chunk sizes.
- Tests `COPY_ALL` and `COPY_CODE` via streaming.
- Tests with 64-byte and 512-byte work buffers.
- Tests `udynlink_get_ram_requirements_stream()` and `udynlink_get_stream_work_buf_size()`.
- Tests XIP rejection.
- `test_data.py` with module and required output regexes.
- `mod_hello.c` — reuse a simple hello-world module.
- **Agent**: `agent` (needs to understand test harness)
- **Outcome**: Test passes in QEMU for both `-O0` and `-Os`.

### Task 7: Update README and codemap
- Document new APIs, `udynlink_io_t`, `work_buf` usage, `UDYNLINK_STREAM_BUF_SIZE`.
- Explain streaming vs. memory-mapped tradeoffs.
- Update `udynlink/codemap.md`.
- **Agent**: `quick` (docs)
- **Outcome**: Documentation is up to date.

### Task 8: Run full test suite and CI validation
- Run `cd tests && python3 test_driver.py`.
- Verify no regressions in existing tests.
- **Agent**: `shell` / `agent`
- **Outcome**: All existing tests pass; new streaming test passes.

## Execution Order

| # | Task | Agent | Dependencies |
|---|------|-------|-------------|
| 1 | `uintptr_t` cleanup | `quick` | None |
| 2 | API declarations in `udynlink.h` + `udynlink_externals.h` | `quick` | Task 1 |
| 3 | `udynlink_load_module_stream()` implementation | `agent` | Task 2 |
| 4 | Query functions (`get_ram_requirements`, `get_stream_work_buf_size`) | `quick` | Task 2 |
| 5 | `UDYNLINK_SYMBOL` macro | `quick` | None |
| 6 | Streaming integration test | `agent` | Task 3, 4 |
| 7 | Documentation updates | `quick` | Task 3, 4, 5 |
| 8 | Full test suite + CI | `shell`/`agent` | Task 6 |

## Key Design Rationale

### Why no internal malloc for metadata?
The user explicitly requested that the loader not internally allocate memory for header/relocs/symtab. This keeps the streaming path suitable for systems where `malloc` is unavailable, unreliable, or where the caller wants full control over all memory usage. The tradeoff is more `read()` callbacks and slightly more complex symbol resolution logic.

### Why on-demand symbol reading?
By reading symbol entries and names from the stream only when needed during relocation processing, we avoid pre-loading the entire symbol table into RAM. For modules with few relocations, this is very efficient. For modules with many relocations referencing the same symbol, the naive on-demand approach re-reads the same symbol entry multiple times. A simple optimization (not required for v1) is to cache the last-read symbol entry in a local static struct.

### Why does `COPY_CODE` streaming need the header in RAM?
After loading, `udynlink_lookup_symbol()` and `udynlink_get_module_name()` dereference `p_mod->p_header`. In memory-mapped `COPY_CODE`, the header stays in flash at `base_addr`. In streaming `COPY_CODE`, there is no persistent flash backing. Therefore, we copy the header to RAM as part of the load. This costs ~40 bytes and is acceptable.

### Why 512-byte default buffer size?
512 bytes matches the sector/page size of common embedded filesystems (FatFS, SPIFFS default). This means a single `read()` callback often maps directly to one filesystem block read, minimizing I/O overhead.

## Review Checklist (for execution agents)
- [ ] `udynlink.h` compiles in both host and module contexts
- [ ] All existing tests pass without modification
- [ ] New streaming test passes for both `-O0` and `-Os`
- [ ] XIP mode correctly rejected by `udynlink_load_module_stream()`
- [ ] `udynlink_get_ram_requirements_stream()` returns exact bytes needed for `COPY_ALL` and `COPY_CODE`
- [ ] `udynlink_get_stream_work_buf_size()` returns `sizeof(header) + num_rels*8 + symt_size`
- [ ] No `printf` or `stdio.h` added to `udynlink.c`
- [ ] Streaming load works with 64-byte work buffer
- [ ] Streaming load works with 512-byte work buffer
- [ ] `uintptr_t` cleanup suppresses all 64-bit pointer cast warnings
- [ ] `UDYNLINK_SYMBOL` macro usable in C99 compound literal arrays
