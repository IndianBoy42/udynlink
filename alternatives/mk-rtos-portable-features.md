# Mk RTOS ELF Loader — Portable Features for udynlink

Source: https://github.com/EmbSoft3/Mk  
Analyzed: /tmp/Mk

## Feature 1: O(1) Host Symbol Resolution via Offline GNU Hash & Bloom Filters

Mk uses an offline tool (`sym2srec`) to parse the host firmware ELF, extract
public symbols, and generate a GNU Hash Table + Bloom filter. This table is
embedded in host flash at a fixed address. At runtime, Mk hashes the symbol
string, checks the Bloom filter to instantly reject misses, then does an O(1)
bucket lookup (`mk_loader_elf_resolveExternalSymbols.c:420-530`).

**udynlink currently**: `udynlink_external_resolve_symbol` is a host callback
typically implemented as a massive array of strings with linear `strcmp` — O(N).

**Integration plan**:
1. Add `scripts/mkhostsyms.py` — takes host firmware ELF, generates C header
   with pre-computed GNU hash table + Bloom filter + symbol addresses
2. Add helper function to udynlink core:
   `void* udynlink_resolve_hashed_symbol(const udynlink_hash_table_t* table, const char* name)`
3. Host implements its callback simply as a hash table lookup
4. ~50-line C lookup function, no heap allocation

**Complexity**: Medium

## Feature 2: Standard ET_DYN (PIE) Execution Model & R_ARM_RELATIVE

Mk loads standard `ET_DYN` (PIE) files produced by `arm-none-eabi-gcc -pie -fPIE`.
It applies `R_ARM_RELATIVE` relocations (`mk_loader_elf_relocate.c:45`) to
adjust global data pointers based on final load address.

**udynlink currently**: Uses custom LOT (Linker Offset Table) via `r9` register,
requiring custom assembly prologues for every exported function and a custom
Python tool (`mkmodule`) to produce `UDLM` format.

**Integration plan**:
1. Add `R_ARM_RELATIVE` relocation handler to udynlink: `*addr = *addr + load_base`
2. Update mkmodule to retain standard relocations instead of converting to custom format
3. Modules become standard ARM PIE binaries — eliminates rigid `r9` / LOT_BASE constraint
4. XIP mode still works (data in RAM gets relocated; code stays in flash)

**Complexity**: High

## Feature 3: Module Dependency Tracking (DT_NEEDED)

Mk parses `PT_DYNAMIC` for `DT_NEEDED` tags and recursively loads dependencies
before resolving symbols (`mk_loader_elf_resolveExternalSymbols.c:359`).

**udynlink currently**: No concept of dependencies. If Module A calls Module B,
the host must manually ensure B is loaded and handle routing in
`udynlink_external_resolve_symbol`.

**Integration plan** (filesystem-free):
1. Update mkmodule to extract dependency strings from ELF → new `UDLM` header section
2. Add dependency array to `udynlink_module_t`
3. Loader checks dependencies during `udynlink_load_module` via new host callback:
   `void* udynlink_external_get_module_handle(const char* module_name)`
4. If dependency missing → `UDYNLINK_ERR_MISSING_DEPENDENCY`
5. Automatically route symbol lookups to dependent module export tables
6. Solve known issue: "Module unload doesn't verify dependents"

**Complexity**: Medium
