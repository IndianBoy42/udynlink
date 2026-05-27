# Fast-Path Symbol Lookup Hook

## Goal

Add a host hook (`udynlink_external_lookup_symbol` weak function) that lets the host provide a custom fast symbol lookup (hash table, bloom filter, etc.) for loaded modules. This sits **on top of** the built-in binary search already implemented in `udynlink_lookup_symbol`.

Precedence: **host hook → built-in binary search → linear scan fallback**.

## Current State (commit `6d860ac4`)

Already implemented:
- `user_ctx` (void*) and `num_named_syms` (uint16_t) added to `udynlink_module_t`
- `deps` changed from fixed array to pointer + `max_deps` capacity
- `UDYNLINK_MAX_DEPS` removed
- `udynlink_check_arch_tag()` extracted as standalone function
- `mkmodule` now emits symbol tables sorted lexicographically (named symbols first, local symbols last)
- `compute_num_named_syms()` validates sort at load time
- `udynlink_lookup_symbol()` uses binary search when `num_named_syms > 0`, linear scan otherwise
- Streaming load path also computes `num_named_syms`

**Broken:** 22/50 tests failing on STM32F429. Known issues:
- Several test files need `deps` array + `max_deps` initialization (partially done in optimization commit but not all tests updated)
- Some symbol lookup failures ("'test' symbol not found" in strip-init-array)
- Some test harness regex matching issues (QEMU prompt text before actual output)

## Design for the Hook

### New weak function

```c
// In udynlink_externals.h:

#define UDYNLINK_LOOKUP_NOT_HANDLED  ((int)-2)
#define UDYNLINK_LOOKUP_NOT_FOUND    ((int)-1)

/**
 * @brief Host-provided fast symbol lookup hook.
 *
 * When overridden, this function replaces both the built-in binary search
 * and the linear scan in udynlink_lookup_symbol().  The host may implement
 * any lookup strategy (hash table, sorted array with binary search, bloom
 * filter, etc.) using udynlink_get_num_symbols() and
 * udynlink_get_symbol() to enumerate symbols during index construction.
 *
 * @param p_mod  Module to search.
 * @param name   Null-terminated symbol name.
 *
 * @return >= 0 symbol index if found,
 *         UDYNLINK_LOOKUP_NOT_FOUND if the symbol does not exist,
 *         UDYNLINK_LOOKUP_NOT_HANDLED if this hook declines to handle
 *         the lookup (the default weak implementation always returns this).
 */
int udynlink_external_lookup_symbol(const udynlink_module_t *p_mod,
                                     const char *name);
```

### New enumeration API

The host needs to enumerate symbols to build its index. These wrap the existing static functions:

```c
// In udynlink.h:

size_t udynlink_get_num_symbols(const udynlink_module_t *p_mod);
udynlink_sym_t *udynlink_get_symbol(const udynlink_module_t *p_mod,
                                     size_t index,
                                     udynlink_sym_t *p_sym);
```

`udynlink_get_symbol()` returns the **raw** symbol (section-relative value, no `offset_sym`). The host uses name/type for indexing only. For resolved values, use `udynlink_lookup_symbol()`.

### Modified `udynlink_lookup_symbol()` logic

Precedence chain:
1. **Host hook** (`udynlink_external_lookup_symbol`) — if returns `>= 0`, use that index
2. **Built-in binary search** — if `num_named_syms > 0`, use binary search
3. **Linear scan fallback** — for old-format unsorted modules

```c
udynlink_sym_t *udynlink_lookup_symbol(...) {
    if (p_mod == NULL) return NULL;

    // 1. Host hook (highest priority)
    int idx = udynlink_external_lookup_symbol(p_mod, name);
    if (idx != UDYNLINK_LOOKUP_NOT_HANDLED) {
        if (idx == UDYNLINK_LOOKUP_NOT_FOUND) return NULL;
        if (get_sym_at(p_mod->p_header, (size_t)idx, p_sym) == NULL) return NULL;
        goto found;
    }

    // 2. Built-in binary search
    if (p_mod->num_named_syms > 0) {
        // ... existing binary search code ...
        // on match: goto found;
        return NULL;
    }

    // 3. Linear scan fallback
    size_t i = 1;
    while (get_sym_at(p_mod->p_header, i++, p_sym) != NULL) {
        if (p_sym->type != UDYNLINK_SYM_TYPE_INTERNAL && !strcmp(p_sym->name, name))
            goto found;
    }
    return NULL;

found:
    offset_sym(p_mod, p_sym);
    if (p_sym->type == UDYNLINK_SYM_TYPE_WEAK) {
        // Three-tier resolution (unchanged)
    }
    return p_sym;
}
```

**Key refactor:** The `offset_sym` + WEAK resolution block appears 3 times currently (binary search path, linear scan path, and would appear again for the hook path). This should be factored into a static helper like `resolve_found_symbol()` to eliminate duplication. Currently this block is duplicated 4+ times across the codebase (load_module, streaming load, lookup_symbol ×2, apply_extern_relocations).

### Default weak implementation

```c
// In udynlink.c:
__attribute__((weak))
int udynlink_external_lookup_symbol(const udynlink_module_t *p_mod,
                                     const char *name) {
    (void)p_mod;
    (void)name;
    return UDYNLINK_LOOKUP_NOT_HANDLED;
}
```

### Host usage example

```c
// After loading, build a hash table index:
udynlink_module_t *mod = udynlink_load_module(base, NULL, mode);
size_t n = udynlink_get_num_symbols(mod);
my_hash_table_t *ht = ht_create(n);
for (size_t i = 0; i < n; i++) {
    udynlink_sym_t sym;
    udynlink_get_symbol(mod, i, &sym);
    if (sym.type != UDYNLINK_SYM_TYPE_INTERNAL)
        ht_insert(ht, sym.name, i);
}
mod->user_ctx = ht;  // stash on the module handle

// Override the weak function:
int udynlink_external_lookup_symbol(const udynlink_module_t *p_mod,
                                     const char *name) {
    my_hash_table_t *ht = (my_hash_table_t*)p_mod->user_ctx;
    if (ht == NULL) return UDYNLINK_LOOKUP_NOT_HANDLED;
    int idx = ht_lookup(ht, name);
    return idx >= 0 ? idx : UDYNLINK_LOOKUP_NOT_FOUND;
}
```

---

## Task Breakdown

### Task 0 (Prerequisite): Fix existing test failures
**Priority:** Must be done before any new feature work.
**Known issues:**
- Tests that need `deps`/`max_deps` initialization not yet updated
- Symbol lookup failures in some test configurations
- Test harness regex matching issues

**Approach:** Dedicated investigation + fix session(s). Run `just test-f429` and `just test-mps2` to validate.

### Task 1: Add enumeration API
**Files:** `udynlink/udynlink.h`, `udynlink/udynlink.c`
**Deliverables:**
- `size_t udynlink_get_num_symbols(const udynlink_module_t *p_mod)` — wraps `*get_sym_table_pointer(p_mod->p_header)`
- `udynlink_sym_t *udynlink_get_symbol(const udynlink_module_t *p_mod, size_t index, udynlink_sym_t *p_sym)` — wraps `get_sym_at()`
- Guard against NULL `p_mod` / NULL `p_header`
- Doc comments

### Task 2: Factor out `resolve_found_symbol()` helper
**Files:** `udynlink/udynlink.c`
**Deliverables:**
- Static helper `resolve_found_symbol(p_mod, p_sym)` that applies `offset_sym()` + WEAK three-tier resolution
- Replace the duplicated block in:
  - `udynlink_lookup_symbol()` binary search path
  - `udynlink_lookup_symbol()` linear scan path
  - (Bonus but out of scope for this task: `udynlink_load_module()` and streaming load — those write to relocation slots, not return sym_t)
- Verify all existing tests still pass

### Task 3: Add lookup hook (weak function + constants)
**Files:** `udynlink/udynlink_externals.h`, `udynlink/udynlink.c`
**Deliverables:**
- `UDYNLINK_LOOKUP_NOT_HANDLED` and `UDYNLINK_LOOKUP_NOT_FOUND` constants in `udynlink.h` (not externals.h — they're used by the loader too)
- `int udynlink_external_lookup_symbol(const udynlink_module_t *p_mod, const char *name)` declaration + doc in `udynlink_externals.h`
- Default weak implementation in `udynlink.c`
- Wire into `udynlink_lookup_symbol()` as the first check (before binary search)
- Verify existing tests still pass (default weak returns NOT_HANDLED → no behavior change)

### Task 4: Integration test for lookup hook
**Files:** New test `tests/test-lookup-hook/` with `test_qemu.c` and `test_data.py`
**Deliverables:**
- Test module with multiple exported symbols
- Host implements `udynlink_external_lookup_symbol` with a simple sorted array + binary search
- Verify: `udynlink_lookup_symbol` returns correct values through the hook
- Verify: `udynlink_get_symbol_value` works through the hook
- Verify: Non-existent symbol returns NULL via hook
- Verify: WEAK symbols still get three-tier resolution
- Verify: `user_ctx` round-trip works (stash index on module, retrieve in hook)
- Verify: Hook returning NOT_HANDLED falls through to binary search/linear scan

### Task 5: Update documentation
**Files:** `docs/api-reference.md`, `docs/integrating-as-host.md`, `CHANGELOG.md`
**Deliverables:**
- Document `udynlink_external_lookup_symbol` in the externals reference
- Document `udynlink_get_num_symbols` and `udynlink_get_symbol` in the API reference
- Document `num_named_syms` and `user_ctx` fields in the module struct reference
- Add a section on building custom lookup indices in `integrating-as-host.md`
- Changelog entry

---

## Context Exploration Guide (for implementation agents)

### Key files
- `udynlink/udynlink.h` — Module struct, symbol types, public API declarations
- `udynlink/udynlink.c` — Core loader implementation. Key functions:
  - `get_sym_table_pointer()` (line ~180): computes symbol table address from header
  - `get_sym_at()` (line ~201): reads a single symbol entry by index
  - `offset_sym()` (line ~232): relocates section-relative → absolute address
  - `compute_num_named_syms()` (line ~188): validates sorted symbol table
  - `udynlink_lookup_symbol()` (line ~699): the function being modified
  - WEAK three-tier resolution: appears at lines ~717, ~749, and in load/stream paths
- `udynlink/udynlink_externals.h` — Weak host callback declarations
- `scripts/mkmodule` — Python toolchain that builds modules. Symbol sorting at line ~350
- `tests/test_driver.py` — Test harness
- `tests/test_*/test_data.py` — Per-test expected output patterns

### Pattern: how to add a weak external
1. Declare in `udynlink_externals.h` with full doc comment
2. Add weak default implementation at top of `udynlink.c`
3. Call from the appropriate place in the codebase
4. Follow the pattern of `udynlink_external_resolve_symbol` (returns 0 = not found, non-zero = found)

### Pattern: how to add a test
1. Create `tests/test-<name>/` with:
   - `mod_*.c` or `mod_*.cpp` — module source
   - `test_qemu.c` — host test code (includes `test_api.h`)
   - `test_data.py` — expected output patterns
2. Register in `tests/config.py` if needed
3. Run with `just test-f429-single test-<name>`

### WEAK three-tier resolution block (for the `resolve_found_symbol` refactor)
The block appears in these locations in `udynlink.c`:
1. `udynlink_lookup_symbol()` binary search path (~line 717)
2. `udynlink_lookup_symbol()` linear scan path (~line 749)
3. `udynlink_load_module()` memory-mapped load (~line 530)
4. `udynlink_load_module_from_stream_impl()` streaming load (~line 1025)
5. `apply_extern_relocations()` post-link re-resolution (~line 1115)

Items 3-5 write to relocation slots (`p_rel_location`), not to `p_sym`. They have a different shape (skipping DEFERRED, handling extern errors). **Only items 1-2 should be factored into `resolve_found_symbol()`** — items 3-5 are too different in structure and purpose.
