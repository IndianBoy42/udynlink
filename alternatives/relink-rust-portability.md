# Relink — Rust Module Relocation & udynlink Portability

Source: https://github.com/weizhiao/Relink  
Analyzed: /tmp/Relink

## How Relink Compiles Rust to Relocatable Modules

### Compilation Pipeline

**For ET_DYN (shared objects / .so):**
```bash
rustc <file.rs> --crate-type=cdylib -O -C panic=abort -C linker=rust-lld \
      -C link-arg=--emit-relocs -C link-arg=-rpath -C link-arg=$ORIGIN
```

**For ET_REL (relocatable objects / .o):**
```bash
rustc <file.rs> --crate-type=lib --emit=obj -O -C panic=abort -o <file.o>
```

### Key Architectural Points

- `--emit-relocs` preserves relocation metadata in final .so for Relink's
  `load_scan_first` section-reordering optimizations
- ET_DYN: maps PT_LOAD segments via Mmap, reads PT_DYNAMIC for relocs
  (`src/relocation/dynamic.rs`)
- ET_REL: synthesizes PLT+GOT on the fly (`src/object/layout.rs:103-276`),
  currently hardcoded for x86_64 only — ARM ET_REL returns `Err(Unsupported)`
  (`src/relocation/traits.rs:95-111`)

### ARM Dynamic Relocations Supported (ET_DYN only)

`ArmArch` (`src/arch/arm/relocation.rs:22-34`) handles:
- R_ARM_RELATIVE, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_ABS32
- R_ARM_IRELATIVE, R_ARM_COPY
- R_ARM_TLS_DTPMOD32, DTPOFF32, TPOFF32

### Host Symbol Injection (SyntheticModule)

```rust
let host = SyntheticModule::new("__host", [
    SyntheticSymbol::function("my_host_func", my_host_func as *const ())
]);
loader.relocator().scope([host])
```
`SyntheticModule` (`src/image/synthetic.rs:78-144`) creates a virtual symbol
table from raw function pointers. During relocation, `find_symdef_impl()`
searches scope modules; `SyntheticModule::lookup_symbol` uses BTreeMap lookup.

---

## Could Relink's Rust Approach Be Ported to udynlink?

**Yes, with one mandatory toolchain change.** No hard blockers exist.

### The r9/LOT Compatibility

LLVM's `-C relocation-model=ropi-rwpi` is the Rust equivalent of GCC's
`-msingle-pic-base -mno-pic-data-is-text-relative`. It also reserves `r9`
as the static base register for data access. This perfectly aligns with
udynlink's `UDYNLINK_PREPARE_CALL()` / LOT mechanism.

### The Key Difference: BREL Immediate Relocations

GCC emits `R_ARM_GOT_BREL` (load GOT offset from LOT table).
LLVM/ropi-rwpi emits `R_ARM_THM_MOVW_BREL_NC` (132) and
`R_ARM_THM_MOVT_BREL` (133) — base-relative immediate loads that embed
the offset directly into Thumb-2 `movw`/`movt` instruction bit-fields.

**This is the mandatory change for mkmodule**: must parse and patch these
two relocation types by calculating the data offset relative to LOT base
and injecting it into the instruction immediates.

### Solvable Problems (No Hard Blockers)

| Concern | Solution |
|---------|----------|
| Thumb-2 interop | `pub extern "C"` has identical EABI calling convention. UDYNLINK_CALL works. |
| No heap / malloc | `#[global_allocator]` delegates to `extern "C" { fn udynlink_external_malloc(); }` |
| Panics | `#![no_std]` + `-C panic=abort` + custom `#[panic_handler]` that calls `udynlink_external_vprintf` |
| Global constructors | Rust uses `.init_array` — eh2k's `udynlink_cpp_init()` already handles this |
| Rust-specific ELF sections | `.ARM.exidx`, `.rustc` etc. — mkmodule strips or ignores them |
| ET_REL ARM loading | Not needed — mkmodule links the .o first, then processes the linked ELF |

### What mkmodule --rust Would Need

1. Accept `.rs` files as input
2. Compile with: `rustc --crate-type=lib --emit=obj -O -C panic=abort -C relocation-model=ropi-rwpi`
3. Link the resulting `.o` with `arm-none-eabi-gcc` and udynlink's linker script
4. Parse the linked ELF with extended relocation handling for:
   - `R_ARM_THM_MOVW_BREL_NC` (132) — patch movw immediate with LOT-relative offset
   - `R_ARM_THM_MOVT_BREL` (133) — patch movt immediate with LOT-relative offset
5. All other mkmodule processing (symbol table, binary emission) works unchanged

### Estimated Effort

- BREL immediate relocation parsing in mkmodule: **Medium** (bit-field manipulation
  of Thumb-2 MOVW/MOVT encoding, ~100 lines Python)
- Rust toolchain integration (mkmodule --rust flag): **Low**
- Testing with no_std Rust modules: **Medium** (first module is hardest)

### What NOT to Port from Relink

- Its ET_REL object linker (x86_64-only, not useful)
- Its Mmap/VirtualMemoryApi (heap-based, Linux-specific)
- Its PLT/GOT synthesis (mkmodule + ld already handle this)
- Its BTreeMap-based SymbolTable (heap-based; udynlink's static symbol table is better)
