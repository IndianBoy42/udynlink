# Zig PIC Spike Report

## Summary

**Incompatible with significant caveats.**

Zig's LLVM backend for `thumb-freestanding-eabi` emits a **PC-relative** position-independent model, not the **GOT-based (`r9`/LOT)** model that udynlink's GCC toolchain expects. No `R_ARM_GOT_BREL` relocations are produced. Data is accessed via `ldr [pc, #n] / add pc` sequences, and external functions are called via direct `bl`/`b.w` with `R_ARM_THM_JUMP24`/`R_ARM_THM_CALL` relocations. This is fundamentally different from GCC's `-msingle-pic-base -mno-pic-data-is-text-relative` model which uses `r9` as a LOT base and `R_ARM_GOT_BREL` for all data/function references.

---

## Relocation Types

### `test.zig` (no imports)

| File | Relocation Type | Symbol | Context |
|------|----------------|--------|---------|
| `test_pie.o` | `R_ARM_REL32` | `test.counter` | Data access in `.text` |
| `test_pie.o` | `R_ARM_PREL31` | `.text` | `.ARM.exidx` |
| `test_pie.o` | `R_ARM_NONE` | `__aeabi_unwind_cpp_pr0` | `.ARM.exidx` |
| `test_pic.o` | `R_ARM_REL32` | `test.counter` | Same as `-fPIE` |
| `test_pic_debug.o` | `R_ARM_REL32` | `test.counter` | Same with Debug build |
| `test_llc19_rwpi.o` | `R_ARM_SBREL32` | `test.counter` | **Only when compiled via `llc -relocation-model=rwpi`** |

### `test_import.zig` (has extern `host_printf`)

| File | Relocation Type | Symbol | Context |
|------|----------------|--------|---------|
| `test_import_pie.o` | `R_ARM_THM_JUMP24` | `host_printf` | External function call (ReleaseSmall) |
| `test_import_pie.o` | `R_ARM_REL32` | `__anon_1292` | String literal in `.rodata` |
| `test_import_pie_debug.o` | `R_ARM_THM_CALL` | `host_printf` | External function call (Debug) |
| `test_import_pie_debug.o` | `R_ARM_REL32` | `__anon_1292` | String literal |
| `test_import_llc_rwpi.o` | `R_ARM_THM_JUMP24` | `host_printf` | Still PC-relative even with `llc rwpi` |
| `test_import_llc_rwpi.o` | `R_ARM_ABS32` | `.rodata.str1.1` | String literal (read-only data) |

### GCC Equivalent (`test_gcc.c`)

| File | Relocation Type | Symbol | Context |
|------|----------------|--------|---------|
| `test_gcc.o` | `R_ARM_GOT_BREL` | `counter` | Data access |
| `test_import_gcc.o` | `R_ARM_GOT_BREL` | `.LC0` | String literal |
| `test_import_gcc.o` | `R_ARM_GOT_BREL` | `host_printf` | **External function via GOT** |
| `test_import_gcc.o` | `R_ARM_THM_JUMP24` | `host_printf` | Only when compiled with `--pc-rel` for local string; extern function still via GOT |

**Key finding:** No `R_ARM_GOT_BREL` appears in any Zig object. The only way to get `r9`-based access is by manually compiling Zig's LLVM IR with `llc -relocation-model=rwpi`, which produces `R_ARM_SBREL32` instead.

---

## Section Analysis

All Zig objects contain the same sections regardless of `-fPIE`/`-fPIC`/`-O`:

- `.text` — code
- `.ARM.exidx` — exception index tables
- `.rel.text` — text relocations
- `.rel.ARM.exidx` — exception index relocations
- `.bss` — zero-initialized data (`counter`)
- `.rodata.str1.1` — string literals (in import test)
- `.ARM.attributes` — build attributes
- `.note.GNU-stack` — stack note (empty)
- `.symtab` — symbol table
- `.shstrtab` — section name strings
- `.strtab` — symbol name strings

**No `.got`, `.got.plt`, `.plt`, or `.data.rel.ro` sections exist.**

The debug build (`test_pie_debug.o`) adds many debug sections:
- `.debug_info`, `.debug_abbrev`, `.debug_str`, `.debug_line`, `.debug_frame`, `.debug_pubnames`, `.debug_pubtypes`, `.debug_aranges`

These debug sections contain `R_ARM_ABS32` relocations (not relevant for runtime).

---

## Symbol Table

Zig correctly exports `export fn` symbols:

| Symbol | Type | Bind | Visibility | Section |
|--------|------|------|------------|---------|
| `hello` | `STT_FUNC` | `STB_GLOBAL` | DEFAULT | `.text` |
| `get_counter` | `STT_FUNC` | `STB_GLOBAL` | DEFAULT | `.text` |
| `set_counter` | `STT_FUNC` | `STB_GLOBAL` | DEFAULT | `.text` |
| `greet` | `STT_FUNC` | `STB_GLOBAL` | DEFAULT | `.text` |
| `test.counter` | `STT_OBJECT` | `STB_LOCAL` | DEFAULT | `.bss` |
| `__anon_1292` | `STT_OBJECT` | `STB_LOCAL` | DEFAULT | `.rodata.str1.1` |
| `host_printf` | `STT_NOTYPE` | `STB_GLOBAL` | DEFAULT | UND |

The internal `counter` variable is visible as `test.counter` with `STB_LOCAL` binding.

---

## Disassembly Observations

### Zig Data Access (PC-relative)

```asm
; test_pie.o: get_counter
00000004 <get_counter>:
   4:   4902        ldr     r1, [pc, #8]    ; @ (10 <get_counter+0xc>)
   6:   4479        add     r1, pc
   8:   6808        ldr     r0, [r1, #0]
   a:   3001        adds    r0, #1
   c:   6008        str     r0, [r1, #0]
   e:   4770        bx      lr
  10:   00000006    .word   0x00000006
```

- `ldr r1, [pc, #8]` loads the word at `PC+8` (which contains `0x00000006`)
- `add r1, pc` adds the aligned PC value to the loaded offset
- This gives the absolute address of `counter`
- **No `r9` register is used.**

### GCC Data Access (r9 / GOT)

```asm
; test_gcc.o: get_counter
00000000 <get_counter>:
   0:   4b03        ldr     r3, [pc, #12]   ; @ (10 <get_counter+0x10>)
   2:   f859 3003   ldr.w   r3, [r9, r3]
   6:   6818        ldr     r0, [r3, #0]
   8:   3001        adds    r0, #1
   a:   6018        str     r0, [r3, #0]
   c:   4770        bx      lr
  10:   00000000    .word   0x00000000
```

- `ldr r3, [pc, #12]` loads the word at `PC+12` (GOT offset placeholder)
- `ldr.w r3, [r9, r3]` loads the pointer from the LOT (pointed to by `r9`)
- Then accesses the data via the pointer
- **Uses `r9` as the LOT base.**

### Zig External Function Call

```asm
; test_import_pie.o: greet
00000000 <greet>:
   0:   4801        ldr     r0, [pc, #4]
   2:   4478        add     r0, pc
   4:   f7ff bffe   b.w     0 <host_printf>
   8:   00000002    .word   0x00000002
```

- Direct `b.w` (unconditional branch) with `R_ARM_THM_JUMP24` relocation
- The linker is expected to patch the branch offset with the target address
- **This is a direct PC-relative branch, not an indirect call via a function pointer.**

### GCC External Function Call (via GOT)

```asm
; test_import_gcc.o (linked): __77f6fbc1b__greet
  12:   f859 3003   ldr.w   r3, [r9, r3]
  16:   4718        bx      r3
```

- Loads the `host_printf` address from the GOT (via `r9`)
- Indirect branch via `bx r3`
- The loader patches the GOT entry at load time with `udynlink_external_resolve_symbol`

---

## Link Test

### Direct Linking

**`test_pie.o`** (no imports):
```bash
arm-none-eabi-gcc -nostartfiles -nodefaultlibs -nostdlib \
    -T code_before_data.ld test_pie.o -o test_pie.elf
```
- **Fails** with `undefined reference to __aeabi_unwind_cpp_pr0`
- With a stub `void __aeabi_unwind_cpp_pr0(void) {}`, **linking succeeds**
- The `.word` values in the disassembly are resolved by the linker (e.g., `0x0000002a` for counter offset)

**`test_import_pie.o`** (has extern):
```bash
arm-none-eabi-gcc -nostartfiles -nodefaultlibs -nostdlib \
    -T code_before_data.ld test_import_pie.o stub.o -o test_import_pie.elf
```
- **Fails** with:
  ```
  undefined reference to `host_printf'
  (host_printf): Unknown destination type (ARM/Thumb)
  dangerous relocation: unsupported relocation
  ```
- The linker cannot resolve `R_ARM_THM_JUMP24` for an undefined `host_printf`

### C Shim Linking (Branch B)

Compiling a C shim with GCC's PIC flags and linking with the Zig object:
```bash
arm-none-eabi-gcc -c -fPIE -msingle-pic-base -mno-pic-data-is-text-relative \
    -mcpu=cortex-m4 -mthumb -O2 -o shim.o shim.c
arm-none-eabi-gcc -nostartfiles -nodefaultlibs -nostdlib \
    -T code_before_data.ld test_pie.o shim.o stub.o -o shim_linked.elf
```
- **Linking succeeds.**
- The C shim exports `c_hello`, `c_get_counter`, `c_set_counter` with GCC's PIC model
- The C shim calls Zig's `hello`, `get_counter`, `set_counter` via PC-relative branches
- The Zig functions are in the same module, so PC-relative calls work

### mkmodule Compatibility

The `mkmodule` script expects `R_ARM_GOT_BREL` and errors on unknown relocation types:
```python
elif t != "R_ARM_ABS32" and t != "R_ARM_TARGET1":
    error("Unknown relocation type '%s' for symbol '%s'" % (t, s))
```

`R_ARM_REL32` and `R_ARM_THM_JUMP24` are not recognized, so `mkmodule` would fail on any Zig object.

---

## Comparison: `-fPIE` vs `-fPIC`

| Aspect | `-fPIE` | `-fPIC` | `-O Debug` | `--emit-relocs` |
|--------|---------|---------|------------|-----------------|
| Data relocation | `R_ARM_REL32` | `R_ARM_REL32` | `R_ARM_REL32` | `R_ARM_REL32` |
| Function call | `R_ARM_THM_JUMP24` | `R_ARM_THM_JUMP24` | `R_ARM_THM_CALL` | `R_ARM_THM_JUMP24` |
| Data access | PC-relative | PC-relative | PC-relative | PC-relative |
| `.got` section | None | None | None | None |
| Disassembly | Identical | Identical | Unwind tables + overflow checks | Identical |

**No difference** between `-fPIE` and `-fPIC` for ARM bare-metal. Zig's LLVM backend does not differentiate these for the `thumb-freestanding-eabi` target.

---

## LLVM IR Workaround (`llc -relocation-model=rwpi`)

By compiling Zig's LLVM IR manually with `llc -relocation-model=rwpi`, we can get `r9`-based access:

```bash
zig build-obj -target thumb-freestanding-eabi -mcpu cortex_m4 -fPIC -O ReleaseSmall \
    -femit-llvm-ir test.zig
llc -march=arm -mcpu=cortex-m4 -mattr=+thumb-mode -relocation-model=rwpi \
    -filetype=obj test.ll -o test_llc_rwpi.o
```

Result:
- Data relocation: `R_ARM_SBREL32` (static base relative)
- Disassembly: `ldr.w r0, [r9, r1]` / `str.w r0, [r9, r1]`
- **No `.got` needed; data is accessed directly relative to `r9`**

However:
- **External function calls still use `R_ARM_THM_JUMP24`** (direct PC-relative branch)
- The `rwpi` model only affects **read-write data access**, not read-only data or function calls
- This requires a two-step toolchain: Zig -> LLVM IR -> `llc` -> object, which is not integrated with `mkmodule`
- `R_ARM_SBREL32` is not handled by the current `mkmodule` script

---

## Recommendation

### Pursue **Branch B** (C-shim workaround)

Branch A (native Zig modules) would require **extensive and risky** changes to both the `mkmodule` toolchain and the `udynlink` loader:

1. **Relocation handling**: Add support for `R_ARM_REL32` (PC-relative data) and `R_ARM_THM_JUMP24`/`R_ARM_THM_CALL` (external function calls) in `mkmodule` and the C loader. The current loader only understands `R_ARM_GOT_BREL`, `R_ARM_ABS32`, and `R_ARM_TARGET1`.

2. **XIP mode incompatibility**: PC-relative data access assumes code and data are at a fixed offset from each other. In `UDYNLINK_LOAD_MODE_XIP`, code stays in flash while data is copied to RAM. The PC-relative offset from flash code to RAM data is **not** the same as the linker-computed offset. This breaks the fundamental assumption of `R_ARM_REL32`.

3. **External function calls**: Direct `bl`/`b.w` to host functions cannot be resolved at load time because the instruction encodes a PC-relative offset. The `mkmodule` script ignores `R_ARM_THM_JUMP24`, which works for GCC because GCC never uses it for external functions (it uses GOT-based indirect calls). For Zig, ignoring this relocation means the branch target is a placeholder (0x0), which will crash at runtime.

**Branch B** is the pragmatic path:
- Write a thin C shim with `export` functions compiled by GCC using `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`
- The C shim handles all external symbol references (via GOT) and data access (via LOT/r9)
- The C shim calls into Zig functions, which are compiled to a static object with standard PC-relative PIC
- The Zig object is linked as a regular `.o` file alongside the C shim
- `mkmodule` needs a small modification to accept pre-compiled object files (or the build process can compile the Zig object separately and pass it to the linker)
- This preserves udynlink's existing architecture, XIP support, and host-call model without changing the loader

**Branch B is the only viable path without rewriting core parts of udynlink.**
