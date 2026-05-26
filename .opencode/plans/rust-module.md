# Rust Module Support for udynlink — Detailed Sub-Plan

**Status:** Draft — Investigation complete, awaiting user review  
**Last updated:** 2026-05-26  

---

## 1. Goal

Enable developers to write `no_std` Rust crates, compile them, and produce udynlink-compatible `.bin` modules that the (slightly extended) C loader can load and execute on any supported Cortex-M target.

**Non-goal:** Using Rust's native PIC/RWPI/ROPI models. Investigation showed none produce `R_ARM_GOT_BREL` and all have significant bugs or `core` library incompatibilities.

---

## 2. Investigation Findings (Summary)

A full investigation of all Rust relocation models revealed:

### 2.1 The Core Problem

**No Rust relocation model produces `R_ARM_GOT_BREL`** relocations. The existing udynlink loader was built entirely around GCC's `-fPIE -msingle-pic-base` model which exclusively uses `R_ARM_GOT_BREL` for data access via r9.

| Model | Produces Relocs | Compatible? | Notes |
|-------|----------------|-------------|-------|
| `static` | `R_ARM_THM_MOVW_ABS_NC`, `R_ARM_THM_MOVT_ABS`, `R_ARM_ABS32` | ✅ With loader extension | `core` lib uses this anyway |
| `pic`/`pie` | `R_ARM_REL32` (user code) + `MOVW/MOVT_ABS` (`core`) | ❌ `core` is still absolute | Only ~10 user relocations are PIC |
| `rwpi` | `R_ARM_THM_MOVW_BR`, `R_ARM_THM_MOVT_BR` | ❌ Unsound (rust#131300), linker errors | r9 reserved but `core` ignores it |
| `ropi-rwpi` | Mixed PR + BR | ❌ Codegen bugs (rust#95871) | Workaround `codegen-units=1` insufficient |

### 2.2 The `core` Library Is Always Static

Even with `-C relocation-model=pic`, the linked binary contains **thousands of absolute MOVW/MOVT relocations** from `core`:
- `R_ARM_THM_MOVW_ABS_NC`: ~1,210 instances
- `R_ARM_THM_MOVT_ABS`: ~1,210 instances
- Only ~10 `R_ARM_REL32` from user code

**This means we MUST handle MOVW/MOVT patching regardless of chosen model.**

### 2.3 Recommended Model: `static`

**Compile with:**
```bash
rustc --target thumbv7em-none-eabihf \
    -C relocation-model=static \
    -C panic=abort \
    -C codegen-units=1 \
    -C link-arg=-Tpath/to/rust_module.ld
```

**Advantages:**
- Simplest relocation model (only absolute addresses)
- Consistent with `core` library (no mixed models)
- No rust-lld linker quirks (GNU ld also works)
- All relocations are of types we can handle

**What the loader must patch:**
- `R_ARM_THM_MOVW_ABS_NC` + `R_ARM_THM_MOVT_ABS` pairs: encode absolute addresses in Thumb-2 instructions
- `R_ARM_ABS32`: absolute 32-bit pointers in `.data`
- `R_ARM_THM_CALL` / `R_ARM_THM_JUMP24`: PC-relative, can be ignored

### 2.4 Instruction Format: Thumb-2 MOVW/MOVT

The loader must decode/encode Thumb-2 immediate fields:

```
MOVW:  F2 40 | i(1) | imm4(4) | 0 | imm3(3) | Rd(4) | imm8(8)
       hw1              hw2
       
MOVT:  F2 C0 | i(1) | imm4(4) | 0 | imm3(3) | Rd(4) | imm8(8)
       hw1              hw2

imm16 = (i << 11) | (imm3 << 8) | imm8 | (imm4 << 12)
```

For patching, each instruction encodes half of a 32-bit address. The loader must:
1. Apply the section load offset (delta between link address and load address)
2. Extract the new address (sym_value + delta)
3. Split into low/high 16-bit halves
4. Re-encode into the instruction

### 2.5 External Symbol Handling

Rust `extern "C" { fn printf(...) }` generates `R_ARM_THM_CALL` (BL instruction) with an undefined symbol. However, the linker may fail to resolve this or produce a stub.

**Approach:** For external/host symbols, the proc-macro pattern should generate a **function pointer stub** that the loader patches:

```rust
// In the Rust source, user writes:
#[udynlink_extern]
pub fn printf(fmt: *const u8, ...) -> i32;

// Proc-macro expands to:
#[no_mangle]
extern "C" fn udynlink_stub_printf(fmt: *const u8, ...) -> i32 {
    unsafe {
        core::arch::asm!(
            "b {target}",
            target = sym __udynlink_extern_printf,
            options(noreturn)
        );
    }
}

#[no_mangle]
extern "C" fn __udynlink_extern_printf() -> ! {
    loop {} // Placeholder, patched by loader to host function address
}
```

Actually, a simpler approach: use the same LOT-based resolution. The C toolchain handles externs by placing their addresses in the LOT. The loader resolves externs at load time by calling the host's `udynlink_external_resolve_symbol()`.

For Rust, we can do the same: the post-processor creates LOT entries for extern symbols. But MOVW/MOVT instructions encode absolute addresses directly, not via LOT offsets.

**Simpler approach for externs:** The post-processor treats extern symbols like locals but marks them in the symbol table as `STB_GLOBAL` / `SHN_UNDEF`. At load time, the loader resolves their addresses. The patching code for `MOVW_ABS_NC` / `MOVT_ABS` already handles any symbol (local, exported, or extern) by using its resolved address.

Example: `printf` is `SHN_UNDEF` in the ELF. The post-processor records it as `type=extern` in the udynlink symbol table. At load time, the loader resolves it via `udynlink_external_resolve_symbol()` and gets its host address. Then the MOVW/MOVT patching code uses that address.

**This works!** No special extern handling needed beyond what's already in the loader's three-tier symbol resolution.

---

## 3. Architecture: How It Works

### 3.1 Build Pipeline

```
Rust Source (.rs)
    |
    v
[rustc] -C relocation-model=static -T rust_module.ld
    |
    v
Rust ELF (.elf)
    |
    v
[udynlink-rust-post.py] (extends mkmodule workflow)
    - Read .text, .data, .bss, .rodata (merged into .text)
    - Parse symbols (classify local/exported/extern)
    - Parse relocations:
        * R_ARM_THM_MOVW_ABS_NC + MOVT_ABS → code relocations
        * R_ARM_ABS32 → data relocations
        * R_ARM_THM_CALL / R_ARM_THM_JUMP24 → ignore
    - Build LOT: one entry per unique relocated symbol
    - Wrap exported functions with assembly prologues (like C toolchain)
    - Build symbol table
    - Emit UDLM binary
    |
    v
UDLM Binary (.bin) → udynlink_load_module() → Loader patches
```

### 3.2 Loader Extension Required

The existing loader handles:
- `R_ARM_GOT_BREL` (LOT patching)
- `R_ARM_ABS32` / `R_ARM_TARGET1` (data patching)
- Flag bits 30/31 (code/data base offset)

**Must add to `udynlink.c`:**
- `R_ARM_THM_MOVW_ABS_NC` (code patching: decode instruction, apply offset, re-encode)
- `R_ARM_THM_MOVT_ABS` (code patching: same, for high half)
- `R_ARM_THM_CALL` (ignore: PC-relative)
- `R_ARM_THM_JUMP24` (ignore: PC-relative)

**No changes to `udynlink.h` data structures needed.** The relocation format (offset, symt_offset pairs) is already flexible. The relocation type is determined by the symbol's classification in the symbol table, not by a separate type field.

Wait — actually the current loader determines relocation type by looking up the symbol in the symbol table. It doesn't have a relocation type field.

Looking at `udynlink.c`:
```c
uint32_t lot_offset = *p_rels ++;
uint32_t symt_offset = *p_rels ++;
```

The loader looks up `symt_offset` in the symbol table and gets the symbol type. But for `MOVW_ABS_NC`, the `symt_offset` is the symbol index, and the `lot_offset` tells it which instruction to patch in the code.

For MOVW/MOVT relocations, we need to store the **instruction offset** in `lot_offset`, not a LOT index. But the current `lot_offset` is used to decide whether a relocation patches the LOT or `.data`:
```c
uint32_t *p_rel_location = (lot_offset < p_header->num_lot) ? p_lot + lot_offset : p_data + lot_offset - p_header->num_lot;
```

For MOVW/MOVT, we DON'T want to patch a memory location. We want to patch an instruction in the code. The current loader doesn't handle this case.

**We need a new flag bit or relocation type encoding.**

### 3.3 New Relocation Encoding

Option A: Add a flag bit to `symt_offset`:
- `(1 << 29)` = MOVW_ABS_NC relocation (patch code instruction)
- `(1 << 28)` = MOVT_ABS relocation (patch code instruction)

But we're running low on flag bits (30 and 31 are already used).

Option B: Use `lot_offset` values ≥ some threshold to indicate code patching.
But `lot_offset` is compared against `num_lot` to decide LOT vs data.

Option C: Extend the header to include a `num_code_rels` field and add them after the regular relocations.

Option D: **Reuse the existing relocation table format but add a new relocation type in the symt_offset flags.**

Looking at the current flag bits:
- Bit 31: data relocation (add data base - value)
- Bit 30: code relocation (add code base to value)

We could add:
- Bit 29: MOVW relocation (patch 32-bit instruction at `lot_offset`)
- When bit 29 is set, `symt_offset` (without flag bits) is the symbol index, and `lot_offset` is the byte offset of the instruction in the code section.

Then the loader does:
```c
if (symt_offset & (1 << 29)) {
    uint32_t *p_instr = (uint32_t*)(get_code_pointer(p_mod) + lot_offset);
    // Look up symbol, resolve address, patch instruction
}
```

But `lot_offset` for MOVW/MOVT pairs: do we need to patch both instructions separately, or can we compute the full address from the symbol and patch both?

MOVW encodes the low 16 bits, MOVT encodes the high 16 bits. The linker records them as two separate relocations. The post-processor would emit two separate relocation records.

**Revised encoding:**
- Flag bit 29 = instruction relocation
- `lot_offset` = byte offset of instruction in code section
- `symt_offset` = symbol index (same as before)
- The loader patches the instruction bytes in-place

For MOVW: set low 16 bits of symbol address
For MOVT: set high 16 bits of symbol address

Both use the same mechanism. The loader just needs to know whether to patch the low or high half. This info can be encoded in a new flag bit or derived from the relocation type stored in the symt_offset.

Wait, we don't have a relocation type in the binary. We just have (lot_offset, symt_offset). We need to encode the relocation type somehow.

**Simplest approach:** Use two new flag bits:
- Bit 29: `R_ARM_THM_MOVW_ABS_NC` relocation
- Bit 28: `R_ARM_THM_MOVT_ABS` relocation

When bit 29 is set: `lot_offset` = instruction byte offset, `symt_offset & ~0x30000000` = symbol index. The loader patches the instruction at that offset with the low 16 bits of the resolved symbol address.

When bit 28 is set: same, but patch with the high 16 bits.

This is clean and doesn't change the header format.

### 3.4 Symbol Table for Externs

For extern symbols, the current loader expects:
- Symbol type = `UDYNLINK_SYM_TYPE_EXTERN` (2)
- Value = 0 (will be resolved at load time)
- Name = the function name

The loader looks up the symbol, sees it's extern, calls `udynlink_external_resolve_symbol()`, and uses the returned address to patch the relocation. This already works for LOT and data relocations. It will also work for instruction relocations once we add the patching logic.

---

## 4. Custom Linker Script for Rust Modules

Rust keeps `.rodata` separate. Udynlink expects `.rodata` merged into `.text` (or adjacent with known offsets). The linker script must:
1. Place `.text` at 0x0
2. Merge `.rodata` into `.text`
3. Place `.data` immediately after `.text`
4. Place `.bss` after `.data`
5. Create `__init_array` markers (for C++ compat, even if not used in Rust)

```ld
/* rust_module.ld - Linker script for udynlink Rust modules */
ENTRY(_start)

MEMORY {
    all (RWX) : ORIGIN = 0x00000000, LENGTH = 0xFFFFFFFF
}

SECTIONS {
    .text : ALIGN(4) {
        KEEP(*(.text._start))
        *(.text .text*)
        *(.rodata .rodata*)
        . = ALIGN(4);
    } > all

    .data : ALIGN(4) {
        *(.data .data*)
        KEEP(*(.init))
        __preinit_array_start = .;
        KEEP (*(.preinit_array))
        __preinit_array_end = .;
        __init_array_start = .;
        KEEP (*(.init_array))
        __init_array_end = .;
        . = ALIGN(4);
    } > all

    .bss : ALIGN(4) {
        *(.bss .bss*)
        *(COMMON)
        . = ALIGN(4);
    } > all

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
```

---

## 5. Phase 1: Proc-Macro (`#[udynlink_export]`)

### 5.1 Design

The proc-macro operates at compile time and injects metadata into a custom ELF section. It does NOT generate inline assembly (too fragile for calling conventions).

```rust
// User writes:
#[udynlink_export]
pub extern "C" fn hello(arg: i32) -> i32 {
    arg + 42
}

// Proc-macro expands to:
#[no_mangle]
#[link_section = ".text"]  // Ensure function goes in .text
pub extern "C" fn __udynlink_inner_hello(arg: i32) -> i32 {
    arg + 42
}

#[no_mangle]
pub extern "C" fn hello(arg: i32) -> i32 {
    // This is a placeholder that the post-processor replaces
    // with actual prologue+trampoline.
    // For now, it's just a tail call to avoid linker complaints.
    __udynlink_inner_hello(arg)
}

// Metadata in custom section:
#[link_section = ".udynlink.exports"]
#[used]
static UDYLNK_EXPORT_HELLO: [u8; 20] = *b"hello\0__udynlink_inner_hello\0";
```

Actually, to avoid the placeholder function being inlined/optimized away, the proc-macro should instead:
1. Rename the function to `__udynlink_inner_hello`
2. Add a `#[no_mangle]` on `hello` that points to a `link_section` directive
3. The post-processor reads `.udynlink.exports` and generates `hello_prologue.s` with the actual prologue
4. The generated `.s` is assembled and linked in a second pass (or the post-processor patches the binary directly)

**Revised design:**
1. Proc-macro renames `fn hello` to `fn __udynlink_inner_hello`
2. Proc-macro adds metadata to `.udynlink.exports` section
3. Post-processor reads the ELF, extracts exports from `.udynlink.exports`
4. Post-processor generates assembly prologue files (one per architecture: armv6m/armv7m/armv8m)
5. Post-processor assembles prologues, links with original object, extracts final binary

Wait — this requires a two-pass link: compile Rust → post-process → assemble prologues → re-link. This is exactly what mkmodule does for C, but with Rust.

Simpler alternative: the post-processor patches the ELF in-place. Rust compiles the function. The post-processor:
1. Reads the function symbol address
2. Inserts assembly prologue bytes before the function code
3. Adjusts the function symbol to point to the prologue
4. Patches all relocations that reference the old function address

This is more complex. Better to follow the C toolchain model: generate `.s` files and re-link.

### 5.2 Two-Pass Build

```bash
# Pass 1: Compile Rust
rustc --target thumbv7em-none-eabihf \
    -C relocation-model=static \
    -C link-arg=-T rust_module.ld \
    src/lib.rs -o module_rust.elf

# Pass 2: Post-process
python3 scripts/rust2udynlink.py \
    --target cortex-m4 \
    --bin-name module.bin \
    module_rust.elf
```

`rust2udynlink.py` does:
1. Read ELF from rustc
2. Read `.udynlink.exports` section
3. Generate `prologue.s` (using templates like `scripts/asm_template_*.tmpl`)
4. Assemble `prologue.s` → `prologue.o`
5. Extract `.text`, `.data`, `.bss` from Rust ELF
6. Link Rust object + prologue.o with `code_before_data.ld`
7. Process the linked ELF into UDLM format (same as mkmodule)

Wait, but the Rust ELF is already linked by rust-lld. We can't easily extract and re-link.

Alternative: `rust2udynlink.py` directly processes the Rust ELF:
1. Read sections from Rust ELF
2. Read symbols and relocations
3. Generate prologues as raw bytes and prepend to .text
4. Adjust symbol addresses
5. Build UDLM binary directly

This avoids re-linking but requires the post-processor to understand the full ELF structure.

Given that we already have `pyelftools` and `udynlink_utils.py` that can parse ELF, extract sections, classify symbols, and parse relocations, the direct post-processing approach is feasible.

**Final pipeline:**
```
rustc → module_rust.elf
python3 rust2udynlink.py module_rust.elf → module.bin
```

No re-linking needed. The post-processor constructs the UDLM binary directly from the Rust ELF.

---

## 6. Phase 2: Python Post-Processing Tool (`scripts/rust2udynlink.py`)

### 6.1 Input

A Rust ELF compiled with:
```bash
rustc --target thumbv7em-none-eabihf \
    -C relocation-model=static \
    -C panic=abort \
    -C codegen-units=1 \
    -C link-arg=-T rust_module.ld \
    -C link-arg=--emit-relocs \
    src/lib.rs -o module_rust.elf
```

### 6.2 Processing Steps

1. **Read sections**
   - `.text`: code (merge with `.rodata` if present)
   - `.data`: initialized data
   - `.bss`: uninitialized data
   - `.udynlink.exports`: custom section with export metadata

2. **Read relocations**
   - `R_ARM_THM_MOVW_ABS_NC`: instruction offset + symbol
   - `R_ARM_THM_MOVT_ABS`: instruction offset + symbol
   - `R_ARM_ABS32`: data offset + symbol
   - `R_ARM_THM_CALL`, `R_ARM_THM_JUMP24`: ignore (PC-relative)

3. **Classify symbols**
   - Exported: defined in module + marked in `.udynlink.exports` + `STB_GLOBAL`
   - Local: defined in module + `STB_LOCAL`
   - External: `SHN_UNDEF` + `STB_GLOBAL`

4. **Build LOT**
   - Each unique symbol referenced by a relocation gets a LOT index
   - Local/exported symbols: LOT value = symbol address (adjusted for load time)
   - External symbols: LOT value = 0 (resolved at load time)

5. **Patch code**
   Wait — with the `static` model, the instructions already contain absolute addresses. But in the UDLM binary, the code is copied from the ELF at link time. The addresses in the instructions are link-time addresses (typically 0x0 based if our linker script starts at 0).
   
   Actually, if `.text` starts at 0x0 in the linker script, then all code addresses are already relative to the start. At load time, the loader copies code to its execution address. For XIP mode, code stays at base_addr. For COPY_CODE/COPY_ALL, code goes to a known RAM address.
   
   But the MOVW/MOVT instructions encode absolute addresses of DATA, not code. For example:
   ```asm
   movw r0, #0xd13c   ; encodes address of COUNTER
   movt r0, #0x0002
   ```
   Here, `0x0002d13c` is the link-time address of `COUNTER` in `.data`.
   
   At load time, `.data` is copied to `ram_addr + lot_size`. So `COUNTER`'s runtime address is `data_base + (link_addr - data_link_base)`.
   
   The MOVW/MOVT pair encodes the full 32-bit address. The instruction is a 32-bit Thumb-2 instruction at some offset in `.text`. The loader must:
   1. Find the instruction at offset `reloc_offset` in the code
   2. Decode the imm16 from the instruction
   3. Compute the full address from the pair (or handle each half separately)
   4. Apply the load-time delta (runtime address - link address)
   5. Re-encode into the instruction bytes

   Wait — this is different from the existing loader model. The existing loader patches LOT entries or `.data` words. It doesn't patch instructions.

   For MOVW/MOVT, each relocation points to a specific instruction. The linker expects two relocations (one for MOVW, one for MOVT) to patch a pair. In the Rust ELF:
   - Reloc 1: `R_ARM_THM_MOVW_ABS_NC` at offset X for symbol S
   - Reloc 2: `R_ARM_THM_MOVT_ABS` at offset X+4 for symbol S

   But actually, the offsets might not be adjacent if there are other instructions between them. The linker generates the pair together, so they should be adjacent.

   **More importantly:** Do we need to patch instructions at all? If the linker script places everything at 0, then the link-time addresses ARE offsets. At runtime:
   - Code runs from `code_base` (which could be the module's loaded address)
   - Data is at `data_base`
   - The MOVW/MOVT address `0x0002d13c` is the offset of `COUNTER` from the start of the module
   - At load time, `data_base = ram_addr + lot_size * 4`
   - The actual runtime address of `COUNTER` is `data_base + 0x2d13c - data_link_base`
   - But if `data_link_base = code_size` (because `.data` follows `.text` in the linker script), then `COUNTER`'s runtime address is `data_base + (0x2d13c - code_size)`

   Hmm, this is getting complicated. Let me think about this differently.

   **Alternative: Don't patch instructions at load time. Patch them at build time.**

   The post-processor can pre-compute the layout:
   - Header size + relocations + symbol table = metadata size
   - Code comes after metadata at some offset
   - Data comes after code
   - The post-processor knows the final layout
   - It calculates the expected load addresses
   - It patches the MOVW/MOVT instructions in the binary to encode the correct offsets
   - Then it emits the UDLM binary
   
   But at load time, the data section is copied to RAM. The addresses depend on:
   - The base address where the module is loaded
   - The LOT size
   - The load mode (XIP, COPY_CODE, COPY_ALL)

   For XIP mode:
   - Code stays in flash at `base_addr + metadata_size`
   - Data is at `ram_addr + lot_size * 4`
   - The offset from code to data is not known at build time

   For COPY_ALL/COPY_CODE:
   - Code is at `ram_addr + lot_size * 4`
   - Data is at `ram_addr + lot_size * 4 + code_size`

   So the absolute addresses depend on the load address, which is known only at load time.

   CONCLUSION: We MUST patch instructions at load time. The loader needs to understand MOVW/MOVT and patch them with the correct runtime addresses.

   **But wait — can we encode the addresses relative to section bases instead?**
   
   In the C toolchain, the LOT stores absolute addresses. The code accesses data through the LOT (`ldr.w r3, [r9, r3]` where r3 is a LOT index). At load time, the LOT entries are patched with absolute addresses.
   
   In the Rust `static` model, data is accessed directly via absolute addresses encoded in MOVW/MOVT. To make this work with the udynlink loader, we have two options:
   
   **Option 1: Load-time instruction patching (loader extension)**
   - Keep MOVW/MOVT in the code
   - Loader patches each instruction pair at load time
   - Pros: matches Rust output exactly, no build-time transformation
   - Cons: requires loader to decode/encode Thumb-2 instructions
   
   **Option 2: Convert to LOT-based access (post-processor transformation)**
   - Replace MOVW/MOVT pairs with `ldr r3, [pc, #off]` → `ldr.w r3, [r9, r3]`
   - This is what GCC does with `-fPIE -msingle-pic-base`
   - Pros: no loader changes needed, works with existing loader
   - Cons: complex binary transformation, changes instruction count, may break CALL/JUMP offsets

   Option 1 is much simpler and less error-prone. The loader just needs to add ~50 lines of C code to handle the new relocation types.

   Let's go with Option 1.

### 6.3 Relocation Table Format Extension

The current UDLM relocation table stores pairs of `(lot_offset, symt_offset)`.

For MOVW/MOVT relocations, we need to store:
- `instruction_offset`: byte offset of the instruction in the code section
- `symt_offset`: symbol table index (same as before)
- Plus a flag to indicate MOVW vs MOVT

Flag bits in `symt_offset`:
- Bit 31: data relocation (existing)
- Bit 30: code relocation (existing)
- Bit 29: instruction relocation (NEW — for MOVW/MOVT)

When bit 29 is set:
- `lot_offset` = byte offset of instruction in code section
- `symt_offset & 0x1FFFFFFF` = symbol table index
- The loader patches the instruction in-place with the resolved symbol address

For MOVW: extract low 16 bits
For MOVT: extract high 16 bits

Wait, how does the loader know whether it's MOVW or MOVT?

Option A: A separate flag bit for each:
- Bit 29: MOVW_ABS_NC
- Bit 28: MOVT_ABS

Then the loader:
```c
if (symt_offset & (1 << 29)) {
    // MOVW_ABS_NC
    uint32_t *p_instr = (uint32_t*)(get_code_pointer(p_mod) + lot_offset);
    uint32_t addr = /* resolved symbol address */;
    *p_instr = patch_movw(*p_instr, addr & 0xFFFF);
} else if (symt_offset & (1 << 28)) {
    // MOVT_ABS
    uint32_t *p_instr = (uint32_t*)(get_code_pointer(p_mod) + lot_offset);
    uint32_t addr = /* resolved symbol address */;
    *p_instr = patch_movt(*p_instr, (addr >> 16) & 0xFFFF);
}
```

This is clean. We use bits 29 and 28 (two new flags), keeping 30 and 31 as-is.

Wait, but the current code already uses bit 30 and 31. Are bits 28 and 29 free?

Looking at `udynlink.c`:
```c
if(symt_offset & (1 << 31)) { ... }      // data relocation
if(symt_offset & (1 << 30)) { ... }      // code relocation
```

So bits 30 and 31 are used. Bits 28 and 29 are free. Perfect.

But wait — for data relocations, `lot_offset` is used as a .data word index. For code relocations (bit 30), `lot_offset` is also used as a .data word index. For instruction relocations (bits 28/29), `lot_offset` is a code section byte offset.

The current logic:
```c
uint32_t *p_rel_location = (lot_offset < p_header->num_lot) ? p_lot + lot_offset : p_data + lot_offset - p_header->num_lot;
```

This won't work for instruction relocations because `lot_offset` isn't a LOT or data index — it's a code offset.

So the check order matters:
```c
if (symt_offset & (1 << 29)) {
    // MOVW — patch instruction at lot_offset in code
} else if (symt_offset & (1 << 28)) {
    // MOVT — patch instruction at lot_offset in code
} else if (symt_offset & (1 << 31)) {
    // Data relocation
} else if (symt_offset & (1 << 30)) {
    // Code relocation
} else {
    // Regular LOT relocation
}
```

This works and maintains backward compatibility.

### 6.4 Symbol Address Resolution for Patching

For MOVW/MOVT, the loader needs the absolute runtime address of the symbol.

For local/exported symbols:
```c
udynlink_sym_t sym;
get_sym_at(p_header, sym_index, &sym);
offset_sym(p_mod, &sym);  // Apply code/data base offset
// sym.val now contains the absolute runtime address
```

For extern symbols:
```c
uint32_t sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
if (sym_addr == 0) { ... try deps ... }
if (sym_addr == 0) { sym_addr = udynlink_external_resolve_symbol(sym.name); }
```
Then use `sym_addr` to patch the instruction.

### 6.5 MOVW/MOVT Decoding and Encoding

```c
// Decode imm16 from a Thumb-2 MOVW/MOVT instruction
static uint16_t decode_thumb2_imm16(uint32_t instr) {
    uint16_t hw1 = instr & 0xFFFF;
    uint16_t hw2 = (instr >> 16) & 0xFFFF;
    uint32_t i    = (hw1 >> 10) & 1;
    uint32_t imm4 = hw1 & 0xF;
    uint32_t imm3 = (hw2 >> 12) & 0x7;
    uint32_t imm8 = hw2 & 0xFF;
    return (i << 11) | (imm3 << 8) | imm8 | (imm4 << 12);
}

// Encode imm16 into a MOVW instruction (preserving Rd and other bits)
static uint32_t encode_thumb2_movw(uint32_t instr, uint16_t imm16) {
    uint32_t i    = (imm16 >> 11) & 1;
    uint32_t imm4 = (imm16 >> 12) & 0xF;
    uint32_t imm3 = (imm16 >> 8) & 0x7;
    uint32_t imm8 = imm16 & 0xFF;
    uint16_t hw1 = (instr & 0xFBF0) | (i << 10) | imm4;
    uint16_t hw2 = (instr & 0x8F00) | (imm3 << 12) | imm8;
    return (hw2 << 16) | hw1;
}

// Encode imm16 into a MOVT instruction
static uint32_t encode_thumb2_movt(uint32_t instr, uint16_t imm16) {
    uint32_t i    = (imm16 >> 11) & 1;
    uint32_t imm4 = (imm16 >> 12) & 0xF;
    uint32_t imm3 = (imm16 >> 8) & 0x7;
    uint32_t imm8 = imm16 & 0xFF;
    uint16_t hw1 = (instr & 0xFBF0) | (i << 10) | imm4;
    // MOVT hw1 is F2C0 | i | imm4 (bits 10:14 pattern differs from MOVW)
    hw1 = (instr & 0xFBF0) | (i << 10) | imm4;
    uint16_t hw2 = (instr & 0x8F00) | (imm3 << 12) | imm8;
    return (hw2 << 16) | hw1;
}
```

Note: `encode_thumb2_movw` and `encode_thumb2_movt` are identical in structure except the `hw1` base pattern differs:
- MOVW: `0xF240` base
- MOVT: `0xF2C0` base

But since we preserve the existing bits (mask `0xFBF0` keeps the opcode), the encoding function is the same.

---

## 7. Phase 3: Loader Extension (`udynlink.c`)

### 7.1 New Flag Bits

```c
// In udynlink.c relocation processing:
// Bit 31: R_ARM_ABS32 data relocation
// Bit 30: Code section base offset relocation
// Bit 29: R_ARM_THM_MOVW_ABS_NC instruction relocation (NEW)
// Bit 28: R_ARM_THM_MOVT_ABS instruction relocation (NEW)
```

### 7.2 Patched Loader Code

Insert after the existing bit 30 handler and before the symbol table lookup:

```c
// In udynlink_load_module(), relocation processing loop:

// ... existing bit 31 handler ...

// ... existing bit 30 handler ...

// NEW: MOVW_ABS_NC instruction relocation
if (symt_offset & (1u << 29)) {
    uint32_t sym_idx = symt_offset & 0x1FFFFFFF;
    if (get_sym_at(p_mod->p_header, sym_idx, &sym) == NULL) {
        res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
        goto exit;
    }
    uint32_t sym_addr;
    if (sym.type == UDYNLINK_SYM_TYPE_LOCAL || sym.type == UDYNLINK_SYM_TYPE_EXPORTED) {
        sym_addr = offset_sym(p_mod, &sym)->val;
    } else if (sym.type == UDYNLINK_SYM_TYPE_EXTERN) {
        sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
        // ... dependency lookup ...
        if (sym_addr == 0) sym_addr = udynlink_external_resolve_symbol(sym.name);
        if (sym_addr == 0) {
            res = UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
            goto exit;
        }
    } else {
        res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
        goto exit;
    }
    // Patch MOVW instruction: set low 16 bits
    uint32_t *p_instr = (uint32_t*)((uint32_t)(uintptr_t)get_code_pointer(p_mod) + lot_offset);
    uint32_t instr = *p_instr;
    uint16_t old_imm16 = decode_thumb2_imm16(instr);
    uint16_t new_imm16 = sym_addr & 0xFFFF;
    *p_instr = encode_thumb2_movw_movt(instr, new_imm16);
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Patched MOVW at offset %u: old_imm=%04X new_imm=%04X\n",
                   lot_offset, old_imm16, new_imm16);
    continue;
}

// NEW: MOVT_ABS instruction relocation
if (symt_offset & (1u << 28)) {
    // Same as above, but patch high 16 bits
    uint32_t sym_idx = symt_offset & 0x1FFFFFFF;
    if (get_sym_at(p_mod->p_header, sym_idx, &sym) == NULL) {
        res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
        goto exit;
    }
    uint32_t sym_addr;
    // ... same resolution as MOVW ...
    uint32_t *p_instr = (uint32_t*)((uint32_t)(uintptr_t)get_code_pointer(p_mod) + lot_offset);
    uint32_t instr = *p_instr;
    uint16_t old_imm16 = decode_thumb2_imm16(instr);
    uint16_t new_imm16 = (sym_addr >> 16) & 0xFFFF;
    *p_instr = encode_thumb2_movw_movt(instr, new_imm16);
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Patched MOVT at offset %u: old_imm=%04X new_imm=%04X\n",
                   lot_offset, old_imm16, new_imm16);
    continue;
}
```

### 7.3 Streaming Loader

The same logic must be applied to `udynlink_load_module_stream()`. The instruction patching is straightforward — it reads the instruction from the copied code, patches it, and writes it back.

---

## 8. Task Breakdown

### Block A: Foundation (can be done in parallel)

| # | Task | Agent | Deliverable | Notes |
|---|------|-------|-------------|-------|
| A1 | Extend `udynlink.c` with MOVW/MOVT patching | `agent` | C loader handles MOVW_ABS_NC and MOVT_ABS | ~50 lines of C |
| A2 | Add MOVW/MOVT relocation handling to `mkmodule`-like parser in Python | `agent` | Python post-processor recognizes these relocation types | Can reuse `udynlink_utils.py` |
| A3 | Create custom linker script `rust_module.ld` | `quick` | Linker script that places .text at 0, merges .rodata | Based on `code_before_data.ld` |
| A4 | Create minimal Rust test module | `quick` | `tests/test-rust-helloworld/` with Rust source | Use `#[no_mangle]`, no proc-macro yet |

### Block B: Python Post-Processing Tool

| # | Task | Agent | Deliverable | Notes |
|---|------|-------|-------------|-------|
| B1 | Create `scripts/rust2udynlink.py` skeleton | `quick` | CLI with `--target`, `--bin-name`, `--gen-c-header` | Match `mkmodule.py` interface |
| B2 | Implement ELF section/symbol/relocation reader | `agent` | Read Rust ELF, classify symbols, extract sections | Use `pyelftools` |
| B3 | Implement relocation processing | `agent` | Handle MOVW/MOVT + ABS32 + CALL/JUMP | Generate UDLM relocation table |
| B4 | Implement symbol table builder | `agent` | Build udynlink-format symbol table with type/location encoding | |
| B5 | Implement UDLM binary writer | `agent` | Output valid `.bin` | |
| B6 | Implement assembly prologue generation | `agent` | Generate prologues for exports using existing `asm_template_*.tmpl` | |

### Block C: Proc-Macro (`udynlink-module` crate)

| # | Task | Agent | Deliverable | Notes |
|---|------|-------|-------------|-------|
| C1 | Create `crates/udynlink-module/` crate skeleton | `quick` | Cargo workspace member | |
| C2 | Implement `#[udynlink_export]` proc-macro | `agent` | Renames function, emits `.udynlink.exports` metadata | |
| C3 | Implement `#[udynlink_extern]` proc-macro | `quick` | Declare external host symbols | |
| C4 | Add panic handler | `quick` | Default abort-on-panic | |

### Block D: Integration & Testing

| # | Task | Agent | Deliverable | Notes |
|---|------|-------|-------------|-------|
| D1 | Integrate Rust module with test harness | `agent` | `test_driver.py` compiles Rust module + runs in QEMU | |
| D2 | Test on MPS2-AN386 (Cortex-M4) | `agent` | Passes `-O0` and `-Os` for all 3 load modes | |
| D3 | Test multiple targets | `quick/agent` | Validate on at least M4, M3, M7 | |
| D4 | Add `just` commands for Rust module compilation | `quick` | `just rust-module source.rs`, `just rust-module-for cortex-m7 source.rs` | |

---

## 9. Risk Register (Updated with Investigation Results)

| Risk | Likelihood | Impact | Strategy |
|------|-----------|--------|----------|
| Rust `core` library generates absolute addresses that can't all be patched | Low | Blocker | Investigation showed all addresses are patchable via MOVW/MOVT |
| MOVW/MOVT instruction encoding is wrong | Medium | High | Write unit tests for decode/encode functions; compare with `objdump` output |
| MOVW/MOVT pairs are not always adjacent | Low | Medium | ELF relocation records point to each instruction individually; handle separately |
| `R_ARM_ABS32` in `.rodata` (now merged into `.text`) | Medium | Medium | Custom linker script merges `.rodata` into `.text`; loader handles ABS32 |
| rust-lld drops `.udynlink.exports` section | Low | Medium | Use `#[used]` attribute; verify with `readelf -S` |
| Performance: patching instructions at load time is slower than LOT | Low | Low | Embedded target with small modules; negligible overhead |
| `dyn Trait` / vtables non-relocatable | High | Medium | Document restriction. Static generics work fine. |
| External function calls don't resolve correctly | Medium | High | Ensure `R_ARM_THM_CALL` with `SHN_UNDEF` symbols is handled; test with printf |

---

## 10. Open Questions

1. **Should we also support `R_ARM_MOVW_ABS_NC` / `R_ARM_MOVT_ABS` (non-Thumb)?** The `thumbv7em` target generates Thumb-2 instructions. ARM instruction variants are unlikely.
2. **How to handle `R_ARM_ABS32` in `.rodata`?** With our custom linker script, `.rodata` is merged into `.text`. ABS32 relocations in `.text` for string literal pointers need to be handled. But since `.text` is in the code section, maybe we should keep `.rodata` separate and place it after `.text` in the binary.
3. **Should we use the LOT at all, or just patch instructions directly?** With the `static` model, there's no LOT indirection. We could set `num_lot = 0` and handle everything via instruction patching and data ABS32. This simplifies the module format. But keeping the LOT allows future flexibility.
   - **Decision:** Start with `num_lot = 0` for Rust modules. Simpler post-processor, simpler loader interaction. This means the relocation table only contains instruction relocations (bits 28/29) and data relocations (bit 31).

---

*End of Rust Module sub-plan.*
