# Plan: Hash-Based Symbol Resolution & Dependency Tracking

**Goal**: Port Mk RTOS's O(1) GNU hash + bloom filter symbol resolution and DT_NEEDED-style module dependency tracking to udynlink.

**Source Reference**: `alternatives/mk-rtos-portable-features.md` (Features 1 & 3)

---

## Decisions Log

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Header size change | Accept 36-byte header (32→36) | Clean extension. Bumps ABI to 2.0. Old 1.0 modules load on new loader. |
| D2 | Symbol resolution order | Split: **critical host → dependencies → fallback host** | Critical symbols (printf, malloc) always win. Dependencies can shadow fallback host symbols. |
| D3 | Symbol filtering in mkhostsyms | All globals by default, optional `--filter` regex | Full symbol table by default. Regex filter for selective export in large firmware. |
| D4 | Dep tracking mechanism | Host callback `udynlink_external_get_module_handle` | Matches existing callback pattern. No global state in loader. |
| D5 | Hash table generation | Offline at build time (C header) | Zero RAM overhead. `const` in host flash. Matches Mk approach. |
| D6 | mkhostsyms as separate tool | Yes, separate from mkmodule | Different inputs (host ELF vs. module sources), different outputs, different lifecycle. |

---

## Architecture

### Feature 1: Hash-Based Symbol Resolution

#### Overview

Replace the O(N) `strcmp` chain in the host's `udynlink_external_resolve_symbol` with O(1) GNU hash table + bloom filter lookup.

- **Offline**: `scripts/mkhostsyms` reads the host firmware ELF, builds a GNU hash table, emits a C header
- **Runtime**: ~50-line C lookup function, no heap allocation, all data `const` in host flash

#### Hash Table Struct (new file: `udynlink/udynlink_hash.h`)

```c
typedef struct {
    uint32_t nbuckets;
    uint32_t symoffset;       // first global symbol index (locals come before)
    uint32_t bloom_size;      // number of 32-bit bloom words
    uint32_t bloom_shift;     // shift for second hash
    const uint32_t *bloom;       // bloom_size words
    const uint32_t *buckets;     // nbuckets words
    const uint32_t *hash_values; // (nsyms - symoffset) words, bit 0 = chain terminator
    const uint32_t *sym_addrs;   // nsyms words of absolute addresses
    const char *strtab;          // packed null-terminated name strings
    const uint32_t *strtab_offsets; // nsyms offsets into strtab
} udynlink_hash_table_t;

// Lookup: O(1) amortized. Returns symbol address or 0 if not found.
void *udynlink_resolve_hashed_symbol(const udynlink_hash_table_t *table, const char *name);
```

#### Lookup Algorithm

Port from Mk's `mk_loader_elf_resolveSymbolInternal` (resolveExternalSymbols.c:420-530):

1. Compute GNU hash: `h = 5381; for each char c: h = h*33 + c`
2. Bloom filter: `mask = (1 << h%32) | (1 << (h>>bloom_shift)%32)`; test `bloom[(h/32) & (bloom_size-1)] & mask`. If zero → symbol definitely not present.
3. Bucket: `idx = buckets[h % nbuckets]`. If `idx < symoffset` → empty bucket, not found.
4. Chain walk: `hash_value = hash_values[idx - symoffset]`. If `(hash_value | 1) == (h | 1)` → potential match, do `strcmp`. If not, increment `idx` and continue. Chain ends when `hash_value & 1` (bit 0 set = last in chain).
5. On match: return `sym_addrs[idx]`.

#### Data Flow

```
Host firmware ELF (.symtab + .strtab)
    │
    ▼  scripts/mkhostsyms
    ├── C header: host_syms.h
    │   // All tables are const (host flash)
    │   const uint32_t g_host_hash_bloom[] = { ... };
    │   const uint32_t g_host_hash_buckets[] = { ... };
    │   ...
    │   const udynlink_hash_table_t g_host_sym_table = { ... };
    │
    ▼  Host firmware #include "host_syms.h"
    └── In udynlink_external_resolve_symbol:
        return (uint32_t)udynlink_resolve_hashed_symbol(&g_host_sym_table, name);
```

#### Offline Tool: `scripts/mkhostsyms`

```
Usage: python3 scripts/mkhostsyms [options]
  --elf PATH         Host firmware ELF file (required)
  --output PATH      Output C header path (required)
  --nbuckets N       Number of hash buckets (default: auto-sized, next power of 2 >= nsyms/4)
  --bloom-size N     Bloom filter size in 32-bit words (default: auto-sized)
  --filter REGEX     Only include symbols matching regex (default: all STB_GLOBAL + STV_DEFAULT)
```

Algorithm (port from Sym2srec's `sym2srec_hash.c`):
1. Parse ELF, extract `.symtab` + `.strtab` using `udynlink_utils.py`
2. Filter: keep only `STB_GLOBAL` symbols that are defined (`stShndx != SHN_UNDEF`) and not `STT_FILE`/`STT_SECTION`
3. Apply `--filter` regex if given
4. Sort: locals first (by original index), then globals sorted by `gnu_hash(name) % nbuckets`
5. Build bloom filter, bucket array, hash value chain
6. Generate C header

#### Files Changed / Added

| File | Change | Complexity |
|------|--------|------------|
| `udynlink/udynlink_hash.h` | **New** — hash table struct + lookup declaration | Low |
| `udynlink/udynlink_hash.c` | **New** — ~60-line lookup implementation | Low |
| `scripts/mkhostsyms` | **New** — offline Python tool (~250 lines) | Medium |
| `tests/qemu_host/src/main.c` | Replace `strcmp` chain with hash lookup | Low |
| `CMakeLists.txt` (root) | Add `udynlink_hash.c` to library sources | Low |
| `CMakeLists.txt` (test) | Add mkhostsyms build step | Low |

No changes to `udynlink.h`, `udynlink.c`, or `udynlink_externals.h` for Feature 1.

---

### Feature 2: Module Dependency Tracking

#### Overview

Allow modules to declare dependencies on other modules. The loader validates dependencies at load time, resolves extern symbols through dependency export tables, and prevents unsafe unloading.

#### Split Symbol Resolution (Key Design)

Three-tier resolution for extern symbols:

```
EXTERN symbol resolution order:
  1. udynlink_external_resolve_critical_symbol(name)  → always wins (host critical API)
  2. Search dependency modules' export tables            → dependencies can shadow host fallback
  3. udynlink_external_resolve_symbol(name)             → fallback host resolver
```

New callback added to `udynlink_externals.h`:

```c
// Critical symbol resolver — checked first, always wins
// Used for symbols the host must never allow modules to shadow (printf, malloc, etc.)
// Returns 0 if symbol is not a critical symbol.
uint32_t udynlink_external_resolve_critical_symbol(const char *name);
```

The existing `udynlink_external_resolve_symbol` becomes the **fallback**, checked last.

This gives the host fine-grained control:
- Critical symbols (printf, malloc, _write): always resolved by host, cannot be shadowed
- Fallback symbols: can be provided by a dependency module or the host

#### UDLM Header Extension

Header grows from 32 to 36 bytes:

```c
typedef struct {
    uint32_t sign;               // 0:  module signature "UDLM"
    uint16_t mod_version;        // 4:  module ABI version
    uint16_t udynlink_version;   // 6:  loader ABI version used to compile
    uint16_t arch_tag;           // 8:  target architecture + FPU + float ABI
    uint16_t num_lot;            // 10: number of LOT entries
    uint16_t num_rels;           // 12: number of relocations
    uint16_t num_deps;           // 14: number of dependencies (was zero padding)
    uint32_t symt_size;          // 16: symbol table size in bytes
    uint32_t code_size;          // 20: code section size
    uint32_t data_size;          // 24: data section size
    uint32_t bss_size;           // 28: BSS section size
    uint32_t deps_strtab_size;   // 32: dependency string table size (NEW)
} udynlink_module_header_t;     // total: 36 bytes (was 32)
```

Updated binary layout:
```
[Header (36 bytes)]
[Relocation table (num_rels × 8 bytes)]
[Symbol table (symt_size bytes)]
[Dependency string table (deps_strtab_size bytes)]  ← NEW
[.text (code_size bytes)]
[.data (data_size bytes)]
```

The dependency string table is a packed sequence of null-terminated module names: `"mod_b\0mod_c\0"`. For modules with no dependencies: `num_deps=0, deps_strtab_size=0`.

#### ABI Version Bump

- `UDYNLINK_LOADER_ABI_VERSION` changes from `1.0` (0x0100) to `2.0` (0x0200)
- Old modules (`udynlink_version = 1.0`): `1.0 <= 2.0` → load succeeds ✓
- New modules (`udynlink_version = 2.0`): `2.0 > 1.0` → rejected by old loaders ✗ (correct)

For old modules, the loader treats `num_deps=0` (was padding, always zero) and `deps_strtab_size` is read from offset 32. But old modules are only 32 bytes of header! **Solution**: The loader checks the version first. If `udynlink_version < 2.0`, skip dependency processing entirely (assume `num_deps=0`).

**Important**: When loading old v1.0 modules, the old header is only 32 bytes. The code that reads `deps_strtab_size` must only execute for v2.0+ modules. For v1.0 modules, `deps_strtab_size = 0` (implicit).

#### Module Handle Extension

```c
#ifndef UDYNLINK_MAX_DEPS
#define UDYNLINK_MAX_DEPS 4
#endif

typedef struct _udynlink_module_t {
    const udynlink_module_header_t *p_header;
    union {
        void *p_ram;
        uint32_t ram_base;
    };
    uint8_t info;
    // New dependency fields:
    uint8_t num_deps;
    uint8_t dep_refcount;                              // how many modules depend on this one
    const udynlink_module_t *deps[UDYNLINK_MAX_DEPS];  // dependency handles
} udynlink_module_t;
```

#### New External Callbacks

```c
// In udynlink/udynlink_externals.h:
// Critical symbol resolver (NEW) — always checked first, always wins
uint32_t udynlink_external_resolve_critical_symbol(const char *name);

// Fallback symbol resolver (EXISTING) — checked last, after dependencies
uint32_t udynlink_external_resolve_symbol(const char *name);

// Module handle lookup (NEW) — returns handle for a loaded dependency module
udynlink_module_t *udynlink_external_get_module_handle(const char *module_name);
```

#### New Error Codes

```c
UDYNLINK_ERR_MISSING_DEPENDENCY    // dependency module not found
UDYNLINK_ERR_MODULE_IN_USE         // cannot unload: other modules depend on this one
```

#### Dependency Validation Flow (in `udynlink_load_module`)

After signature + version + arch check but before RAM allocation:

1. If `udynlink_version >= 2.0`: read `num_deps` and dependency strings from module
2. For each dependency name:
   - Call `udynlink_external_get_module_handle(name)`
   - If NULL → `UDYNLINK_ERR_MISSING_DEPENDENCY`
   - Store handle in `p_mod->deps[i]`
   - Increment `dep_refcount` on the dependency module
3. If `num_deps > UDYNLINK_MAX_DEPS` → `UDYNLINK_ERR_MISSING_DEPENDENCY` (with debug message)

#### Extern Symbol Resolution Flow (in relocation loop)

```c
case UDYNLINK_SYM_TYPE_EXTERN:
{
    uint32_t sym_addr = 0;

    // Tier 1: Critical host symbols (always win)
    sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
    if (sym_addr) {
        *p_rel_location = sym_addr;
        break;
    }

    // Tier 2: Search dependency modules
    for (uint8_t d = 0; d < p_mod->num_deps; d++) {
        udynlink_sym_t dep_sym;
        if (udynlink_lookup_symbol(p_mod->deps[d], sym.name, &dep_sym) != NULL) {
            sym_addr = dep_sym.val;
            break;
        }
    }
    if (sym_addr) {
        *p_rel_location = sym_addr;
        break;
    }

    // Tier 3: Fallback host resolver
    sym_addr = udynlink_external_resolve_symbol(sym.name);
    if (sym_addr) {
        *p_rel_location = sym_addr;
    } else {
        res = UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
        goto exit;
    }
    break;
}
```

#### Safe Unload Flow (in `udynlink_unload_module`)

```c
udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod) {
    // Check if other modules depend on this one
    if (p_mod->dep_refcount > 0) {
        return UDYNLINK_ERR_MODULE_IN_USE;
    }

    // Decrement refcount on all dependencies
    for (uint8_t i = 0; i < p_mod->num_deps; i++) {
        if (p_mod->deps[i] != NULL) {
            p_mod->deps[i]->dep_refcount--;
        }
    }

    // ... existing unload logic (free RAM, zero handle)
}
```

#### mkmodule `--depends` Flag

```
python3 scripts/mkmodule mod_consumer.c --depends mod_provider,mod_utils
```

- Validates: non-empty names, no duplicates, count ≤ 255 (uint8_t)
- Encodes: `"mod_provider\0mod_utils\0"` after symbol table in UDLM binary
- Sets: `num_deps = 2`, `deps_strtab_size = 23`, `udynlink_version = 2.0`

#### Impact on Existing Test Infrastructure

- `udynlink_external_resolve_critical_symbol`: Weak default implementation returning 0 (no critical symbols). Test host can override to declare critical symbols like `printf`.
- `udynlink_external_get_module_handle`: Returns module handle by name. Test host maintains array of loaded modules.
- All existing tests: `num_deps=0` (1.0 modules or 2.0 modules without `--depends`). No behavior change.
- New test (`test-deps`): Validates full dependency lifecycle.

#### Files Changed / Added

| File | Change | Complexity |
|------|--------|------------|
| `udynlink/udynlink.h` | Extend header struct, module struct, error codes, ABI version | Medium |
| `udynlink/udynlink.c` | Add dependency validation, 3-tier resolution, safe unload | High |
| `udynlink/udynlink_externals.h` | Add 2 new callbacks (critical_resolve, get_module_handle) | Low |
| `scripts/mkmodule` | Add `--depends` flag, deps section in UDLM binary | Medium |
| `tests/qemu_host/src/main.c` | Implement new callbacks | Medium |
| `tests/test-deps/` | **New** — integration test for dependency tracking | Medium |

---

## Task Breakdown

### Phase 1: Hash-Based Symbol Resolution

> **No changes to existing udynlink.h / udynlink.c / udynlink_externals.h.** Pure addition.

| Task | Description | Agent | Deliverable |
|------|-------------|-------|-------------|
| **1.1** | Implement `udynlink_hash.h` + `udynlink_hash.c` — hash table struct and ~60-line lookup function. Algorithm: GNU hash + bloom filter, port from Mk reference. | `agent` | Two new files in `udynlink/` |
| **1.2** | Implement `scripts/mkhostsyms` — Python tool reading host ELF, generating C header with const hash table data. Port algorithm from Sym2srec `sym2srec_hash.c`. Reuse `udynlink_utils.py` for ELF parsing. | `agent` | New Python script in `scripts/` |
| **1.3** | Add `udynlink_hash.c` to CMake build system (root + test) | `quick` | Updated `CMakeLists.txt` files |
| **1.4** | Integrate into QEMU test host: generate `host_syms.h` from test1.elf, replace `strcmp` chain with hash lookup | `agent` | Updated `tests/qemu_host/src/main.c` + build |
| **1.5** | End-to-end test: all existing tests pass with hash resolution | `agent` | `test_driver.py` green |

### Phase 2: Module Dependency Tracking

> **Modifies udynlink.h, udynlink.c, udynlink_externals.h** — ABI bump to 2.0.

| Task | Description | Agent | Deliverable |
|------|-------------|-------|-------------|
| **2.1** | Extend `udynlink_module_header_t` (32→36 bytes), `udynlink_module_t` (add deps fields), error codes, ABI version bump | `agent` | Updated `udynlink/udynlink.h` |
| **2.2** | Add new callbacks to `udynlink_externals.h`: `udynlink_external_resolve_critical_symbol`, `udynlink_external_get_module_handle` | `quick` | Updated `udynlink/udynlink_externals.h` |
| **2.3** | Implement dependency validation + 3-tier extern resolution + safe unload in `udynlink.c` | `agent` | Updated `udynlink/udynlink.c` |
| **2.4** | Update `scripts/mkmodule` for `--depends` flag + deps section in UDLM binary + v2.0 header | `agent` | Updated `scripts/mkmodule` |
| **2.5** | Implement new callbacks in QEMU test host + add weak defaults for `resolve_critical_symbol` | `quick` | Updated `tests/qemu_host/src/main.c` |
| **2.6** | New integration test: `tests/test-deps/` with provider + consumer modules | `agent` | New test directory |
| **2.7** | All existing tests pass with v2.0 loader (backward compat) | `agent` | `test_driver.py` green |

### Phase 3: Polish

| Task | Description | Agent | Deliverable |
|------|-------------|-------|-------------|
| **3.1** | Update README, codemap, AGENTS.md with new features | `quick` | Updated docs |
| **3.2** | Final CI verification | `agent` | CI green |

---

## Context Guide for Implementation Agents

### Key Files and What to Read

| File | Why Read It | Key Lines |
|------|-------------|-----------|
| `udynlink/udynlink.h` | Public API, data structures, error codes, macros | Lines 31-46 (header struct), 59-66 (module struct), 86-99 (error codes) |
| `udynlink/udynlink.c` | Loader implementation — where dependency validation and resolution changes go | Lines 173-348 (load_module), 274-331 (relocation loop), 314-325 (EXTERN resolution), 362-374 (unload) |
| `udynlink/udynlink_externals.h` | Host callbacks — where new callbacks are added | Lines 28-32 (existing callbacks) |
| `scripts/mkmodule` | Module builder — where `--depends` flag and deps section are added | Lines 282-348 (header construction), 431-449 (CLI), 126-398 (process function) |
| `scripts/udynlink_utils.py` | ELF parsing helpers — reuse for mkhostsyms | Lines 119-189 (ELF functions) |
| `scripts/targets.py` | Target database — no changes needed | — |
| `tests/qemu_host/src/main.c` | Test host — where new callbacks are implemented | Lines 17-44 (existing callbacks) |

### Reference Implementations (Read-Only)

| What | Where | Key Lines |
|------|-------|-----------|
| GNU hash + bloom filter constructor | `/tmp/Sym2srec/sym2srec/src/sym2srec_hash.c` | Lines 53-380 (full algorithm) |
| GNU hash runtime lookup | `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c` | Lines 420-530 (resolveSymbolInternal) |
| DT_NEEDED resolution (filesystem-based) | Same file | Lines 327-412 (resolveSymbolExternal) |
| Old SysV hash lookup (used for deps) | Same file | Lines 260-319 (resolveSymbolExternalLibrary) |
| GNU hash table struct | `/tmp/Mk/Mk/Includes/Loader/mk_loader_elf_types.h` | Lines 371-381 (T_mkELF32GNUHashTable) |

### CMake Build Structure

- **Root `CMakeLists.txt`**: Defines `udynlink` static library target. Add `udynlink_hash.c` here.
- **`tests/qemu_host/CMakeLists.txt`**: Builds test1.elf. The host_syms.h generation should be a custom command here.
- **Test driver** (`tests/test_driver.py`): Orchastrates module compilation, host build, and QEMU execution. May need updates for `--depends` flag in test_data.py.

### Pitfalls to Avoid

1. **Don't modify the existing `udynlink_external_resolve_symbol` signature** — it's the fallback resolver and must stay compatible.
2. **v1.0 modules are only 32 bytes of header** — when reading `deps_strtab_size`, the loader must check `udynlink_version >= 2.0` first. For v1.0, assume no deps.
3. **`udynlink_lookup_symbol` is O(N) per module** — for dependency symbol resolution, this is acceptable since dependency modules are typically small. But don't accidentally create an O(N²) situation by scanning all modules globally.
4. **The test host's `test_resolve_symbol` is a weak symbol** — the new `udynlink_external_resolve_critical_symbol` also needs a weak default (returning 0).
5. **`dep_refcount` fields in `udynlink_module_t`** must be properly initialized to 0. The `mark_module_free` function uses `memset(p_mod, 0, ...)` which handles this.
6. **Hash table strtab offsets** must be byte offsets, not word offsets. The existing udynlink symbol table uses a different encoding (28-bit offset | 4-bit info). Don't confuse the two.
