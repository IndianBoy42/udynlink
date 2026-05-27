# API Reference

## Table of Contents

- [Compile-Time Configuration](#compile-time-configuration)
- [Data Structures](#data-structures)
- [Enumerations](#enumerations)
- [Symbol Constants](#symbol-constants)
- [Architecture Tag Constants and Macros](#architecture-tag-constants-and-macros)
- [Module Loading Functions](#module-loading-functions)
- [Module Query Functions](#module-query-functions)
- [Symbol Lookup Functions](#symbol-lookup-functions)
- [RAM Requirement Functions](#ram-requirement-functions)
- [C++ Support](#c-support)
- [Hash-Based Symbol Resolution](#hash-based-symbol-resolution)
- [Utility Functions](#utility-functions)
- [Convenience Macros](#convenience-macros)
- [External Callbacks](#external-callbacks)

---

## Compile-Time Configuration

These macros control the behavior of the udynlink loader. Some are **required** and must be defined before including `udynlink.h`; others have sensible defaults.

| Macro | Required | Default | Description |
|-------|----------|---------|-------------|
| `UDYNLINK_HOST_ARCH_TAG` | No | `UDYNLINK_ARCH_TAG_CORTEX_M4` | Architecture tag of the host MCU. Used at load time to validate that a module was compiled for a compatible core family and float ABI. |
| `UDYNLINK_LOT_BASE_ADDR` | No | `0x20000000` | Fixed RAM address where the loader writes the current module's `ram_base` before calling any module function. The module's assembly prologue reads this address to set `r9` (the LOT base register). |
| `UDYNLINK_STREAM_BUF_SIZE` | No | `512` | Default work buffer size for streaming I/O operations. |
| `UDYNLINK_MAX_DEPS` | No | `4` | Maximum number of dependencies a single module may declare. Affects the size of `udynlink_module_t`. |

**Example:**

```c
#define UDYNLINK_HOST_ARCH_TAG UDYNLINK_ARCH_TAG_CORTEX_M7
```

---

## Data Structures

### `udynlink_module_header_t`

The binary header that starts every loadable module image. It is followed immediately by the relocation table, symbol table, dependency string table (ABI v2.0+), code, and data.

```c
typedef struct {
    uint32_t  sign;              // "UDLM" signature
    uint16_t  mod_version;       // Module ABI version (major.minor)
    uint16_t  udynlink_version;  // udynlink version used to build the module
    uint16_t  arch_tag;          // Target architecture tag + float ABI
    uint16_t  num_lot;           // Number of Linker Offset Table entries
    uint16_t  num_rels;          // Number of relocations
    uint16_t  num_deps;          // Number of dependencies (0 for v1.0 modules)
    uint32_t  symt_size;         // Size of symbol table in bytes
    uint32_t  code_size;         // Size of .text section in bytes
    uint32_t  data_size;         // Size of .data section in bytes
    uint32_t  bss_size;          // Size of .bss section in bytes
    uint32_t  deps_strtab_size;  // Dependency string table size (0 for v1.0)
} udynlink_module_header_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `sign` | Magic signature `0x55444C4D` ("UDLM"). The loader validates this first. |
| `mod_version` | Module-specific ABI version, packed as `major << 8 | minor`. The host may inspect this to handle incompatible module updates. |
| `udynlink_version` | Version of the toolchain that produced the module. The loader rejects modules with a version greater than `UDYNLINK_LOADER_ABI_VERSION`. |
| `arch_tag` | Encodes the target core family, FPU presence, and float ABI. See [Architecture Tag Constants](#architecture-tag-constants-and-macros). |
| `num_lot` | Number of entries in the Linker Offset Table. The LOT is the first region allocated in module RAM. |
| `num_rels` | Number of `(lot_offset, symt_offset)` relocation pairs. |
| `num_deps` | Number of module dependencies declared at build time (v2.0+). |
| `symt_size` | Size of the symbol table blob in bytes. |
| `code_size` | Size of the `.text` section. |
| `data_size` | Size of the initialized `.data` section. |
| `bss_size` | Size of the zero-initialized `.bss` section. |
| `deps_strtab_size` | Size of the dependency name string table (v2.0+). Padded to 4-byte boundary to keep code alignment. |

**Binary layout after the header:**

```
[Header] [Relocations: num_rels * 8 bytes] [Symbol table: symt_size bytes]
[Dependency strtab: deps_strtab_size bytes (v2.0+)] [Code] [Data]
```

---

### `udynlink_module_t`

A runtime handle representing a loaded module instance. The host allocates this structure and passes it to `udynlink_load_module` or `udynlink_load_module_from_stream`.

```c
typedef struct _udynlink_module_t {
    const udynlink_module_header_t *p_header;  // Pointer to module header
    union {
        void     *p_ram;      // Pointer to module RAM area
        uintptr_t  ram_base;   // Same address as an integer
    };
    uint8_t  info;           // Load mode and RAM ownership flags
    uint8_t  num_deps;       // Number of successfully resolved dependencies
    uint8_t  dep_refcount;   // Number of other modules that depend on this one
    const struct _udynlink_module_t *deps[UDYNLINK_MAX_DEPS];
} udynlink_module_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `p_header` | Points to the module header. In `COPY_ALL` mode this lives in RAM; in `COPY_TEXT_DATA` and `XIP` modes it lives at the original `base_addr`. |
| `p_ram` / `ram_base` | Base address of the RAM region allocated for this module. The LOT starts here. Before calling any module function, the host **must** write `ram_base` to `*(uint32_t *)UDYNLINK_LOT_BASE_ADDR`. |
| `info` | Bitfield storing the [load mode](#udynlink_load_mode_t) and whether the RAM was provided by the host (`FOREIGN_RAM`) or allocated by the loader. |
| `num_deps` | Count of dependencies resolved at load time. |
| `dep_refcount` | Reference count of modules that list this module as a dependency. `udynlink_unload_module` fails with `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` if this is non-zero. |
| `deps[]` | Array of pointers to dependency module handles. Used for inter-module symbol resolution. |

---

### `udynlink_sym_t`

Describes a single symbol entry from a module's symbol table.

```c
typedef struct {
    const char *name;   // Symbol name (NULL for internal symbols)
    uintptr_t    val;    // Symbol value (offset within its section)
    uint8_t     type;   // UDYNLINK_SYM_TYPE_* constant
    uint8_t     location; // UDYNLINK_SYM_LOCATION_* constant
} udynlink_sym_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `name` | Human-readable symbol name. For `UDYNLINK_SYM_TYPE_INTERNAL` this is `"(N/A)"` because internal symbols are nameless in the binary format. |
| `val` | The symbol's raw value. For local/exported symbols this is a section-relative offset; lookup functions relocate it to an absolute address before returning. |
| `type` | One of `UDYNLINK_SYM_TYPE_INTERNAL`, `UDYNLINK_SYM_TYPE_EXPORTED`, `UDYNLINK_SYM_TYPE_EXTERN`, or `UDYNLINK_SYM_TYPE_MODULE_NAME`. |
| `location` | `UDYNLINK_SYM_LOCATION_CODE` if the symbol lives in `.text`, or `UDYNLINK_SYM_LOCATION_DATA` if it lives in `.data`/`.bss`. |

---

### `udynlink_io_t`

Streaming I/O callback structure used by `udynlink_load_module_from_stream`.

```c
typedef int32_t (*udynlink_read_cb_t)(void *pv_ctx, void *buf,
                                      size_t num_bytes, size_t offset);

typedef int32_t (*udynlink_get_size_cb_t)(void *pv_ctx);

typedef struct {
    udynlink_read_cb_t     read;
    udynlink_get_size_cb_t  get_size;
    void                  *pv_ctx;
} udynlink_io_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `read` | Called to read `num_bytes` from `offset` into `buf`. Must return the number of bytes actually read, or `-1` on error. |
| `get_size` | Called to query the total module image size in bytes. Must return the size, or `-1` on error. |
| `pv_ctx` | Opaque context pointer passed back to both callbacks. Typically holds a file handle or SD-card state. |

---

### `udynlink_hash_table_t`

GNU-hash table structure for O(1) host symbol resolution.

```c
typedef struct {
    uint32_t        nbuckets;
    uint32_t        symoffset;
    uint32_t        bloom_size;
    uint32_t        bloom_shift;
    const uint32_t *bloom;
    const uint32_t *buckets;
    const uint32_t *hash_values;
    const uintptr_t *sym_addrs;
    const char     *strtab;
    const size_t *strtab_offsets;
} udynlink_hash_table_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `nbuckets` | Number of hash buckets. |
| `symoffset` | Index of the first global symbol in the sorted symbol array. |
| `bloom_size` | Size of the Bloom filter in 32-bit words (always a power of two). |
| `bloom_shift` | Right-shift amount used for the second Bloom hash (`BLOOM_SHIFT = 26`). |
| `bloom` | Pointer to the Bloom filter word array. |
| `buckets` | Pointer to the bucket array. Each entry holds the first symbol index for that bucket. |
| `hash_values` | Pointer to the array of hashed symbol values (with chain-stop bit). |
| `sym_addrs` | Pointer to the array of symbol addresses, indexed by original symbol order. |
| `strtab` | Pointer to the concatenated null-terminated symbol name string table. |
| `strtab_offsets` | Pointer to the array of byte offsets into `strtab` for each symbol. |

See [Hash-Based Symbol Resolution](#hash-based-symbol-resolution) for usage.

---

## Enumerations

### `udynlink_load_mode_t`

Controls how much of the module image is copied to RAM versus executed in place.

| Value | Name | Description |
|-------|------|-------------|
| `0` | `UDYNLINK_LOAD_MODE_COPY_ALL` | Copy header, relocations, symbol table, code, and data to RAM. Largest RAM footprint; safest if the source flash may be updated later. |
| `1` | `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA` | Copy text and data sections to RAM. The header stays at `base_addr`. Not supported by streaming load (internally converted to `COPY_ALL`). |
| `2` | `UDYNLINK_LOAD_MODE_XIP` | Copy **only** data to RAM; execute code directly from flash at `base_addr`. Smallest RAM footprint. Requires `udynlink_external_is_pointer_in_ram` to verify that code addresses are not in RAM. |

---

### `udynlink_error_t`

Error codes returned by loader functions.

| Value | Name | Description |
|-------|------|-------------|
| `0` | `UDYNLINK_OK` | Success. |
| `1` | `UDYNLINK_ERR_LOAD_INVALID_SIGN` | The module signature does not match "UDLM". |
| `2` | `UDYNLINK_ERR_LOAD_RAM_LEN_LOW` | The caller-provided `load_size` is smaller than the RAM required by the module. |
| `3` | `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY` | `udynlink_external_malloc` returned `NULL`. |
| `4` | `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED` | XIP is not supported for this load configuration (e.g., streaming load, or code resides in RAM). |
| `5` | `UDYNLINK_ERR_LOAD_INVALID_MODE` | An invalid load mode was specified. Also returned by streaming load if the work buffer is too small. |
| `6` | `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` | A relocation references an out-of-range symbol, or a relocation targets the module name entry. |
| `7` | `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` | An `extern` symbol could not be resolved by the host or any dependency module. |
| `8` | `UDYNLINK_ERR_LOAD_DUPLICATE_NAME` | A module with the same name is already loaded. (Currently unused; the eh2k fork allows duplicate instances.) |
| `10` | `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` | The module's `udynlink_version` is greater than the loader's `UDYNLINK_LOADER_ABI_VERSION`. |
| `11` | `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` | The module's `arch_tag` is incompatible with the host (different core family or stricter float ABI). |
| `12` | `UDYNLINK_ERR_LOAD_MISSING_DEP` | A declared dependency was not found, the dependency string table is missing, or `num_deps > UDYNLINK_MAX_DEPS`. |
| `13` | `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` | A circular dependency was detected: a module depends on itself, or the host reported a dependency is already being loaded via `udynlink_external_is_module_loading()`. |
| `14` | `UDYNLINK_ERR_LOAD_IO_ERROR` | A streaming read operation failed (returned `-1` or short count). |
| `15` | `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` | `udynlink_unload_module` was called on a module that other loaded modules still depend on. |
| `16` | `UDYNLINK_ERR_INVALID_MODULE` | A `NULL` module pointer was passed, or the module handle is uninitialized. |

---

### `udynlink_debug_level_t`

Controls verbosity of loader debug output.

| Value | Name | Description |
|-------|------|-------------|
| `0` | `UDYNLINK_DEBUG_NONE` | No debug output. |
| `1` | `UDYNLINK_DEBUG_ERROR` | Only errors are logged. |
| `2` | `UDYNLINK_DEBUG_WARNING` | Errors and warnings are logged. |
| `3` | `UDYNLINK_DEBUG_INFO` | All messages including informational traces are logged. |

---

## Symbol Constants

### Symbol Type Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `UDYNLINK_SYM_TYPE_INTERNAL` | `0` | Internal/static symbol. Not visible to the host or other modules. |
| `UDYNLINK_SYM_TYPE_EXPORTED` | `1` | Symbol explicitly exported by the module (e.g., `extern "C"` functions or public API). |
| `UDYNLINK_SYM_TYPE_EXTERN` | `2` | Symbol referenced by the module but defined elsewhere (host firmware or another module). |
| `UDYNLINK_SYM_TYPE_MODULE_NAME` | `3` | The module's own name entry. Always the first entry in the symbol table. |

### Symbol Location Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `UDYNLINK_SYM_LOCATION_CODE` | `0` | Symbol resides in the `.text` (code) section. |
| `UDYNLINK_SYM_LOCATION_DATA` | `1` | Symbol resides in the `.data` or `.bss` section. |

---

## Architecture Tag Constants and Macros

The `arch_tag` field is a `uint16_t` with the following bit layout:

```
Bits [3:0]  — Core family ID
Bit  4      — FPU present (1 = yes)
Bits [6:5]  — Float ABI: 00=soft, 01=softfp, 10=hard
Bits [15:7] — Reserved
```

### Architecture Tag Constants

| Constant | Value | Target |
|----------|-------|--------|
| `UDYNLINK_ARCH_TAG_CORTEX_M0` | `0x01` | Cortex-M0 |
| `UDYNLINK_ARCH_TAG_CORTEX_M0PLUS` | `0x02` | Cortex-M0+ |
| `UDYNLINK_ARCH_TAG_CORTEX_M3` | `0x03` | Cortex-M3 |
| `UDYNLINK_ARCH_TAG_CORTEX_M4` | `0x04` | Cortex-M4 (soft-float) |
| `UDYNLINK_ARCH_TAG_CORTEX_M4F` | `0x54` | Cortex-M4F (hard-float, FPU present) |
| `UDYNLINK_ARCH_TAG_CORTEX_M7` | `0x57` | Cortex-M7 (hard-float, FPU present) |
| `UDYNLINK_ARCH_TAG_CORTEX_M33` | `0x08` | Cortex-M33 |
| `UDYNLINK_ARCH_TAG_CORTEX_M55` | `0x59` | Cortex-M55 (hard-float, FPU present) |
| `UDYNLINK_ARCH_TAG_CORTEX_M85` | `0x5A` | Cortex-M85 (hard-float, FPU present) |

### Architecture Mask Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `UDYNLINK_ARCH_FAMILY_MASK` | `0x0F` | Mask to extract the core family ID. |
| `UDYNLINK_ARCH_FPU_MASK` | `0x10` | Mask to extract the FPU-present bit. |
| `UDYNLINK_ARCH_FLOAT_ABI_MASK` | `0x60` | Mask to extract the float ABI bits. |
| `UDYNLINK_ARCH_FLOAT_ABI_SHIFT` | `5` | Shift amount to right-align the float ABI field. |

### Float ABI Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `UDYNLINK_ARCH_FLOAT_ABI_SOFT` | `0` | Soft-float (all FP in software). |
| `UDYNLINK_ARCH_FLOAT_ABI_SOFTFP` | `1` | Softfp (FP arguments in core registers, software routines). |
| `UDYNLINK_ARCH_FLOAT_ABI_HARD` | `2` | Hard-float (FP arguments in VFP registers). |

### Version Macros

| Macro | Definition | Description |
|-------|------------|-------------|
| `UDYNLINK_MAKE_VERSION(major, minor)` | `(((major) << 8) \| (minor))` | Pack a major.minor version into a `uint16_t`. |
| `UDYNLINK_GET_MAJOR_VERSION(v)` | `(((v) >> 8) & 0xFF)` | Extract the major version. |
| `UDYNLINK_GET_MINOR_VERSION(v)` | `((v) & 0xFF)` | Extract the minor version. |

| Macro | Value | Description |
|-------|-------|-------------|
| `UDYNLINK_LOADER_ABI_VERSION` | `UDYNLINK_MAKE_VERSION(2, 0)` | The ABI version of the current loader. Modules with a higher `udynlink_version` are rejected. |

---

## Module Loading Functions

### `udynlink_load_module`

```c
udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod,
                                      const void *base_addr,
                                      void *load_addr,
                                      size_t load_size,
                                      udynlink_load_mode_t load_mode);
```

Loads a module from a memory-mapped image.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Pointer to a module handle structure allocated by the caller. Must not be `NULL`. **Must be zero-initialized before the first call** (e.g. `memset(p_mod, 0, sizeof(*p_mod))`), or the error-path cleanup may attempt to free garbage pointers. |
| `base_addr` | `const void *` | Start address of the module binary image in memory (flash or RAM). |
| `load_addr` | `void *` | RAM address where the module should be loaded, or `NULL` to request automatic allocation via `udynlink_external_malloc`. |
| `load_size` | `size_t` | If `load_addr` is not `NULL`, the size of the pre-allocated memory region. Ignored when `load_addr` is `NULL`. |
| `load_mode` | `udynlink_load_mode_t` | How much of the module to copy to RAM. See [`udynlink_load_mode_t`](#udynlink_load_mode_t). |

**Return value:**

- `UDYNLINK_OK` on success.
- One of the [`udynlink_error_t`](#udynlink_error_t) error codes on failure.

**Error conditions:**

- `UDYNLINK_ERR_INVALID_MODULE` — `p_mod` is `NULL`.
- `UDYNLINK_ERR_LOAD_INVALID_SIGN` — `base_addr` does not point to a valid "UDLM" header.
- `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` — Module toolchain version is newer than the loader.
- `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` — Core family or float ABI mismatch.
- `UDYNLINK_ERR_LOAD_MISSING_DEP` — A dependency is missing or the dependency table is malformed.
- `UDYNLINK_ERR_LOAD_RAM_LEN_LOW` — Caller-provided `load_size` is too small.
- `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY` — `udynlink_external_malloc` returned `NULL`.
- `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` — An `extern` symbol could not be resolved by any source.
- `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` — Corrupt relocation data.

**Thread safety:** The loader does **not** use any locking. Concurrent calls to `udynlink_load_module` from multiple interrupt levels will corrupt the internal module table. The host must disable interrupts (or use a mutex) around load/unload operations.

**Notes:**

- **Zero-initialization is required.** The caller must clear `*p_mod` (e.g. with `memset`) before the first call to `udynlink_load_module()`. If the structure contains uninitialized garbage, a mid-load error will read `p_mod->p_ram` and potentially call `udynlink_external_free()` on an invalid address.
- On error, all internally allocated memory is freed and `p_mod` is zeroed.
- The host must write `p_mod->ram_base` to `*(uint32_t *)UDYNLINK_LOT_BASE_ADDR` before calling any module function.

---

### `udynlink_load_module_from_stream`

```c
udynlink_error_t udynlink_load_module_from_stream(udynlink_module_t *p_mod,
    const udynlink_io_t *p_io, void *load_addr, size_t load_size,
    udynlink_load_mode_t load_mode, void *work_buf, size_t work_buf_size);
```

Loads a module from a stream (e.g., SD card, serial flash).

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Pointer to caller-allocated module handle. Must not be `NULL`. **Must be zero-initialized before the first call** (e.g. `memset(p_mod, 0, sizeof(*p_mod))`), or the error-path cleanup may attempt to free garbage pointers. |
| `p_io` | `const udynlink_io_t *` | Streaming callbacks and context. See [`udynlink_io_t`](#udynlink_io_t). |
| `load_addr` | `void *` | RAM address for loading, or `NULL` for automatic allocation. |
| `load_size` | `size_t` | Size of pre-allocated RAM if `load_addr` is not `NULL`. |
| `load_mode` | `udynlink_load_mode_t` | Must be `COPY_ALL` or `COPY_TEXT_DATA`. `XIP` is **not** supported and returns `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED`. |
| `work_buf` | `void *` | Caller-provided scratch buffer used for partial reads. |
| `work_buf_size` | `size_t` | Size of `work_buf`. Must be at least `64` bytes. |

**Return value:**

- `UDYNLINK_OK` on success.
- `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED` if `load_mode == UDYNLINK_LOAD_MODE_XIP`.
- `UDYNLINK_ERR_LOAD_INVALID_MODE` if `work_buf` is `NULL` or `work_buf_size < 64`.
- `UDYNLINK_ERR_LOAD_IO_ERROR` if any `read` or `get_size` callback fails.
- Other errors from [`udynlink_error_t`](#udynlink_error_t) apply as for `udynlink_load_module`.

**Notes:**

- The streaming loader reads the header first, validates it, then pulls the rest of the image through the `read` callback.
- Relocations and symbol names are fetched on demand from the stream; the work buffer is reused for each chunk.
- Internally, `COPY_TEXT_DATA` mode is converted to `COPY_ALL` because the header must reside in RAM for relocation processing.
- **Zero-initialization is required.** The caller must clear `*p_mod` (e.g. with `memset`) before the first call to `udynlink_load_module_from_stream()`, for the same reason as `udynlink_load_module()`.
- On error, allocated memory is freed and `p_mod` is zeroed.

**Thread safety:** The loader does **not** use any locking. Concurrent calls to `udynlink_load_module_from_stream` from multiple interrupt levels will corrupt the internal module table and `dep_refcount` fields. See [Thread Safety](integrating-as-host.md#thread-safety-and-concurrency) for full details.

---

### `udynlink_unload_module`

```c
udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod);
```

Unloads a module, freeing its RAM and clearing its handle.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Pointer to a loaded module handle. |

**Return value:**

- `UDYNLINK_OK` on success.
- `UDYNLINK_ERR_INVALID_MODULE` if `p_mod` is `NULL` or uninitialized.
- `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` if another loaded module still depends on this one (`dep_refcount > 0`).

**Thread safety:** The loader does **not** use any locking. Concurrent calls to `udynlink_unload_module` from multiple interrupt levels will corrupt `dep_refcount` fields on module handles. The host must disable interrupts (or use a mutex) around load/unload operations. See [Thread Safety](integrating-as-host.md#thread-safety-and-concurrency) for full details.

**Notes:**

- Dependency reference counts of modules this module depends on are decremented.
- RAM allocated by the loader (`!FOREIGN_RAM`) is freed via `udynlink_external_free`.
- The module structure is zeroed after unloading.
- If the module was loaded via streaming with the header copied to RAM, the header memory is also freed.

---

## Module Query Functions

### `udynlink_get_ram_size`

```c
size_t udynlink_get_ram_size(const udynlink_module_t *p_mod);
```

Returns the total RAM space currently used by the loaded module.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Loaded module handle. |

**Return value:** Number of bytes occupied in RAM, including LOT, `.data`, `.bss`, and optionally header+code depending on the load mode.

**Note:** This reflects the *actual* allocation size after loading, which is useful for diagnostics and memory pool accounting.

---

### `udynlink_get_module_name`

```c
const char *udynlink_get_module_name(const udynlink_module_t *p_mod);
```

Returns the name of a loaded module.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Loaded module handle. |

**Return value:** The module name string, or `NULL` if the module name entry is missing or corrupt.

---

### `udynlink_get_module_name_from_image`

```c
const char *udynlink_get_module_name_from_image(const void *base_addr);
```

Returns the name of a module given only its base address, **without** loading it.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `base_addr` | `const void *` | Pointer to the start of a module image in memory. |

**Return value:** The module name string, or `NULL` on error.

**Note:** This is useful for pre-load inspection, e.g., to show a list of available modules to the user.

---

### `udynlink_get_image_size`

```c
size_t udynlink_get_image_size(const void *base_addr);
```

Calculates the total module image size (header + metadata + code + data), not the RAM size.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `base_addr` | `const void *` | Pointer to the start of a module image. |

**Return value:** Total bytes from header through data section. Returns `0` if the signature is invalid.

---

### `udynlink_get_text_pointer`

```c
uint8_t *udynlink_get_text_pointer(const udynlink_module_t *p_mod);
```

Returns the absolute address of the module's `.text` section.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Loaded module handle. |

**Return value:** Pointer to the first byte of the text section.

**Note:** In `COPY_ALL` and `COPY_TEXT_DATA` modes this points to RAM. In `XIP` mode it points to the original flash address.

---

## Symbol Lookup Functions

### `udynlink_lookup_symbol`

```c
udynlink_sym_t *udynlink_lookup_symbol(const udynlink_module_t *p_mod,
                                       const char *name,
                                       udynlink_sym_t *p_sym);
```

Searches a module's symbol table for a symbol by name.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Module to search. If `NULL`, no search is performed (returns `NULL`). |
| `name` | `const char *` | Symbol name to look up. |
| `p_sym` | `udynlink_sym_t *` | Caller-provided structure to fill with symbol information. |

**Return value:**

- `p_sym` if the symbol is found.
- `NULL` if the symbol is not found or `p_mod` is `NULL`.

**Note:** For local and exported symbols, the `val` field is **relocated** to an absolute address before returning.

---

### `udynlink_get_symbol_value`

```c
uintptr_t udynlink_get_symbol_value(const udynlink_module_t *p_mod,
                                    const char *name);
```

Looks up a symbol and returns its absolute value.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Module to search. |
| `name` | `const char *` | Symbol name. |

**Return value:** The symbol's relocated absolute value, or `0` if not found.

**Note:** Because `0` is a valid address on Cortex-M, a return value of `0` is ambiguous. Use `udynlink_lookup_symbol` when you need to distinguish "not found" from "address 0".

---

## RAM Requirement Functions

### `udynlink_get_ram_requirements`

```c
size_t udynlink_get_ram_requirements(const void *base_addr,
                                     udynlink_load_mode_t mode);
```

Computes the RAM needed to load a module from a memory-mapped image, **before** actually loading it.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `base_addr` | `const void *` | Pointer to the module image header. |
| `mode` | `udynlink_load_mode_t` | Intended load mode. |

**Return value:** Required RAM size in bytes.

---

### `udynlink_get_ram_requirements_stream`

```c
size_t udynlink_get_ram_requirements_stream(const udynlink_io_t *p_io,
                                            udynlink_load_mode_t mode);
```

Computes the RAM needed to load a module from a streaming source.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_io` | `const udynlink_io_t *` | Streaming I/O callbacks. |
| `mode` | `udynlink_load_mode_t` | Intended load mode. |

**Return value:** Required RAM size in bytes, or `0` on I/O error or invalid signature.

**Note:** For `COPY_TEXT_DATA` mode, the streaming implementation internally copies the header to RAM, so the returned size includes the header offset.

---

### `udynlink_get_stream_metadata_size`

```c
size_t udynlink_get_stream_metadata_size(const udynlink_io_t *p_io);
```

Returns the size of module metadata (everything before the code section) for a streaming load.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_io` | `const udynlink_io_t *` | Streaming I/O callbacks. |

**Return value:** Size in bytes equal to the byte offset from the start of the image to the code section (header + relocation table + symbol table + dependency string table + padding to 4-byte alignment). This allows the loader to read all metadata in a single `read()` callback. Returns `0` on I/O error or invalid signature.

---

## C++ Support

### `udynlink_cpp_init`

```c
void udynlink_cpp_init(udynlink_module_t *p_mod);
```

Runs global C++ constructors for a loaded C++ module by invoking `__init_array`.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Loaded C++ module handle. |

**Note:** The host **must** set `*(uint32_t *)UDYNLINK_LOT_BASE_ADDR = p_mod->ram_base` before calling this function, because constructor code may access module data through the LOT.

See [C++ Module with Constructors](examples.md#c-module-with-constructors) for a complete example.

---

## Hash-Based Symbol Resolution

### `udynlink_resolve_hashed_symbol`

```c
void *udynlink_resolve_hashed_symbol(const udynlink_hash_table_t *table,
                                     const char *name);
```

Performs O(1) symbol lookup in a pre-built GNU hash table.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `table` | `const udynlink_hash_table_t *` | Pointer to an initialized hash table structure. |
| `name` | `const char *` | Symbol name to resolve. |

**Return value:**

- Pointer to the symbol's address if found.
- `NULL` if the symbol is not in the table.

**Note:** The hash table is typically generated offline by the `mkhostsyms` script from the host firmware ELF. See [Host with Hash-Based Resolution](examples.md#host-with-hash-based-resolution) for usage.

---

## Utility Functions

### `udynlink_error_msg`

```c
const char *udynlink_error_msg(udynlink_error_t *err);
```

Returns a human-readable string for an error code.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `err` | `udynlink_error_t *` | Pointer to the error code. |

**Return value:** Static string describing the error. Do not free.

**Note:** This function takes a **pointer** to the error code, not the value itself.

---

### `udynlink_set_debug_level`

```c
void udynlink_set_debug_level(udynlink_debug_level_t level);
```

Sets the global debug verbosity level.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `level` | `udynlink_debug_level_t` | Desired level. See [`udynlink_debug_level_t`](#udynlink_debug_level_t). |

---

## Convenience Macros

### `UDYNLINK_SYMBOL`

```c
#define UDYNLINK_SYMBOL(sym) { #sym, (void *)(uintptr_t)(sym) }
```

Expands to a `{name, address}` initializer pair for building static host symbol tables. See [Host with Symbol Table](examples.md#host-with-symbol-table).

### `UDYNLINK_DEBUG`

```c
#define UDYNLINK_DEBUG(...) udynlink_debug(__func__, __LINE__, __VA_ARGS__)
```

Internal macro used by the loader to emit debug messages. Not intended for host use, but documented for completeness.

---

## External Callbacks

The host firmware **must** implement every function declared in `udynlink/udynlink_externals.h`. Without these, the loader will not link.

### `udynlink_external_is_pointer_in_ram`

```c
int udynlink_external_is_pointer_in_ram(const void *p);
```

Returns non-zero if the given pointer lies in the MCU's SRAM region.

**Called by:** The loader during XIP validation (ensuring code does not reside in RAM).

**Note:** This callback is declared in the header but the current loader implementation does not actively call it during XIP loads. Hosts should still provide a correct implementation for future compatibility.

---

### `udynlink_external_malloc`

```c
void *udynlink_external_malloc(size_t size);
```

Allocates `size` bytes of RAM for a module.

**Called by:** `udynlink_load_module` and `udynlink_load_module_from_stream` when `load_addr == NULL` (auto-allocation mode).

**Semantics:** Must return a pointer to at least `size` bytes of writable RAM, aligned suitably for 32-bit access. May return `NULL` on failure.

---

### `udynlink_external_free`

```c
void udynlink_external_free(void *p);
```

Frees memory previously allocated by `udynlink_external_malloc`.

**Called by:** `udynlink_unload_module` and error cleanup paths in the loaders.

**Semantics:** Must safely handle `NULL`.

---

### `udynlink_external_vprintf`

```c
void udynlink_external_vprintf(const char *s, va_list va);
```

Debug/logging output function.

**Called by:** The loader's internal debug macro whenever `debug_level` permits.

**Semantics:** Must format and emit the message. On bare-metal systems this typically forwards to an ITM/SWO channel, UART, or semi-hosting `printf`. If debug output is not needed, the host may provide an empty stub.

---

### `udynlink_external_resolve_symbol`

```c
uintptr_t udynlink_external_resolve_symbol(const char *name);
```

Resolves an external symbol name to an address.

**Called by:** The relocation engine for every `UDYNLINK_SYM_TYPE_EXTERN` symbol that was **not** resolved by `udynlink_external_resolve_critical_symbol` and was **not** found in any dependency module.

**Semantics:** Must return the 32-bit address of the named symbol, or `0` if the symbol is unknown. This is the *fallback* host symbol resolution path in the three-tier search order.

**Three-tier resolution order:**

1. `udynlink_external_resolve_critical_symbol(name)`
2. Search dependency modules (loaded modules declared via `--depends`)
3. `udynlink_external_resolve_symbol(name)` (fallback)

---

### `udynlink_external_resolve_critical_symbol`

```c
uintptr_t udynlink_external_resolve_critical_symbol(const char *name);
```

Resolves critical host symbols before falling back to dependency modules.

**Called by:** The relocation engine as the **first** tier of symbol resolution.

**Semantics:** Must return the symbol address, or `0` to defer to the next tier (dependency modules, then `udynlink_external_resolve_symbol`). Use this for core firmware services that should never be shadowed by module exports.

---

### `udynlink_external_get_module_handle`

```c
struct _udynlink_module_t *udynlink_external_get_module_handle(const char *module_name);
```

Looks up a loaded module by its name string.

**Called by:** `udynlink_load_module` and `udynlink_load_module_from_stream` during dependency validation, to verify that modules declared via `--depends` are already loaded.

**Semantics:** Must search the host's module registry and return the matching `udynlink_module_t *`, or `NULL` if not found. The host is responsible for tracking loaded modules (e.g., in a static array or linked list).

**Note:** The eh2k fork allows multiple instances of the same module name; this callback should return **any** matching instance for dependency resolution.

---

### `udynlink_external_is_module_loading`

```c
int udynlink_external_is_module_loading(const char *module_name);
```

Checks whether a module is currently in the middle of being loaded.

**Called by:** `udynlink_load_module` and `udynlink_load_module_from_stream` during dependency validation, immediately after `udynlink_external_get_module_handle` returns `NULL` for a missing dependency.

**Semantics:** Must return a non-zero value if the host has an in-progress load for the named module, and `0` otherwise. A weak default returning `0` is provided. Hosts that track load state can override this to enable cross-module circular dependency detection.

**Note:** Self-dependencies (a module depending on itself) are always detected and rejected with `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` regardless of this callback.
