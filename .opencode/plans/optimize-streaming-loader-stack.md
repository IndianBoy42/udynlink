# Optimize Streaming Loader: Reduce Stack Allocations & Copies

## Goal

Eliminate stack-allocated buffers in the streaming loader path by using host-provided scratch space, and reduce unnecessary data copies.

## Current State

### Stack Buffers in `udynlink_load_module_from_stream()` (~196 bytes)

| Line | Variable | Size | Lifetime |
|------|----------|------|----------|
| 684 | `udynlink_module_header_t header` | 36B | Function scope — needed throughout |
| 739 | `char dep_name[64]` | 64B | Per dep iteration |
| 836 | `uint32_t rel_pair[2]` | 8B | Per reloc iteration |
| 859 | `uint32_t sym_count` | 4B | Per reloc iteration |
| 870 | `uint32_t sym_entry[2]` | 8B | Per reloc iteration |
| 899 | `char sym_name[64]` | 64B | Per extern reloc iteration |
| 910 | `udynlink_sym_t dep_sym` | ~12B | Per extern reloc iteration |

### Stack Buffers in Helper Functions

| Function | Variable | Size |
|----------|----------|------|
| `udynlink_get_ram_requirements_stream()` | `header` | 36B |
| `udynlink_get_stream_metadata_size()` | `header` | 36B |

### Copy Overhead in `stream_read_exact()`

Every read: `stream → work_buf → memcpy → dest`. When the final destination is module RAM (bulk copy), this intermediate copy is unnecessary if the stream callback can write directly to dest.

## Design

### 1. Add `scratch_buf` Parameter

Add `void *scratch_buf, uint32_t scratch_buf_size` to the streaming load function. This buffer holds all temporary state that currently lives on the stack.

**Why a separate parameter, not a partitioned `work_buf`:**
- `work_buf` serves as an I/O intermediate for `stream_read_exact()`/`stream_read_string()`. It cannot overlap with `dest` (memcpy UB).
- Many stream implementations (SD card, flash) require `work_buf` to be sector-aligned and sector-sized. Carving space out of `work_buf` breaks these drivers.
- Separation keeps concerns clean: `work_buf` = I/O, `scratch_buf` = temp state.

### 2. Scratch Buffer Layout (Internal, Phased Reuse)

The scratch buffer is used in phases — regions are reused as processing progresses:

```
Offset 0..35:    udynlink_module_header_t header  (needed throughout entire function)
Offset 36..99:   char name_buf[64]                 (reused for dep_name AND sym_name)
Offset 100..107: uint32_t reloc_pair[2]            (per-reloc iteration, reused)
Offset 108..111: uint32_t sym_count                (per-reloc iteration, reused)
Offset 112..119: uint32_t sym_entry[2]             (per-reloc iteration, reused)
Offset 120..131: udynlink_sym_t dep_sym            (per-extern-reloc, reused)
```

**Minimum scratch size: 132 bytes** (define as `UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE`)

**Phased reuse rules:**
- `name_buf` is used for `dep_name` during dependency resolution, then reused for `sym_name` during relocation. These phases never overlap.
- `reloc_pair`, `sym_count`, `sym_entry`, `dep_sym` are all per-iteration variables that share the same loop phase. They're at separate offsets within scratch_buf (not a union) to give the compiler natural alignment.

### 3. API Changes

#### Primary function — modify existing signature:

```c
udynlink_error_t udynlink_load_module_from_stream(
    udynlink_module_t *p_mod,
    const udynlink_io_t *p_io,
    void *load_addr, uint32_t load_size,
    udynlink_load_mode_t load_mode,
    void *work_buf, uint32_t work_buf_size,
    void *scratch_buf, uint32_t scratch_buf_size);  // NEW
```

This is a **breaking API change**. On embedded systems, downstream consumers are typically rebuilt from source, so this is acceptable. The new parameter is validated: if `scratch_buf == NULL` or `scratch_buf_size < UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE`, return `UDYNLINK_ERR_LOAD_INVALID_MODE`.

#### Helper functions — add scratch parameter:

```c
uint32_t udynlink_get_ram_requirements_stream(
    const udynlink_io_t *p_io, udynlink_load_mode_t mode,
    void *scratch_buf, uint32_t scratch_buf_size);  // NEW

uint32_t udynlink_get_stream_metadata_size(
    const udynlink_io_t *p_io,
    void *scratch_buf, uint32_t scratch_buf_size);  // NEW
```

These only need 36 bytes of scratch for the header. The same scratch buffer can be reused across all three calls in a typical load sequence.

### 4. Reduce Double-Copy in `stream_read_exact()`

**Current:** `p_io->read(ctx, work_buf, chunk, offset)` → `memcpy(dest, work_buf, chunk)`  
**Problem:** For bulk copies into module RAM (COPY_ALL/COPY_TEXT_DATA), the work_buf → dest memcpy is pure overhead.

**Optimization:** Add a `read_direct` field to `udynlink_io_t`:

```c
typedef struct {
    udynlink_read_cb_t      read;
    udynlink_get_size_cb_t  get_size;
    udynlink_read_cb_t      read_direct;  // NEW (optional)
    void                   *pv_ctx;
} udynlink_io_t;
```

- When `read_direct != NULL`, `stream_read_exact()` calls it with `dest` directly — no work_buf intermediate.
- When `read_direct == NULL`, falls back to the existing work_buf → memcpy path.
- Block-device stream implementations leave `read_direct = NULL` (they need sector-aligned buffers).
- Memory-mapped or buffered stream implementations can set `read_direct = read` to eliminate the copy.

This is **backward compatible** at the struct level: the new field is at the end, and zero-initialized (NULL) by default. Existing code that initializes `udynlink_io_t` with designated initializers or `= {read, get_size, ctx}` will get `read_direct = NULL` (struct padding/initialization rules).

**Wait** — actually, adding a field to the middle of a struct breaks ABI if anyone uses positional initialization: `= {read, get_size, ctx}`. We must add it BEFORE `pv_ctx` to maintain offset compatibility, or use designated initializers. Since this is C99+ embedded code, we should assume designated initializers.

**Actually** — a simpler alternative: just check if `dest != work_buf` and if so, try to read directly into dest for the full length first, falling back to chunked work_buf if the read returns short:

```c
if (dest != work_buf && len <= work_buf_size) {
    int32_t n = p_io->read(p_io->pv_ctx, dest, len, offset);
    if (n > 0 && (uint32_t)n == len) return (int32_t)len;
    // fall through to chunked path
}
```

This is simpler but only eliminates the copy for short reads that complete in one callback call. For large bulk copies (which is the common case), we still need chunked reads.

**Decision point: `read_direct` callback vs. opportunistic direct read vs. neither.**

### 5. Helper Function Changes

`stream_read_exact()` and `stream_read_string()` currently take `(p_io, dest, offset, len, work_buf, work_buf_size)`. No changes needed to their signatures — the scratch buffer is not passed to them, it's used as `dest` by the caller.

Specifically:
- `stream_read_string(p_io, (char*)scratch_name_buf, ...)` — scratch_buf provides the dest
- `stream_read_exact(p_io, scratch_rel_pair, ...)` — scratch_buf provides the dest
- The header read: `p_io->read(p_io->pv_ctx, scratch_header_buf, sizeof(header), 0)` — reads directly into scratch (no work_buf intermediate needed for a single 36-byte read)

## Task Breakdown

### Task 1: Add scratch_buf to API surface
**File:** `udynlink/udynlink.h`
- Add `UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE` constant (132)
- Add `scratch_buf, scratch_buf_size` parameters to `udynlink_load_module_from_stream()`
- Add `scratch_buf, scratch_buf_size` parameters to `udynlink_get_ram_requirements_stream()` and `udynlink_get_stream_metadata_size()`
- Update doc comments

### Task 2: Refactor udynlink_load_module_from_stream() to use scratch_buf
**File:** `udynlink/udynlink.c`
- Add internal accessor macros/inline helpers for scratch_buf offsets
- Replace stack `header` with `(udynlink_module_header_t*)scratch_buf`
- Replace stack `dep_name[64]` with `scratch_buf + 36`
- Replace stack `sym_name[64]` with `scratch_buf + 36` (same region, phased reuse)
- Replace stack `rel_pair[2]` with `scratch_buf + 100`
- Replace stack `sym_count` with `scratch_buf + 108`
- Replace stack `sym_entry[2]` with `scratch_buf + 112`
- Replace stack `dep_sym` with `scratch_buf + 120`
- Add scratch_buf validation at function entry
- Update all access patterns (pointer dereference through scratch base instead of local variable)

### Task 3: Refactor helper functions to use scratch_buf
**File:** `udynlink/udynlink.c`
- `udynlink_get_ram_requirements_stream()`: use scratch_buf for header instead of stack local
- `udynlink_get_stream_metadata_size()`: same

### Task 4: Optimize stream_read_exact to reduce copies (optional)
**File:** `udynlink/udynlink.c`, `udynlink/udynlink.h`
- Add `read_direct` optional callback to `udynlink_io_t`
- Modify `stream_read_exact()` to use `read_direct` when available
- Update test mock stream to support `read_direct`

### Task 5: Update test harness
**File:** `tests/test-streaming-load/test_qemu.c`
- Add scratch_buf allocations to `test_streaming_load()`
- Pass scratch_buf to all streaming API calls
- Add a test with minimum scratch_buf size (132 bytes)
- Verify all 6 test vectors still pass

### Task 6: Run test suite
- `just test-f429` or equivalent to validate all streaming tests pass
- Verify at both `-O0` and `-Os`

## Open Questions

1. **read_direct optimization** — Is eliminating the double-copy worth the added API complexity? The `read_direct` callback in `udynlink_io_t` is clean but adds another field to maintain. For typical streaming loads from SD card, the bulk copy memcpy cost is small relative to I/O latency. Is this optimization valued?

2. **Scratch buffer layout stability** — If we define `UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE = 132`, it's part of the ABI. If future headers grow or we need more scratch space, this constant increases. Should we expose a struct (e.g., `udynlink_stream_scratch_t`) that callers allocate, so they get the right size automatically from `sizeof()`?

3. **Static assert on scratch layout** — Should we add `_Static_assert` to ensure the internal offsets match? This prevents drift between the scratch layout and the actual struct sizes.

## Context Guide for Implementation Agents

Key files:
- `udynlink/udynlink.h` — Public API declarations (streaming section starts at line 273)
- `udynlink/udynlink.c` — Implementation (streaming helpers at line 246, main function at line 678)
- `tests/test-streaming-load/test_qemu.c` — Streaming test harness
- `udynlink/udynlink_externals.h` — Host callback interface (no changes needed)

The scratch buffer replaces ALL stack-allocated buffers listed in "Current State" section. The phased reuse is safe because deps are resolved before relocations, and each relocation is processed sequentially. No two phases of scratch_buf usage overlap.
