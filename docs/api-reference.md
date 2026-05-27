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
### Sentinel Macros

| Macro | Value | Description |
|-------|-------|-------------|
| `UDYNLINK_DEP_DEFERRED` | `(udynlink_module_t*)1` | Returned by `udynlink_external_get_module_handle()` to defer a dependency. See [Deferred Dependencies](integrating-as-host.md#deferred-dependencies-and-symbols). |
| `UDYNLINK_SYM_DEFERRED` | `(uint32_t)1` | Returned by symbol-resolution callbacks to defer an individual extern symbol. See [Deferred Symbols](integrating-as-host.md#deferred-dependencies-and-symbols). |

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

A runtime handle representing a loaded module instance. The host allocates this structure and passes it to `udynlink_load_module` or `udynlink_load_module_image`.

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
    uint8_t  max_deps;       // Capacity of the deps array
    const struct _udynlink_module_t **deps;    // Host-allocated dependency handle array
    void    *user_ctx;       // Opaque user context pointer (never touched by the loader)
} udynlink_module_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `p_header` | Points to the module header. In `COPY_ALL` mode this lives in RAM; in `COPY_TEXT_DATA` and `XIP` modes it lives at the original `base_addr`. |
| `p_ram` / `ram_base` | Base address of the RAM region allocated for this module. The LOT starts here. Before calling any module function, the host **must** write `ram_base` to `*(uint32_t *)UDYNLINK_LOT_BASE_ADDR`. |
| `info` | Bitfield storing the [load mode](#udynlink_load_mode_t) and whether the RAM was provided by the host (`FOREIGN_RAM`) or allocated by the loader. |
| `num_deps` | Count of dependencies actually linked at load time. May be less than `p_header->num_deps` if some dependencies were deferred. |
| `dep_refcount` | Reference count of modules that list this module as a dependency. `udynlink_unload_module` fails with `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` if this is non-zero. |
| `max_deps` | Number of slots allocated in the `deps` array by the host. The loader refuses to load a module whose `num_deps` exceeds this value. |
| `deps` | Pointer to a host-allocated array of dependency module handles. Used for inter-module symbol resolution. Only entries `0` through `num_deps - 1` are valid. May be `NULL` if the module has no dependencies. |
| `user_ctx` | Opaque pointer for host use. The loader **never** reads or writes this field; it is purely a convenience slot for associating arbitrary state (e.g., a filesystem path, a language runtime handle, or a reference-counting wrapper) with a module handle. |

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

### `udynlink_module_image_t`

Non-contiguous module image descriptor. Points to each section of a module image independently. The loader reads relocation and symbol information through these pointers; it never assumes the image is contiguous.

```c
typedef struct {
    const udynlink_module_header_t *p_header;       // module header
    const uint32_t                *p_relocations; // relocation table
    const uint32_t                *p_symtab;        // symbol table base
    const char                    *p_deps_strtab;   // dependency string table
    const uint8_t                 *p_code;          // .text section
    const uint8_t                 *p_data;          // .data section
} udynlink_module_image_t;
```

**Field descriptions:**

| Field | Description |
|-------|-------------|
| `p_header` | Pointer to the module header (36 bytes). |
| `p_relocations` | Pointer to the relocation table (`num_rels * 2 * uint32_t`). |
| `p_symtab` | Pointer to the symbol table base (first word = entry count). The string pool is assumed contiguous with entries. |
| `p_deps_strtab` | Pointer to the dependency string table (NULL for v1.0 or no deps). |
| `p_code` | Pointer to the code section in the source. |
| `p_data` | Pointer to the data section in the source. |

For memory-mapped images that follow the standard UDLM layout, use `udynlink_image_from_memory()` to populate this structure.

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
| `1` | `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA` | Copy text and data sections to RAM. The header stays at `base_addr` (or at `image->p_header` for non-contiguous loads). |
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
| `4` | `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED` | XIP is not supported because the code pointer is not in an executable region. |
| `5` | `UDYNLINK_ERR_LOAD_INVALID_MODE` | An invalid load mode was specified. |
| `6` | `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` | A relocation references an out-of-range symbol, or a relocation targets the module name entry. |
| `7` | `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` | An `extern` symbol could not be resolved by the host or any dependency module. |
| `8` | `UDYNLINK_ERR_LOAD_DUPLICATE_NAME` | A module with the same name is already loaded. (Currently unused; the eh2k fork allows duplicate instances.) |
| `10` | `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` | The module's `udynlink_version` is greater than the loader's `UDYNLINK_LOADER_ABI_VERSION`. |
| `11` | `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` | The module's `arch_tag` is incompatible with the host (different core family or stricter float ABI). |
| `12` | `UDYNLINK_ERR_LOAD_MISSING_DEP` | A declared dependency was not found, the dependency string table is missing, or `num_deps` exceeds the host-provided `max_deps` capacity. |
| `13` | `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` | A circular dependency was detected: a module depends on itself, or the host reported a dependency is already being loaded via `udynlink_external_is_module_loading()`. |
| `14` | `UDYNLINK_ERR_LOAD_IO_ERROR` | Reserved. Previously used for streaming I/O read failures. |
| `15` | `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` | `udynlink_unload_module` was called on a module that other loaded modules still depend on. |
| `16` | `UDYNLINK_ERR_INVALID_MODULE` | A `NULL` module pointer was passed, or the module handle is uninitialized. |
| `17` | `UDYNLINK_ERR_LOAD_HOOK_ABORTED` | Reserved. Previously used when a streaming load lifecycle hook returned non-OK. |

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
| `UDYNLINK_SYM_TYPE_WEAK` | `4` | Weak symbol defined in the module. The loader first applies the module's own address, then attempts host/dependency override via the same three-tier resolution used for `EXTERN`. If no override is found, the module's definition remains. |

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

### `udynlink_load_module_image`

```c
udynlink_error_t udynlink_load_module_image(udynlink_module_t *p_mod,
    const udynlink_module_image_t *image,
    void *load_addr, size_t load_size,
    udynlink_load_mode_t load_mode);
```

Loads a module from a non-contiguous image descriptor.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Pointer to caller-allocated module handle. Must not be `NULL`. **Must be zero-initialized before the first call** (e.g. `memset(p_mod, 0, sizeof(*p_mod))`), or the error-path cleanup may attempt to free garbage pointers. |
| `image` | `const udynlink_module_image_t *` | Module image descriptor with all section pointers valid for the duration of the load. |
| `load_addr` | `void *` | RAM address for loading, or `NULL` for automatic allocation. |
| `load_size` | `size_t` | Size of pre-allocated RAM if `load_addr` is not `NULL`. |
| `load_mode` | `udynlink_load_mode_t` | `COPY_ALL`, `COPY_TEXT_DATA`, or `XIP`. |

**Return value:**

- `UDYNLINK_OK` on success.
- `UDYNLINK_ERR_LOAD_INVALID_SIGN` — `image->p_header` does not point to a valid "UDLM" header.
- `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` — module toolchain version is newer than the loader.
- `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` — core family or float ABI mismatch.
- `UDYNLINK_ERR_LOAD_MISSING_DEP` — a dependency is missing or the dependency table is malformed.
- `UDYNLINK_ERR_LOAD_RAM_LEN_LOW` — caller-provided `load_size` is too small.
- `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY` — `udynlink_external_malloc` returned `NULL`.
- `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` — an `extern` symbol could not be resolved by any source.
- `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` — corrupt relocation data.

**Notes:**

- For `COPY_ALL` mode, this function copies each section from the image descriptor into a single contiguous RAM buffer.
- For `COPY_TEXT_DATA` and `XIP` modes, the metadata (header, relocations, symbol table) is **not** copied; it must remain accessible via `image->p_header` for post-load symbol lookups. If your source metadata is not contiguous with the header, use `COPY_ALL`.
- **Zero-initialization is required.** The caller must clear `*p_mod` before the first call.
- On error, all internally allocated memory is freed and `p_mod` is zeroed.
- The host must write `p_mod->ram_base` to `*(uint32_t *)UDYNLINK_LOT_BASE_ADDR` before calling any module function.

**Thread safety:** The loader does **not** use any locking. Concurrent calls to `udynlink_load_module_image` from multiple interrupt levels will corrupt the internal module table and `dep_refcount` fields. See [Thread Safety](integrating-as-host.md#thread-safety-and-concurrency) for full details.

---

### `udynlink_load_apply_relocations`

```c
udynlink_error_t udynlink_load_apply_relocations(udynlink_module_t *p_mod,
    const udynlink_module_header_t *p_header,
    const uint32_t *p_relocations,
    const uint32_t *p_symtab);
```

Applies all relocations to a module whose sections are already in RAM.

This is a low-level primitive for custom loading pipelines. The caller must have already allocated RAM, copied code/data, zeroed BSS, and populated `p_mod->p_ram` and `p_mod->p_header`. This function reads relocation and symbol data from the provided pointers and patches the module's LOT and `.data` slots.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Module handle with RAM and header already set. |
| `p_header` | `const udynlink_module_header_t *` | Module header (for `num_rels`, `num_lot`). |
| `p_relocations` | `const uint32_t *` | Pointer to the relocation table. |
| `p_symtab` | `const uint32_t *` | Pointer to the symbol table base. |

**Return value:**

- `UDYNLINK_OK` on success.
- `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` if a relocation references an out-of-range symbol.
- `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` if an `EXTERN` symbol could not be resolved.

---

### `udynlink_validate_header`

```c
udynlink_error_t udynlink_validate_header(const udynlink_module_header_t *header);
```

Validates a module header.

Checks the signature and ABI version. Architecture tag compatibility is **not** checked here; call `udynlink_check_arch_tag()` separately.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `header` | `const udynlink_module_header_t *` | Pointer to the module header. |

**Return value:** `UDYNLINK_OK` if valid, or an error code (`UDYNLINK_ERR_LOAD_INVALID_SIGN` or `UDYNLINK_ERR_LOAD_VERSION_MISMATCH`).

---

### `udynlink_compute_ram_size`

```c
size_t udynlink_compute_ram_size(const udynlink_module_header_t *header,
                                 udynlink_load_mode_t mode);
```

Computes the RAM size required to load a module.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `header` | `const udynlink_module_header_t *` | Pointer to the module header. |
| `mode` | `udynlink_load_mode_t` | Intended load mode. |

**Return value:** Required RAM size in bytes.

---

### `udynlink_get_image_metadata_size`

```c
size_t udynlink_get_image_metadata_size(const udynlink_module_header_t *header);
```

Returns the size of module metadata (everything before the code section).

This is the byte offset from the start of the module image to the beginning of the code section. For non-contiguous images, this is the total size of the metadata buffers that must be provided.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `header` | `const udynlink_module_header_t *` | Pointer to the module header. |

**Return value:** Metadata size in bytes.

---

### `udynlink_image_get_module_name`

```c
const char *udynlink_image_get_module_name(const uint32_t *p_symtab);
```

Gets the module name from a symbol table.

Only reads `p_symtab`; the caller does not need to provide a full `udynlink_module_image_t`. The string pool is assumed to be contiguous with the symbol table entries.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_symtab` | `const uint32_t *` | Pointer to the symbol table base. |

**Return value:** Pointer to the null-terminated module name, or `NULL` on error.

---

### `udynlink_image_get_deps`

```c
size_t udynlink_image_get_deps(const udynlink_module_header_t *p_header,
                               const char *deps_strtab,
                               const char **deps, size_t max_deps);
```

Reads dependency names from a module image without loading it.

Only reads `p_header` (for `num_deps`) and `deps_strtab`. The caller does not need to provide a full `udynlink_module_image_t`.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_header` | `const udynlink_module_header_t *` | Pointer to the module header. |
| `deps_strtab` | `const char *` | Pointer to the dependency string table. |
| `deps` | `const char **` | Array to receive dependency name pointers. May be `NULL` if `max_deps` is 0. |
| `max_deps` | `size_t` | Maximum number of entries to write into `deps`. |

**Return value:** Total number of dependencies declared in the module image. This may be larger than `max_deps` if the array was too small. Returns 0 if the image is invalid or has no dependencies.

---

### `udynlink_image_from_memory`

```c
void udynlink_image_from_memory(const void *base_addr,
                                udynlink_module_image_t *out_image);
```

Populates an image descriptor from a contiguous memory buffer.

Derives each section pointer from `base_addr` using the standard UDLM layout. The resulting descriptor is suitable for `udynlink_load_module_image()` or for direct use with the low-level relocation API.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `base_addr` | `const void *` | Address of the module image in memory. |
| `out_image` | `udynlink_module_image_t *` | Image descriptor to populate. |

---

### `udynlink_image_from_module`

```c
void udynlink_image_from_module(const udynlink_module_t *p_mod,
                                udynlink_module_image_t *out_image);
```

Populates an image descriptor from an already-loaded module.

Convenience wrapper around `udynlink_image_from_memory()` using the module's current header pointer. Assumes the loaded module image remains contiguous (true for all load modes).

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Pointer to the loaded module handle. |
| `out_image` | `udynlink_module_image_t *` | Image descriptor to populate. |

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

## Linking Functions

### `udynlink_link_incremental`

```c
udynlink_error_t udynlink_link_incremental(udynlink_module_t *p_mod);
```

Incrementally resolves unresolved `EXTERN` relocations in a loaded module.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Loaded module. |

**Return value:** `UDYNLINK_OK` on success.

**Behavior:**

- Scans the module's relocation table for `EXTERN` entries.
- Only slots that are currently `0` are resolved; already-resolved slots are left untouched.
- Resolution uses the current `deps[]` array and the three-tier chain (critical host → dependencies → fallback host).

**Precondition:** The caller must populate `p_mod->deps[]` with all desired dependency modules before calling this function. To append a deferred dependency:
1. Set `p_mod->deps[p_mod->num_deps++] = dep_mod`.
2. Increment `dep_mod->dep_refcount`.
3. Call `udynlink_link_incremental(p_mod)`.

**Use cases:**
- Loading circular dependency graphs (e.g., `mod_a` ↔ `mod_b`).
- Linking optional dependencies after they are loaded later.
- Bulk-linking multiple dependencies efficiently (only zero slots are touched).

> ⚠️ **Warning:** Resolving a dependency symbol so that module A can call module B does **not** automatically fix the LOT base register. When A calls B's resolved function, B's wrapper prologue still loads `r9` from the global `UDYNLINK_LOT_BASE_ADDR` word, which at that moment likely contains A's base. B will execute with the wrong `r9` unless the host manually rewrote the LOT base before the call. This is safe only for pure leaf functions that never access global data. See [Cross-Module Calls and the LOT Base](integrating-as-host.md#cross-module-calls-and-the-lot-base).

**Thread safety:** This function reads `deps` and writes relocation slots. The host must ensure no concurrent load/unload operations are in progress. See [Thread Safety](integrating-as-host.md#thread-safety-and-concurrency).

---

### `udynlink_relink_all`

```c
udynlink_error_t udynlink_relink_all(udynlink_module_t *p_mod);
```

Re-resolves **all** `EXTERN` relocations in a loaded module from scratch.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Loaded module. |

**Return value:** `UDYNLINK_OK` on success.

**Behavior:**

- Scans every `EXTERN` relocation slot and re-resolves it using the current `deps[]` and the three-tier chain.
- Dependency module symbols (tier 2) take precedence over host fallback symbols (tier 3), even if the slot was already non-zero.
- Slower than `udynlink_link_incremental()` but correct when host fallbacks must be overridden.

**Precondition:** Same as `udynlink_link_incremental()` — caller must populate `deps[]` first.

**Use cases:**
- Re-linking after adding a dependency that should override an existing host fallback symbol.
- Explicit full re-resolution when the incremental behavior is insufficient.

> ⚠️ **Warning:** Same LOT base limitation as `udynlink_link_incremental()`. Re-linking a dependency symbol does not fix the `r9` register at call time. See [Cross-Module Calls and the LOT Base](integrating-as-host.md#cross-module-calls-and-the-lot-base).

**Thread safety:** Same as `udynlink_link_incremental`.

---

### `udynlink_link_symbol`

```c
udynlink_error_t udynlink_link_symbol(udynlink_module_t *p_mod,
                                      const char *sym_name,
                                      uintptr_t sym_addr);
```

Directly patches a symbol's relocation slot in a loaded module.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `udynlink_module_t *` | Loaded module. |
| `sym_name` | `const char *` | Null-terminated symbol name. |
| `sym_addr` | `uint32_t` | Address to write into matching relocation slots. |

**Return value:**

- `UDYNLINK_OK` if at least one relocation slot was patched.
- `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` if the symbol is not found in the relocation table.

**Behavior:**
- Scans the module's relocation table for entries referencing `sym_name`.
- For each matching relocation (any type), overwrites the slot with `sym_addr`.
- Does **not** update `deps`, `dep_refcount`, or the symbol table.

**Use cases:**
1. Deferred host symbols — host knows the address now and patches directly.
2. Hot-patching — replace a module's extern reference at runtime (e.g., a mock for testing).
3. Dynamic symbol tables — host maintains its own table and pushes updates into loaded modules.

**Thread safety:** Same as `udynlink_link_dependency` — host must serialize with load/unload.

---

## Module Query Functions

### `udynlink_is_module_fully_linked`

```c
int udynlink_is_module_fully_linked(const udynlink_module_t *p_mod);
```

Checks whether all declared dependencies of a module are linked.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Loaded module. |

**Return value:** `1` if every dependency declared in the module header is present in `p_mod->deps`, `0` otherwise.

---

### `udynlink_get_linked_dependency`

```c
udynlink_module_t *udynlink_get_linked_dependency(const udynlink_module_t *p_mod,
                                                  const char *dep_name);
```

Returns the handle of a linked dependency by name.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Loaded module. |
| `dep_name` | `const char *` | Null-terminated dependency name. |

**Return value:** Pointer to the dependency module if it is linked into `p_mod`, or `NULL` if the dependency is not linked (either deferred or not declared).

---

### `udynlink_is_symbol_resolved`

```c
int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod,
                                const char *sym_name);
```

Checks whether an extern symbol has a non-zero resolved value.

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `p_mod` | `const udynlink_module_t *` | Loaded module. |
| `sym_name` | `const char *` | Null-terminated symbol name. |

**Return value:** `1` if the symbol exists and its value is non-zero, `0` otherwise.

**Note:** A symbol that was deferred during load and has not yet been linked resolves to `0`. After `udynlink_link_incremental()` or `udynlink_relink_all()` resolves it from a dependency module, this function returns `1`.

---

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

**Called by:** `udynlink_load_module` and `udynlink_load_module_image` when `load_addr == NULL` (auto-allocation mode).

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

**Called by:** The relocation engine (both load-time and post-link re-resolution) for every `UDYNLINK_SYM_TYPE_EXTERN` symbol that was **not** resolved by `udynlink_external_resolve_critical_symbol` and was **not** found in any dependency module.

**Semantics:** Must return the 32-bit address of the named symbol, `0` if the symbol is unknown, or `UDYNLINK_SYM_DEFERRED` if the symbol is known but should not be resolved yet. This is the *fallback* host symbol resolution path in the three-tier search order.

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

**Semantics:** Must return the symbol address, `0` to defer to the next tier (dependency modules, then `udynlink_external_resolve_symbol`), or `UDYNLINK_SYM_DEFERRED` to defer the symbol without failing the load. Use this for core firmware services that should never be shadowed by module exports.

---

### `udynlink_external_get_module_handle`

```c
struct _udynlink_module_t *udynlink_external_get_module_handle(const char *module_name);
```

Looks up a loaded module by its name string.

**Called by:** `udynlink_load_module` and `udynlink_load_module_image` during dependency validation, to verify that modules declared via `--depends` are already loaded.

**Semantics:** Must search the host's module registry and return one of three values:
- A valid `udynlink_module_t *` — dependency is loaded and will be linked immediately.
- `NULL` — dependency not found; load fails with `UDYNLINK_ERR_LOAD_MISSING_DEP`.
- `UDYNLINK_DEP_DEFERRED` — dependency exists but should not be linked yet; loader skips it.

**Note:** The eh2k fork allows multiple instances of the same module name; this callback should return **any** matching instance for dependency resolution.

> ⚠️ **Warning:** Returning a valid module handle here enables the loader to resolve the dependency's exported symbols into the consuming module's LOT. However, this does **not** make cross-module function calls safe at runtime. When module A calls a function resolved from dependency B, B's wrapper prologue loads `r9` from the global `UDYNLINK_LOT_BASE_ADDR` word, which may still contain A's base. See [Cross-Module Calls and the LOT Base](integrating-as-host.md#cross-module-calls-and-the-lot-base).

---

### `udynlink_external_is_module_loading`

```c
int udynlink_external_is_module_loading(const char *module_name);
```

Checks whether a module is currently in the middle of being loaded.

**Called by:** `udynlink_load_module` and `udynlink_load_module_image` during dependency validation, immediately after `udynlink_external_get_module_handle` returns `NULL` for a missing dependency.

**Semantics:** Must return a non-zero value if the host has an in-progress load for the named module, and `0` otherwise. A weak default returning `0` is provided. Hosts that track load state can override this to enable cross-module circular dependency detection.

**Note:** Self-dependencies (a module depending on itself) are always detected and rejected with `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` regardless of this callback.
