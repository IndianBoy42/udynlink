# Loader Hardening: Robust Module Validation

## Problem Statement

The udynlink loader trusts the module header implicitly, making it highly vulnerable to malformed or malicious binaries:

1. **No Bounds Checking on Relocations** — `lot_offset` from the relocation table used directly to compute write targets without validation
2. **Integer Overflows in Allocation** — `get_ram_size_for_header()` uses 32-bit arithmetic with no overflow checks
3. **Missing Integrity Verification** — Beyond a 4-byte `UDLM` magic and architecture tag, there is no checksum

## Design: Incremental Tools & Hooks, Not a Toggle

**No monolithic `UDYNLINK_HARDENING` toggle.** Instead: expose composable public APIs the host uses as needed. A host that trusts its modules calls nothing extra and pays zero cost. A zero-trust host picks exactly the checks it needs.

Three layers:

1. **Pre-flight validation functions** — public functions the host calls on the raw image *before* loading. Each is independent; the host composes them.
2. **Load hooks** — the `udynlink_load_hooks_t` / `udynlink_hook_stage_t` mechanism already exists for the streaming path. Extend it to the memory-mapped path via `udynlink_load_module_ex()`. The host can call the public validation functions from inside hooks for inline interception.
3. **Utility functions** — safe arithmetic, CRC32 — building blocks the host (or the validation functions) use internally, also available for custom host logic.

### Zero-Cost Guarantee

- `udynlink_load_module()` keeps its **original 5-parameter signature** — unchanged
- Validation functions are compiled but linker-strippable (`-ffunction-sections --gc-sections`)
- CRC32 lookup table lives in its own compilation unit — stripped if nothing calls it
- No new mandatory parameters, no `#if` walls in the core loader, no conditional API

### Host Usage Patterns

**Trusted (current, zero change):**
```c
udynlink_load_module(&mod, base, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
```

**Pre-flight validation (pick & choose):**
```c
udynlink_error_t res;
res = udynlink_validate_header((const udynlink_module_header_t *)base);
if (res != UDYNLINK_OK) return res;
res = udynlink_validate_image(base, sizeof(module_data));
if (res != UDYNLINK_OK) return res;
res = udynlink_validate_relocations(base);
if (res != UDYNLINK_OK) return res;
res = udynlink_verify_crc32(base);
if (res != UDYNLINK_OK) return res;
res = udynlink_load_module(&mod, base, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
```

**Inline validation via hooks (for the memory-mapped path):**
```c
static udynlink_error_t on_header(udynlink_hook_stage_t stage,
                                   udynlink_module_t *p_mod,
                                   void *ctx) {
    return udynlink_validate_header(p_mod->p_header);
}

udynlink_load_hooks_t hooks = { .on_event = on_header, .pv_hook_ctx = NULL };
udynlink_error_t res = udynlink_load_module_ex(
    &mod, base, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL, &hooks);
```

**Custom host checks (using the utilities directly):**
```c
// Host implements its own integrity scheme
uint32_t my_hash = udynlink_crc32(base, image_len, 0);
if (my_hash != expected) return MY_ERR_INTEGRITY;
// Host does its own overflow-safe size math
uint32_t safe_size;
if (!udynlink_safe_add(code_size, data_size, &safe_size)) return MY_ERR_OVERFLOW;
```

---

## Existing Infrastructure (Do Not Reinvent)

The streaming path already has hooks:
- `udynlink_load_module_from_stream_ex()` takes `const udynlink_load_hooks_t *p_hooks`
- `udynlink_hook_stage_t`: `HEADER_PARSED`, `DEPS_RESOLVED`, `SECTIONS_LOADED`, `RELOCS_APPLIED`
- `UDYNLINK_ERR_LOAD_HOOK_ABORTED` already exists
- Hook callback signature: `udynlink_error_t (*udynlink_hook_cb_t)(udynlink_hook_stage_t, udynlink_module_t*, void*)`

The memory-mapped path has no `_ex` variant. We add one (mirrors the streaming pattern exactly).

---

## New Public API

### Validation Functions

```c
// Validate header field limits & safe arithmetic (overflow, bounds)
// Reads only the header — no image_size needed
udynlink_error_t udynlink_validate_header(const udynlink_module_header_t *p_header);

// Validate that header section sizes are consistent with the actual image
// image_size=0 is valid (skip the image bounds check, just check internal consistency)
udynlink_error_t udynlink_validate_image(const void *base_addr, size_t image_size);

// Validate all relocation lot_offsets against header bounds
// Reads the relocation table from the image — no writes
udynlink_error_t udynlink_validate_relocations(const void *base_addr);

// Verify CRC32 checksum (v2.1+ modules only)
// No-op (returns UDYNLINK_OK) for v1.0/v2.0 modules
udynlink_error_t udynlink_verify_crc32(const void *base_addr);
```

### Utility Functions

```c
// Safe 32-bit arithmetic — overflow returns false
bool udynlink_safe_add_u32(uint32_t a, uint32_t b, uint32_t *result);
bool udynlink_safe_mul_u32(uint32_t a, uint32_t b, uint32_t *result);

// CRC32 (ISO 3309) — incremental: init=0 for first chunk, chain for subsequent
uint32_t udynlink_crc32(const void *data, size_t length, uint32_t init);
```

### Extended Load Function

```c
// Memory-mapped load with hooks — mirrors udynlink_load_module_from_stream_ex()
// p_hooks=NULL is equivalent to calling udynlink_load_module()
udynlink_error_t udynlink_load_module_ex(
    udynlink_module_t *p_mod,
    const void *base_addr,
    void *load_addr,
    size_t load_size,
    udynlink_load_mode_t load_mode,
    const udynlink_load_hooks_t *p_hooks);

// Original function becomes a thin wrapper:
udynlink_error_t udynlink_load_module(
    udynlink_module_t *p_mod,
    const void *base_addr,
    void *load_addr,
    size_t load_size,
    udynlink_load_mode_t load_mode);
```

### New Error Codes

Appended to `UDYNLINK_ERROR_CODES` (no existing values shift):

```c
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_HEADER_INVALID),
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_CRC_MISMATCH),
```

### Configurable Limits

Same `#ifndef` pattern as `UDYNLINK_MAX_DEPS` and `UDYNLINK_MAX_HANDLES`. Only used by `udynlink_validate_header()`:

```c
#ifndef UDYNLINK_MAX_LOT_ENTRIES
#define UDYNLINK_MAX_LOT_ENTRIES       1024
#endif
#ifndef UDYNLINK_MAX_RELOCATIONS
#define UDYNLINK_MAX_RELOCATIONS       4096
#endif
#ifndef UDYNLINK_MAX_SYMT_SIZE
#define UDYNLINK_MAX_SYMT_SIZE         16384
#endif
#ifndef UDYNLINK_MAX_CODE_SIZE
#define UDYNLINK_MAX_CODE_SIZE         (1024 * 1024)
#endif
#ifndef UDYNLINK_MAX_DATA_SIZE
#define UDYNLINK_MAX_DATA_SIZE         (256 * 1024)
#endif
#ifndef UDYNLINK_MAX_BSS_SIZE
#define UDYNLINK_MAX_BSS_SIZE          (256 * 1024)
#endif
#ifndef UDYNLINK_MAX_DEPS_STRTAB_SIZE
#define UDYNLINK_MAX_DEPS_STRTAB_SIZE  1024
#endif
```

### Header Change (v2.1+)

```c
typedef struct {
    // ... existing 36 bytes (unchanged) ...
    uint32_t crc32;   // CRC32 over entire image EXCLUDING this 4-byte field
    // Total: 40 bytes for v2.1+, 36 for v2.0, 32 for v1.0
} udynlink_module_header_t;
```

`get_header_size()` (internal) already dispatches on version. Looks at `udynlink_version < 2.0 → 32`, `< 2.1 → 36`, otherwise `sizeof(udynlink_module_header_t)` = 40.

---

## Implementation Plan

### Task 1: Public Validation & Utility API + `udynlink_load_module_ex()`

**Files:** `udynlink/udynlink.c`, `udynlink/udynlink.h`

**Goal:** Add the public validation functions, safe arithmetic utilities, `udynlink_load_module_ex()`, and the configurable limit `#define`s. These are the building blocks.

**Changes:**

1. **`udynlink.h` — Configurable limit `#define`s** (see above). Same `#ifndef` pattern as `UDYNLINK_MAX_DEPS`.

2. **`udynlink.h` — New error codes** appended to `UDYNLINK_ERROR_CODES`:
   ```c
   _UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_HEADER_INVALID),
   _UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_CRC_MISMATCH),
   ```

3. **`udynlink.h` — Public function declarations** (see "New Public API" above).

4. **`udynlink.c` — Safe arithmetic implementations:**
   ```c
   bool udynlink_safe_add_u32(uint32_t a, uint32_t b, uint32_t *result) {
       if (a > UINT32_MAX - b) return false;
       *result = a + b;
       return true;
   }
   bool udynlink_safe_mul_u32(uint32_t a, uint32_t b, uint32_t *result) {
       if (b != 0 && a > UINT32_MAX / b) return false;
       *result = a * b;
       return true;
   }
   ```

5. **`udynlink.c` — `udynlink_validate_header()` implementation:**
   - Check each field against its `UDYNLINK_MAX_*` limit
   - Use `udynlink_safe_mul_u32()` / `udynlink_safe_add_u32()` to verify `num_lot * 4 + data_size + bss_size` doesn't overflow
   - Return `UDYNLINK_ERR_LOAD_HEADER_INVALID` on failure

6. **`udynlink.c` — `udynlink_validate_image()` implementation:**
   - Read the header, call `udynlink_validate_header()` internally
   - Compute `get_code_offset_from_header() + code_size + data_size` using safe arithmetic
   - If `image_size > 0`, verify `total <= image_size`
   - Also check internal consistency: reloc table + symtab + deps strtab must fit within the computed total

7. **`udynlink.c` — `udynlink_validate_relocations()` implementation:**
   - Read the header from `base_addr`
   - Call `udynlink_validate_header()` internally
   - Iterate over each relocation entry in the image (at the known offset)
   - For each `(lot_offset, symt_offset)` pair, validate:
     - `lot_offset < num_lot` → LOT write (always safe since we validated `num_lot`)
     - `lot_offset >= num_lot` → data write: check `(lot_offset - num_lot) * 4 < data_size + bss_size`
   - Return `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` on failure

8. **`udynlink.c` — `udynlink_load_module_ex()` implementation:**
   - Refactor `udynlink_load_module()` body into a static `load_module_impl()` that takes the hooks parameter
   - `udynlink_load_module()` becomes: `return load_module_impl(p_mod, base_addr, load_addr, load_size, load_mode, NULL);`
   - `udynlink_load_module_ex()` becomes: `return load_module_impl(p_mod, base_addr, load_addr, load_size, load_mode, p_hooks);`
   - Inside `load_module_impl()`, call the hook at each stage (like `udynlink_load_module_from_stream_ex()` already does):
     - After header parsed → call hook with `UDYNLINK_HOOK_HEADER_PARSED`
     - After deps resolved → call hook with `UDYNLINK_HOOK_DEPS_RESOLVED`
     - After sections loaded → call hook with `UDYNLINK_HOOK_SECTIONS_LOADED`
     - After relocs applied → call hook with `UDYNLINK_HOOK_RELOCS_APPLIED`
   - If hook returns non-`UDYNLINK_OK`, abort with `UDYNLINK_ERR_LOAD_HOOK_ABORTED`

9. **No changes to the streaming path** — `udynlink_load_module_from_stream_ex()` already has hooks. The host can call the public validation functions from inside those hooks.

**Deliverable:** Public validation API, safe arithmetic, `udynlink_load_module_ex()`, configurable limits. Original `udynlink_load_module()` unchanged. All existing tests pass.

**Agent:** `agent` — Multi-file foundational change

---

### Task 2: CRC32 Utility & Verification

**Files:** `udynlink/udynlink_crc32.c` (new), `udynlink/udynlink_crc32.h` (new), `udynlink/udynlink.c`, `udynlink/udynlink.h`, `scripts/mkmodule`, `CMakeLists.txt`

**Goal:** Add CRC32 utility function and `udynlink_verify_crc32()` validation function. Add `crc32` field to the header struct.

**Changes:**

1. **`udynlink/udynlink.h` — Extend `udynlink_module_header_t`:**
   ```c
   typedef struct {
       // ... existing 36 bytes ...
       uint32_t crc32;   // CRC32 over image[0..35] + image[40..end]
   } udynlink_module_header_t;
   ```

2. **Update `get_header_size()` (internal in `udynlink.c`):**
   ```c
   static size_t get_header_size(const udynlink_module_header_t *p_header) {
       if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 0)) return 32;
       if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 1)) return 36;
       return sizeof(udynlink_module_header_t);  // 40
   }
   ```

3. **`udynlink/udynlink_crc32.c` + `udynlink/udynlink_crc32.h`:**
   - Standard CRC32 (ISO 3309 / ITU-T V.42) using 256-byte lookup table
   - API: `uint32_t udynlink_crc32(const void *data, size_t length, uint32_t init)`
   - Incremental: first call with `init=0`, chain with previous return
   - Own compilation unit — linker strips it if nothing calls `udynlink_crc32()` or `udynlink_verify_crc32()`

4. **`udynlink.c` — `udynlink_verify_crc32()` implementation:**
   - Read header, check `udynlink_version >= 2.1`
   - If `< 2.1`: return `UDYNLINK_OK` (no CRC to check)
   - If `>= 2.1`: compute CRC32 over `image[0..35] + image[40..end]` (skip the 4-byte CRC field at offset 36-39)
   - Compare with `p_header->crc32`
   - Return `UDYNLINK_ERR_LOAD_CRC_MISMATCH` on failure
   - Uses `udynlink_crc32()` from the utility module

5. **`CMakeLists.txt` — Add `udynlink_crc32.c` to the library sources**

6. **`scripts/mkmodule` — Generate CRC32 in v2.1+ images:**
   - After building the binary image, compute CRC32 over `img[0..35] + img[40..end]`
   - Write CRC to `img[36..39]`
   - New CLI flag `--udynlink-version 2.1` generates 40-byte headers with CRC
   - Default `--udynlink-version` remains `2.0` for backward compat

7. **All existing query functions that use `get_header_size()` / `get_code_offset_from_header()` automatically handle v2.1 headers** because they already dispatch on version.

**Deliverable:** CRC32 utility, `udynlink_verify_crc32()`, header struct extended. mkmodule can generate v2.1 images. Zero cost when not called. All existing tests pass.

**Agent:** `expert` — CRC32 implementation, header layout change, streaming CRC, mkmodule Python changes

---

### Task 3: Hardening Test Suite

**Files:** `tests/test-hardening/` (new directory with `test_qemu.c`, `test_data.py`)

**Depends on:** Tasks 1 & 2

**Goal:** Verify:
1. Validation functions correctly accept valid modules and reject malformed ones
2. Hook-based loading correctly intercepts bad modules
3. Original `udynlink_load_module()` still works unchanged for valid modules
4. CRC32 verification works for v2.1+ probes

**Test cases:**
1. `udynlink_validate_header()` accepts a valid module → `UDYNLINK_OK`
2. `udynlink_validate_header()` rejects `num_lot = 0xFFFF` → `UDYNLINK_ERR_LOAD_HEADER_INVALID`
3. `udynlink_validate_header()` rejects `data_size = 0xFFFFFFFF` → `UDYNLINK_ERR_LOAD_HEADER_INVALID`
4. `udynlink_validate_image()` rejects image size mismatch → `UDYNLINK_ERR_LOAD_HEADER_INVALID`
5. `udynlink_validate_relocations()` rejects OOB `lot_offset` → `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE`
6. `udynlink_verify_crc32()` rejects corrupted CRC → `UDYNLINK_ERR_LOAD_CRC_MISMATCH`
7. `udynlink_verify_crc32()` accepts v2.0 module (no CRC) → `UDYNLINK_OK`
8. `udynlink_load_module_ex()` with header-validation hook rejects bad module → `UDYNLINK_ERR_LOAD_HOOK_ABORTED`
9. `udynlink_load_module()` (original) accepts valid module → `UDYNLINK_OK`
10. `udynlink_load_module()` (original) rejects invalid signature → `UDYNLINK_ERR_LOAD_INVALID_SIGN`

**Implementation approach:**
- Malformed blobs as `static const uint8_t` arrays in `test_qemu.c`
- Base blob: copy a real module's data and mutate specific fields at known offsets
- Run on standard test platform (MPS2-AN386 or STM32F429)

**Deliverable:** New `test-hardening` test covering all validation functions and hooks. Existing tests unchanged.

**Agent:** `agent` — Test writing, test harness integration

---

## Execution Order

```
Task 1: Public Validation API + ex()  ─────┐
                                             │
Task 2: CRC32 + Header Extension  ─────────┤  (depends on Task 1:
                                             │   uses udynlink_validate_header
                                             │   internals, get_header_size)

Task 3: Hardening Test Suite  ──────────────┘  (depends on Tasks 1 & 2)
```

Tasks 1 and 2 are sequential (Task 2 extends Task 1's header handling). Task 3 validates everything.

---

## Agent Assignment

| Task | Agent Type | Rationale |
|------|-----------|-----------|
| Task 1 | `agent` | Multi-file: new public API, validation logic, refactoring load_module into _ex — needs judgment |
| Task 2 | `expert` | CRC32 implementation, header layout change, mkmodule Python changes, streaming path — most complex |
| Task 3 | `agent` | Test writing and integration |

---

## Context for Implementation Agents

### Key Files to Read First
- `udynlink/udynlink.c` — Core loader. The only file that changes significantly.
- `udynlink/udynlink.h` — Public API, types, error codes. Where new declarations go.
- `codemap.md` (project root) — Full repository map

### Existing Patterns to Follow
- `udynlink_load_module_from_stream_ex()` — the `_ex` variant with hooks. Mirror this pattern for `udynlink_load_module_ex()`.
- `UDYNLINK_MAX_DEPS` / `UDYNLINK_MAX_HANDLES` — `#ifndef` pattern for configurable constants
- `UDYNLINK_ERROR_CODES` X-macro — append new error codes at the end
- `udynlink_externals.h` — weak symbol pattern for optional host callbacks

### Critical Internal Functions (udynlink.c)
- `get_header_size()` (line ~88) — dispatches on version, needs 40-byte case for v2.1
- `get_code_offset_from_header()` (line ~92) — computes code section offset
- `get_ram_size_for_header()` (line ~198) — RAM size computation (overflow-prone)
- `udynlink_load_module()` (line ~252) — to be refactored into `load_module_impl()`
- `udynlink_load_module_from_stream_ex()` (line ~606) — reference for hooks pattern

### Testing
- Run full suite: `just test-mps2`
- Run single test: `just test-f429-single test-hardening`
- All existing tests must pass after each task
