# Plan: I/O Transform Hooks for Streaming Load

## Goal

Add hooks into the streaming I/O loading path to support:
1. **Compressed module images** (user-provided decompression)
2. **Byte-stream transformations** (ECC per-frame/page for lossy media like LoRa, encryption, etc.)
3. **Lifecycle observability** during streaming load (verification, logging, progress)

## Architectural Decision: Combined Layered I/O + Lifecycle Hooks

### Why this approach

Three approaches were considered:

| Approach | Decompression | ECC/Verification | Code Impact | Safety |
|----------|--------------|-----------------|-------------|--------|
| A. Transform callback on `io_t` | Chunk boundary issues, loader must manage residue state | Decent | Medium (loader internal changes) | Low — in-place mutation |
| B. Layered I/O (wrap `io_t` in `io_t`) | Clean — user manages full read contract | Awkward — wrapper must know when load is "done" | Zero (no loader changes) | High — isolated to wrapper |
| **C. Combined** (B for transforms + lifecycle hooks) | **Clean** — same as B | **Simple** — hook observes lifecycle events | **Low** (~30-40 lines) | **High** — both layers isolated |

**Chosen: Approach C.** Layered I/O handles byte-transforming use cases (decompression, decryption, per-page ECC). Lifecycle hooks handle logical verification (signature check, progress reporting).

### Key Design Insight: Offset Semantics

The current `udynlink_read_cb_t(ctx, buf, num_bytes, offset)` uses **absolute byte offsets**. This is critical for the layered I/O approach:

- **Block-based transforms** (compress per-256B-page, ECC per-sector) work naturally — the wrapper translates logical offsets into physical block indices, reads+decodes, and returns the logical bytes.
- **Global streaming compression** (single LZ4 stream) does NOT work with random-access offsets. This is a fundamental limitation but acceptable: embedded systems typically compress per-block anyway (flash pages, LoRa frames).

The layered I/O approach has **zero impact** on the loader because `udynlink_io_t` is unchanged — the user simply passes a wrapped `io_t` instead of the raw one.

### User-defined Compressed Format

No new header signature (`UDLC`) or `mkmodule` changes. The user is entirely responsible for:
- Their own container format (e.g., a small header before the UDLM image)
- Their own decompression wrapper that strips the container and exposes clean UDLM data through the `udynlink_io_t` interface

### Lifecycle Hooks: Streaming Path Only

Hooks are only on `udynlink_load_module_from_stream_ex()`. The memory-mapped path works with direct pointers — verification can happen before calling load.

---

## API Design

### 1. Lifecycle Hook Types

```c
typedef enum {
    UDYNLINK_HOOK_HEADER_PARSED,     // Header read & validated, RAM not yet allocated
    UDYNLINK_HOOK_DEPS_RESOLVED,     // Dependencies verified, RAM allocated but empty
    UDYNLINK_HOOK_SECTIONS_LOADED,   // Code+data copied to RAM, BSS zeroed
    UDYNLINK_HOOK_RELOCS_APPLIED,    // All relocations processed
} udynlink_hook_stage_t;

typedef udynlink_error_t (*udynlink_hook_cb_t)(
    udynlink_hook_stage_t stage,
    udynlink_module_t *p_mod,
    void *pv_hook_ctx
);

typedef struct {
    udynlink_hook_cb_t  on_event;    // Single callback for all stages (saves struct size)
    void               *pv_hook_ctx; // User context
} udynlink_load_hooks_t;
```

**Design choices:**
- Single callback + stage enum instead of one callback per stage — saves 12 bytes of struct on Cortex-M
- Return != UDYNLINK_OK to abort the load immediately (error is propagated to caller)
- `p_mod` is partially populated at each stage — the hook can inspect what's available
- `NULL` hooks pointer means "no hooks" — backward compatible

### 2. Extended Streaming Load Function

```c
udynlink_error_t udynlink_load_module_from_stream_ex(
    udynlink_module_t *p_mod,
    const udynlink_io_t *p_io,
    void *load_addr, size_t load_size,
    udynlink_load_mode_t load_mode,
    void *work_buf, size_t work_buf_size,
    const udynlink_load_hooks_t *p_hooks   // NEW — NULL for no hooks
);
```

The original `udynlink_load_module_from_stream()` becomes a thin wrapper:
```c
udynlink_error_t udynlink_load_module_from_stream(...) {
    return udynlink_load_module_from_stream_ex(..., NULL);
}
```

### 3. Error Reporting

When a hook returns non-OK, the loader returns `UDYNLINK_ERR_LOAD_HOOK_ABORTED`. The specific stage that triggered the abort is identifiable from the error code plus the caller's own hook logic (same developer controls both sides). Any additional error detail should be communicated out-of-band through the hook context — the `pv_hook_ctx` can hold a status variable that the hook writes and the caller reads after the load fails.

### 4. Layered I/O — No API Changes

`udynlink_io_t` remains unchanged. Users create wrappers in their own code. We provide **example wrapper patterns** in documentation but no library code (to avoid pulling in decompression libs into the core).

Example pattern:
```c
typedef struct {
    udynlink_io_t *p_raw;
    uint8_t page_buf[256];
    uint32_t cached_page;
    // ... decompression/ECC state
} my_wrapper_ctx_t;

int32_t my_wrapper_read(void *pv_ctx, void *buf, uint32_t num_bytes, uint32_t offset) {
    my_wrapper_ctx_t *ctx = (my_wrapper_ctx_t *)pv_ctx;
    // Translate logical offset → physical page, decompress, ECC-correct, copy to buf
    ...
}
```

---

## Hook Stage Details

### UDYNLINK_HOOK_HEADER_PARSED
- **When**: After signature, ABI version, and architecture checks pass
- **`p_mod` state**: `p_header` points to a **stack-local copy** of the header — valid for the duration of the hook callback only. Do not store this pointer. `p_ram` is NULL. Load mode is set.
- **Use cases**: Pre-alloc verification, module allowlisting by name/size, logging

### UDYNLINK_HOOK_DEPS_RESOLVED
- **When**: After dependency modules found and `dep_refcount` incremented
- **`p_mod` state**: `p_header` valid, `p_ram` allocated (or set to load_addr), deps resolved, sections NOT loaded yet
- **Use cases**: Dependency policy enforcement, allocating per-dependency resources

### UDYNLINK_HOOK_SECTIONS_LOADED
- **When**: After code+data copied to RAM, BSS zeroed, before relocations
- **`p_mod` state**: Code and data pointers valid, LOT unmapped, relocations NOT applied
- **Use cases**: Integrity verification of loaded code/data, code signing check

### UDYNLINK_HOOK_RELOCS_APPLIED
- **When**: After all relocations processed, before function returns
- **`p_mod` state**: Fully loaded and relocated, ready to use
- **Use cases**: Post-link audit, logging

---

## Implementation Plan

### Task 1: Add hook types and `_ex` function to header
**File**: `udynlink/udynlink.h`
- Add `udynlink_hook_stage_t` enum
- Add `udynlink_hook_cb_t` typedef
- Add `udynlink_load_hooks_t` struct
- Add `UDYNLINK_ERR_LOAD_HOOK_ABORTED` to error codes
- Declare `udynlink_load_module_from_stream_ex()`
- Add doc comments

**Deliverable**: Header compiles cleanly with existing code (new function just declared, not defined yet).

### Task 2: Refactor streaming loader to use internal `_ex` implementation
**File**: `udynlink/udynlink.c`
- Rename current `udynlink_load_module_from_stream()` body to internal `udynlink_load_module_from_stream_impl()` that takes `p_hooks`
- Insert 4 hook invocations at the correct points (guarded by `if (p_hooks && p_hooks->on_event)`)
- Make original function a thin wrapper calling `_impl` with `NULL` hooks
- Add `udynlink_load_module_from_stream_ex()` as public wrapper

**Key insertion points in the streaming loader (from current `udynlink.c` lines ~744–1052):**

1. **`HEADER_PARSED`** — after arch tag check passes (line ~800), before dep resolution
2. **`DEPS_RESOLVED`** — after dep loop completes (line ~847) and RAM allocated (line ~868), before section copy begins
3. **`SECTIONS_LOADED`** — after BSS zeroing (line ~900), before LOT/relocation processing
4. **`RELOCS_APPLIED`** — after relocation loop completes (line ~1037), before `exit` label

**Important streaming loader details for implementers:**

The streaming loader is significantly different from the memory-mapped loader. Key behaviors:

- **Header lives in scratch buffer**: `header` is `(udynlink_module_header_t *)scratch_buf` (line 760). After sections are copied, `p_mod->p_header` is set to point into the copied RAM (lines 881, 895). At `HEADER_PARSED` hook time, `p_mod->p_header` still points to scratch — document this lifetime.
- **COPY_ALL mode**: copies header+relocs+symtab+code+data in ONE `stream_read_exact` call (line 877). `p_mod->p_header` is set after this copy.
- **COPY_TEXT_DATA mode**: copies relocs+symtab first (line 885), then code+data (line 889). Then normalizes to COPY_ALL mode (line 897). `p_mod->p_header` is set after both copies.
- **Relocations are read per-pair from stream**: each reloc pair is fetched via `stream_read_exact` (line 911). Symbol entries are also fetched on-demand from stream. This means the relocation loop does NOT read from RAM — the stream `p_io` must remain valid throughout.
- **Symbol names read from stream**: weak and extern symbol names are fetched from the stream during relocation processing (lines 976, 1003), NOT from a RAM copy.
- **Cleanup at `exit`**: checks `UDYNLINK_LOAD_IS_STREAM_HDR` flag to decide whether to free `p_header` or `p_ram` (line 1044). Hook abort must jump to this cleanup path.

**Deliverable**: Refactored loader, all existing tests still pass, hooks are called at correct points.

### Task 3: Add test for lifecycle hooks
**File**: `tests/test-streaming-hooks/test_qemu.c`
- Create a new test directory with a test that exercises all 4 hook stages
- Verify hooks are called in order
- Verify hook can abort loading at each stage
- Verify `p_mod` state at each stage (e.g., `p_ram` is NULL at HEADER_PARSED but set at SECTIONS_LOADED)
- Verify `NULL` hooks pointer works (equivalent to original function)

**Deliverable**: New test that passes on QEMU.

### Task 4: Add layered I/O wrapper example/documentation
- Add a well-documented example showing how to create a decompression wrapper and an ECC wrapper
- Include in `docs/` or as a header comment block
- No new library code — examples only

**Deliverable**: Example code that compiles standalone.

### Task 5: Update `test_data.py` and test harness
**File**: `tests/test-streaming-hooks/test_data.py`
- Add test expectations for the new streaming hooks test

**Deliverable**: Test runs correctly via `just test-f429 test-streaming-hooks`.

---

## Task Dependency Graph

```
Task 1 (header) ──► Task 2 (implementation) ──► Task 3 (tests)
                                                │
                                                └──► Task 5 (test harness)
Task 4 (docs/examples) ── independent
```

Tasks 1 and 2 are sequential (header must exist before implementation).
Task 3 depends on Task 2.
Task 4 is independent (can run in parallel).
Task 5 can run in parallel with Task 3 but must be ready before final validation.

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Hook callback crashes in interrupt context | Document that hooks must be re-entrant; user responsibility |
| Hook modifies `p_mod` state causing loader corruption | Document that `p_mod` fields are read-only during hooks; no enforcement (would add code size) |
| `HEADER_PARSED` hook sees stack-local header | Document pointer lifetime: valid only during callback, do not store. Header contents are safe to copy. |
| Existing tests break from refactoring | Task 2 requires all 24 existing tests to pass before proceeding |
| `UDYNLINK_ERR_LOAD_HOOK_ABORTED` leaks resources | Use existing cleanup path (`goto exit`) — hooks returning error just set `res` and jump to cleanup |

## Resolved Design Questions

1. **`HEADER_PARSED` hook sees stack-local header** — Document the pointer lifetime clearly: valid only during the hook callback, do not store. The header data itself (the struct contents) is safe to read and copy.

2. **`DEPS_RESOLVED` merged with RAM allocation** — Kept as a single stage. A hook between deps-resolved and RAM-allocated would let you veto after knowing deps, but `HEADER_PARSED` already enables veto (all size/version info is in the header, and dep names can be read from raw I/O if needed before committing). Splitting adds code size for no practical gain.

3. **Hook error specificity** — Return `UDYNLINK_ERR_LOAD_HOOK_ABORTED` to indicate a hook aborted. The developer controls both the hook and the caller, so additional error detail goes out-of-band through `pv_hook_ctx`.
