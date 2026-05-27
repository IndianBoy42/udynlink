# Optimize Streaming Loader: Reduce Stack Allocations & Copies

## Goal

Eliminate stack-allocated buffers in the streaming loader by using host-provided scratch space, and eliminate the double-copy (work_buf intermediate) by reading directly into final destinations.

## Current State

### Stack Buffers in `udynlink_load_module_from_stream()` (~196 bytes)

| Line | Variable | Size | Lifetime |
|------|----------|------|----------|
| 684 | `udynlink_module_header_t header` | 36B | Function scope |
| 739 | `char dep_name[64]` | 64B | Per dep iteration |
| 836 | `uint32_t rel_pair[2]` | 8B | Per reloc iteration |
| 859 | `uint32_t sym_count` | 4B | Per reloc iteration |
| 870 | `uint32_t sym_entry[2]` | 8B | Per reloc iteration |
| 899 | `char sym_name[64]` | 64B | Per extern reloc iteration |
| 910 | `udynlink_sym_t dep_sym` | ~12B | Per extern reloc iteration |

### Double-Copy in `stream_read_exact()`

Every read: `stream -> work_buf -> memcpy -> dest`. The work_buf intermediate is unnecessary - the `read` callback already writes directly to the buffer it's given. The work_buf provides no alignment guarantee (typically a stack `uint8_t[64]`), so removing it loses no real benefit.

### Scope: Single Consumer

The streaming API has exactly one consumer: `tests/test-streaming-load/test_qemu.c`. Migration is trivial.

## Decisions

- **Scratch buffer size: 132 bytes (tight)** - no padding for future-proofing. If the header grows, the constant will be bumped.
- **Remove work_buf entirely** - `read` callback writes directly to dest. Block-device drivers manage their own alignment.
- **Helper functions unchanged** - `udynlink_get_ram_requirements_stream()` and `udynlink_get_stream_metadata_size()` keep 36B stack header.
- **Raw void* + min size constant** - no exposed struct for scratch buffer layout.

## Design

### 1. Remove `work_buf`, Add `scratch_buf`

**Old signature:**
```c
udynlink_error_t udynlink_load_module_from_stream(
    udynlink_module_t *p_mod, const udynlink_io_t *p_io,
    void *load_addr, uint32_t load_size,
    udynlink_load_mode_t load_mode,
    void *work_buf, uint32_t work_buf_size);
```

**New signature:**
```c
udynlink_error_t udynlink_load_module_from_stream(
    udynlink_module_t *p_mod, const udynlink_io_t *p_io,
    void *load_addr, uint32_t load_size,
    udynlink_load_mode_t load_mode,
    void *scratch_buf, uint32_t scratch_buf_size);
```

Same parameter count, same position. `scratch_buf` replaces `work_buf` with different semantics:
- `work_buf` was an I/O intermediate (read into work_buf, memcpy to dest)
- `scratch_buf` holds temp state (header, names, reloc data) that was on the stack

### 2. Eliminate Double-Copy: Read Directly to Dest

**`stream_read_exact()`** simplified to read directly into dest:
```c
static int32_t stream_read_exact(const udynlink_io_t *p_io, void *dest,
                                 uint32_t offset, uint32_t len) {
    uint8_t *d = (uint8_t *)dest;
    uint32_t pos = 0;
    while (pos < len) {
        int32_t n = p_io->read(p_io->pv_ctx, d + pos, len - pos, offset + pos);
        if (n <= 0) return -1;
        pos += (uint32_t)n;
    }
    return (int32_t)len;
}
```

The loop handles partial reads. If the callback always returns exact-length reads, this collapses to a single call.

**`stream_read_string()`** reads directly into dest and scans for NUL in-place:
```c
static int32_t stream_read_string(const udynlink_io_t *p_io, char *dest,
                                  uint32_t offset, uint32_t max_len) {
    if (max_len < 2) return -1;
    uint32_t total_read = 0;
    uint32_t cur_offset = offset;
    while (total_read + 1 < max_len) {
        uint32_t chunk = max_len - total_read - 1;
        int32_t n = p_io->read(p_io->pv_ctx, dest + total_read, chunk, cur_offset);
        if (n <= 0) return -1;
        for (int32_t i = 0; i < n; i++) {
            if (dest[total_read + i] == '\0') return (int32_t)(total_read + i + 1);
        }
        total_read += (uint32_t)n;
        cur_offset += (uint32_t)n;
        if ((uint32_t)n < chunk) return -1;
    }
    dest[total_read] = '\0';
    return -1;
}
```

Both helpers drop to 4 params: `(p_io, dest, offset, len/max_len)`.

### 3. Scratch Buffer Layout (Internal, Phased Reuse)

```
Offset 0..35:    udynlink_module_header_t header  (needed throughout entire function)
Offset 36..99:   char name_buf[64]                 (reused for dep_name AND sym_name, phased)
Offset 100..107: uint32_t reloc_pair[2]            (per-reloc iteration, reused)
Offset 108..111: uint32_t sym_count                (per-reloc iteration, reused)
Offset 112..119: uint32_t sym_entry[2]             (per-reloc iteration, reused)
Offset 120..131: udynlink_sym_t dep_sym            (per-extern-reloc, reused)
```

**Minimum scratch size: 132 bytes** -> `UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE 132`

**Phased reuse rules:**
- `name_buf` (offset 36): used for `dep_name` during dependency resolution, then reused for `sym_name` during relocation. These phases never overlap.
- All per-iteration variables (reloc_pair through dep_sym) are at separate offsets for natural alignment. They're reused across iterations.

**Validation:** `_Static_assert` in `udynlink.c` to catch drift against struct sizes.

### 4. Removed Constants

- `UDYNLINK_STREAM_BUF_SIZE` (512) -> removed (never referenced in C code)
- `UDYNLINK_STREAM_MIN_WORK_BUF_SIZE` (64) -> replaced by `UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE` (132)

### 5. `read` Callback Contract Update

The `read` callback documentation should note that `buf` may point to any writable address (including module RAM, scratch buffer, or stack). The callback must write data directly to `buf`. This is already the de facto contract.

Block-device stream drivers that require sector-aligned buffers must internally buffer sectors and copy to `buf`.

## Task Breakdown

### Task 1: Refactor streaming helpers - remove work_buf, read directly to dest
**Files:** `udynlink/udynlink.c`
- Simplify `stream_read_exact()`: remove `work_buf`/`work_buf_size` params, read directly into `dest`
- Simplify `stream_read_string()`: remove `work_buf`/`work_buf_size` params, read directly into `dest` and scan in-place for NUL
- Handle partial reads with a loop in `stream_read_exact()`
- Both helpers drop to 4 params: `(p_io, dest, offset, len/max_len)`

### Task 2: Update API surface - replace work_buf with scratch_buf
**Files:** `udynlink/udynlink.h`
- Replace `work_buf`/`work_buf_size` with `scratch_buf`/`scratch_buf_size` in `udynlink_load_module_from_stream()` signature
- Remove `UDYNLINK_STREAM_BUF_SIZE` and `UDYNLINK_STREAM_MIN_WORK_BUF_SIZE`
- Add `UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE 132`
- Update doc comments: new `read` callback contract, scratch_buf semantics

### Task 3: Refactor `udynlink_load_module_from_stream()` to use scratch_buf and direct reads
**Files:** `udynlink/udynlink.c`
- Replace `work_buf`/`work_buf_size` params with `scratch_buf`/`scratch_buf_size`
- Add scratch_buf validation (NULL check, min size check)
- Replace stack `header` -> `(udynlink_module_header_t*)scratch_buf`
- Replace stack `dep_name[64]` -> `(char*)(scratch_buf + 36)`
- Replace stack `sym_name[64]` -> `(char*)(scratch_buf + 36)` (phased reuse)
- Replace stack `rel_pair[2]` -> `(uint32_t*)(scratch_buf + 100)`
- Replace stack `sym_count` -> `(uint32_t*)(scratch_buf + 108)`
- Replace stack `sym_entry[2]` -> `(uint32_t*)(scratch_buf + 112)`
- Replace stack `dep_sym` -> `(udynlink_sym_t*)(scratch_buf + 120)`
- Update all calls to `stream_read_exact()` and `stream_read_string()` (remove work_buf args, pass scratch offsets as dest)
- Add `_Static_assert` for scratch size validation
- Update error handling paths

### Task 4: Update test harness
**Files:** `tests/test-streaming-load/test_qemu.c`
- Remove `work_buf[512]` and `work_buf[128]` stack allocations
- Add scratch buffer to `test_streaming_load()` (min 132 bytes)
- Pass scratch_buf to `udynlink_load_module_from_stream()`
- Remove work_buf_size from test function signatures
- Add a test case with minimum scratch_buf size (132 bytes)
- Keep testing all load modes

### Task 5: Run full test suite
- `just test-f429` or equivalent
- Verify all streaming tests pass at both `-O0` and `-Os`

## Context Guide for Implementation Agents

Key files:
- `udynlink/udynlink.h` - Public API (streaming section: line 273-559)
- `udynlink/udynlink.c` - Implementation (helpers: line 246-282, main function: line 678-943)
- `tests/test-streaming-load/test_qemu.c` - Test harness

The scratch buffer replaces ALL stack-allocated buffers. The phased reuse is safe because:
1. Dependency resolution completes before relocation processing
2. Within dependency resolution, `dep_name` is used one at a time (sequential)
3. Within relocation, `sym_name` is used one at a time (sequential)
4. No two name-buffer uses overlap in time

The direct-read approach is safe because:
1. The `read` callback already writes directly to the provided buffer
2. Block-device drivers manage their own alignment internally
3. The existing `work_buf` provided no alignment guarantee anyway
