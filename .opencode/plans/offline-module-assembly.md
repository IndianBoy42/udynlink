# Plan: Offline Assembling / Linking Multiple Modules into One

## Goal
Enable combining multiple independently-built `udynlink` modules into a single loadable module image, resolving inter-module symbol dependencies **offline** (at build time) rather than at runtime.

## Context Summary

### Current Build Pipeline (`scripts/mkmodule`)
1. **Compile**: C/C++ sources → `.o` with `-fPIE -msingle-pic-base`. Exported functions are renamed (`foo` → `__md5__foo`), and assembly prologues are generated that load `r9` (LOT base) from a fixed address.
2. **Link**: All objects + prologue objects are linked with `code_before_data.ld` (origin 0) using `--gc-sections` and `--emit-relocs`.
3. **Post-process** (`make_symbols_local`): Wrapped internals are hidden via `objcopy -L`.
4. **Process** (`process()` in `mkmodule`): Extract `.text`/`.data`/`.bss`, classify symbols (local/exported/external/weak), build LOT, patch `R_ARM_GOT_BREL` relocations to LOT offsets, handle `R_ARM_ABS32`/`R_ARM_TARGET1` data relocations, emit the binary `UDLM` image.

### Runtime Loader (`udynlink/udynlink.c`)
- Loads a single `UDLM` module image.
- Allocates RAM for LOT + data + bss (+ optional code copy).
- Resolves **extern** symbols via three-tier lookup: critical host → loaded dependency modules → host fallback.
- Dependencies are declared at build time with `--depends mod_a,mod_b` and validated at load time via `udynlink_external_get_module_handle()`.

### Key Constraints
- Each module has its own LOT (Linker Offset Table) within its RAM allocation. Code accesses data via `r9`-relative addressing.
- Exported functions have assembly prologues that set `r9` from `UDYNLINK_LOT_BASE_ADDR` before calling the actual implementation.
- The `.bin` format embeds partially-patched LOT offsets in the code section. Data relocations encode original ELF addresses that the loader subtracts from the runtime data base.

---

## Design Options

### Option A: Object-Level Offline Linker (Recommended)
**Approach**: Introduce a new `--emit-archive` mode in `mkmodule` that emits a `.a` archive (all object files + manifest) instead of a `.bin`. Create a new `mkcombine` tool that takes multiple `.a` archives, links them together with `ld`, and runs the existing `process()` step to produce a single `.bin`.

**Why it works**:
- Object files already contain renamed symbols and prologues, but cross-module references are still unresolved (extern).
- GNU `ld` naturally resolves undefined symbols from one archive using defined symbols from another.
- The existing `process()` function then builds a unified LOT for the combined module.
- Data relocations are handled correctly by the linker because we re-link from objects.

**Workflow**:
```bash
# Build individual modules as archives
python3 mkmodule --emit-archive mod_provider.c
python3 mkmodule --emit-archive mod_consumer.c --depends mod_provider

# Combine offline
python3 mkcombine mod_provider.a mod_consumer.a --output mod_combined.bin

# Load as a single module (no runtime dependency resolution needed for provider)
udynlink_load_module(&mod, mod_combined_bin, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
```

**Pros**:
- Leverages GNU `ld` for correct relocation handling.
- No changes to the C runtime loader (combined `.bin` is a standard UDLM module).
- C++ `.init_array` sections naturally merge via the linker script.
- Weak symbols handled correctly by the linker.

**Cons**:
- Requires modules to be built as archives (not applicable retroactively to existing `.bin` files).
- Need to define an archive manifest format (module name, public symbols, dependencies, arch tag).

---

### Option B: Binary-Level Merger
**Approach**: Create `mkcombine` that reads existing `.bin` module images, parses their headers/symbol tables/relocation tables, merges code/data sections, builds a unified LOT, patches embedded LOT offsets in the code, and emits a new `.bin`.

**Pros**:
- Works with any existing `.bin` files; no need to recompile.
- Pure Python, no additional toolchain invocations.

**Cons**:
- **Fragile for data relocations**: `R_ARM_ABS32` entries in the `.bin` encode original ELF addresses that assume the individual module's layout. Concatenating data sections shifts bases, breaking the load-time math unless we also patch data words.
- **LOT slot conflicts**: Module A's LOT slot 0 might be `foo`, while Module B's LOT slot 0 might be `bar`. We must identify every embedded LOT offset in the code and patch it to the unified LOT offset.
- **Complex offset tracking**: Need to map old per-module offsets to new combined offsets for both code and data.

**Verdict**: Technically possible but significantly more complex and error-prone than Option A. Best considered as a future enhancement or fallback.

---

### Option C: Source-Level Combination
**Approach**: Enhance `mkmodule` so that when multiple sources are passed together (including `--depends` declarations), it automatically resolves internal dependencies and strips satisfied deps from the emitted binary.

**Pros**:
- Simplest to implement; no new tools.
- Works within the existing `mkmodule` invocation.

**Cons**:
- Only works when all sources are compiled together in one shot. Cannot combine pre-built modules.
- Does not solve the "offline linking" use case for independently distributed modules.

**Verdict**: A useful convenience feature, but orthogonal to the main goal. Could be added as a side improvement.

---

## Open Questions (Need User Input)

1. **Input Artifacts**: Do you need to combine **pre-built `.bin` files** (e.g., from different teams/vendors), or is it acceptable to require modules to be built as intermediate **`.a` archives** (Option A)? This is the primary architectural fork.

2. **Symbol Name Collisions**: If two modules both export a symbol named `init`, should `mkcombine`:
   - (a) Error out and refuse to combine?
   - (b) Allow one to shadow the other (first wins)?
   - (c) Provide a `--rename-symbol` override mechanism?

3. **Dependency Metadata in Output**: Should the combined module still declare runtime dependencies on modules that were **not** included in the combination? (e.g., combining A+B but C remains a runtime dependency). This implies preserving a subset of the dependency string table.

4. **Loader Changes**: Are you open to modifying the C loader code (`udynlink.c`), or should this be a **pure toolchain/Python** change with zero runtime impact?

5. **Scope of "Module" Identity**: Should the combined module expose all exported symbols from all constituent modules under a **single module name**, or should it retain some form of sub-module identity for symbol namespacing?

---

## Proposed Task Breakdown (Pending Decision)

If **Option A** is selected, the work breaks down into atomic, independently-reviewable tasks:

### Phase 1: Archive Format & `mkmodule` Extension
- Add `--emit-archive` flag to `mkmodule`.
- Package object files into a `.a` archive using `arm-none-eabi-ar`.
- Write a manifest file (JSON or plain text) inside the archive: module name, public symbols, dependency list, arch tag, LOT base.
- Ensure `mkmodule` without `--emit-archive` behaves identically (no regressions).
- **Deliverable**: `mkmodule --emit-archive` works; validation test compiles a module to `.a` and verifies manifest contents.

### Phase 2: `mkcombine` Tool (Core)
- New Python script `scripts/mkcombine`.
- Parse manifest from each input `.a` archive.
- Validate ABI compatibility (arch tag, version).
- Detect exported symbol name collisions (configurable policy).
- Extract all `.o` files from archives into a temp directory.
- Link all objects with `code_before_data.ld` into a combined `.elf`.
- Invoke the existing `process()` logic to emit the final `.bin`.
- Handle `--name`, `--output`, `--target`, `--public-symbols`, and other standard flags.
- **Deliverable**: `mkcombine` can link two simple archives into one `.bin`.

### Phase 3: Dependency Resolution Logic
- When combining modules, identify external symbols that are satisfied by exports from other combined modules.
- These become internal/local in the combined symbol table.
- Dependencies satisfied by the combination are removed from the combined module's `deps_strtab`.
- Unresolved externs remain external for runtime host resolution.
- **Deliverable**: Cross-module calls work without runtime dependency loading.

### Phase 4: Integration Tests
- New test directory `tests/test-combine/`.
- Test case 1: Two modules with one calling the other; combine offline; load single module; verify call works.
- Test case 2: Three-module chain (A → B → C); combine A+B; C remains runtime dep; verify partial offline resolution.
- Test case 3: Symbol collision detection (expect error).
- Test case 4: Weak symbol override across combined modules.
- Test case 5: C++ modules with `__init_array` combined and constructors run.
- Run tests on at least `mps2_an386` and `stm32f429_discovery` platforms.
- **Deliverable**: All new tests pass across platforms.

### Phase 5: Documentation & Justfile Integration
- Add `just combine` recipe to Justfile.
- Update `docs/writing-modules.md` with `mkcombine` usage.
- Update README quick-start with an example.
- **Deliverable**: User can discover and use the feature from docs alone.

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| `--gc-sections` drops prologues or init arrays from combined link | Linker script already has `KEEP(*(.text_nogc))` and `KEEP(*(.init_array))`; verify with tests. |
| `objcopy -L` in existing `mkmodule` makes symbols invisible for recombination | `--emit-archive` must emit objects **before** `make_symbols_local()` is applied. |
| C++ name mangling causes unexpected collisions or hidden symbols | Use `extern "C"` for exported APIs; document this requirement. |
| LOT base address mismatch between combined modules | All use the same compile-time `lot_base`; validate manifests match. |
| Streaming loader compatibility | Combined `.bin` is a standard UDLM image; streaming loader works unchanged. |

---

## Next Step
**Awaiting user decision on the Open Questions above**, particularly:
- Which Option (A, B, or C) matches the intended use case?
- Preferred collision policy?
- Whether partial dependency preservation is needed?

Once clarified, this plan will be refined into strict, executable tasks with specific file paths and function names for implementation agents.
