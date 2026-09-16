# How udynlink Works

## Table of Contents

1. [Overview: Position-Independent Code and Data](#overview-position-independent-code-and-data)
2. [The Linker Offset Table (LOT)](#the-linker-offset-table-lot)
3. [Function Wrapping (Assembly Prologue)](#function-wrapping-assembly-prologue)
4. [The mkmodule Pipeline](#the-mkmodule-pipeline)
5. [Module Binary Format](#module-binary-format)
6. [Relocation Processing](#relocation-processing)
7. [Three Load Modes](#three-load-modes)
8. [Multi-Region Memory Placement](#multi-region-memory-placement)
9. [Symbol Resolution at Load Time](#symbol-resolution-at-load-time)
10. [ABI Versioning and Architecture Tags](#abi-versioning-and-architecture-tags)
11. [Cross-Module Calls via Runtime Thunks](#cross-module-calls-via-runtime-thunks)
12. [Standalone Call Thunks (`udynlink_thunk`)](#standalone-call-thunks-udynlink_thunk)
13. [Non-Contiguous Image Loading](#non-contiguous-image-loading)
---

## Overview: Position-Independent Code and Data

### The Problem

Standard ARM code uses PC-relative addressing to load the addresses of global variables. At link time, the linker fills in the absolute address of each variable directly into the code section. This works for statically linked firmware, but breaks for code that must be loaded at runtime: the variable's address is baked into the `.text` section, and if the module is loaded at a different location, the address is wrong.

Consider this simple C code:

```c
volatile int i = 1;

int main() {
    return i;
}
```

Compiled with standard options (`arm-none-eabi-gcc -O0 -c -mcpu=cortex-m4 -mthumb`), the generated assembly is:

```asm
00000000 <main>:
   0: b480        push  {r7}
   2: af00        add   r7, sp, #0
   4: 4b03        ldr   r3, [pc, #12]   ; (14 <main+0x14>)
   6: 681b        ldr   r3, [r3, #0]
   8: 4618        mov   r0, r3
   a: 46bd        mov   sp, r7
   c: f85d 7b04   ldr.w r7, [sp], #4
  10: 4770        bx    lr
  12: bf00        nop
  14: 00000000    .word 0x00000000
```

At offset `0x14`, the linker will place the absolute address of `i`. If the module is relocated in RAM, this word in `.text` must be patched. Patching `.text` is feasible when it resides in RAM, but difficult or impossible when executing from flash (XIP).

### The Solution

udynlink compiles modules with GCC flags that generate position-independent code (PIC) and position-independent data (PID):

```bash
-fPIE -msingle-pic-base -mno-pic-data-is-text-relative
-ffunction-sections -fdata-sections
```

Recompiling the same source with these flags yields:

```asm
00000000 <main>:
   0: b480        push  {r7}
   2: af00        add   r7, sp, #0
   4: 4b04        ldr   r3, [pc, #16]   ; (18 <main+0x18>)
   6: f859 3003   ldr.w r3, [r9, r3]
   a: 681b        ldr   r3, [r3, #0]
   c: 4618        mov   r0, r3
   e: 46bd        mov   sp, r7
  10: f85d 7b04   ldr.w r7, [sp], #4
  14: 4770        bx    lr
  16: bf00        nop
  18: 00000000    .word 0x00000000
```

The key difference is at offset `0x6`: instead of loading the variable directly, the code loads `r3` from `[r9, r3]`. The PC-relative word at `0x18` now contains an **index into the Linker Offset Table (LOT)**, not the absolute address of `i`. The LOT is a table of resolved addresses whose base is held in `r9`. By changing the values in the LOT at runtime, the module's data can be relocated without modifying a single byte of `.text`.

---

## The Linker Offset Table (LOT)

### What the LOT Is

The LOT is an array of 32-bit resolved addresses placed at the beginning of the module's RAM region. Each entry corresponds to a symbol that the module references. When module code needs the address of a global variable or function, it indexes into the LOT via `r9` and then dereferences the resulting pointer.

### How r9 Points to the LOT

The ARM EABI reserves `r9` as a platform-specific register. udynlink uses `r9` as the PIC base register (`SB`). The compiler is told to treat `r9` this way via `-msingle-pic-base`. Every data access in the module compiles to a `r9`-relative load:

```asm
ldr r3, [pc, #offset]     ; load LOT index
ldr r3, [r9, r3]          ; load resolved address from LOT
ldr r3, [r3, #0]          ; load actual data
```

### The LOT Base Address Convention

In ABI v3.0, the host manages `r9` directly. There is no fixed memory address, no global word, and no memory load inside the prologue. The assembly wrapper only saves the caller's `r9` on the stack and restores it after the call. The host is responsible for setting `r9` to the module's RAM base before every call.

### The Host's Responsibility

Before calling any exported function from a loaded module, the host firmware **must** set `r9` to the module's RAM base address:

```c
UDYNLINK_PREPARE_CALL(p_mod);
```

This macro expands to inline assembly that moves `p_mod->ram_base` into `r9`. The module's wrapper prologue saves the caller's original `r9` before branching to the real function, and restores it on return. Because `r9` is already correct when the real function body starts, all subsequent PIC data accesses work automatically.

For convenience, the C call layer (`udynlink/udynlink_call.h`) provides macros that handle `r9` save/restore automatically:

```c
udynlink_func_t h;
udynlink_resolve_func(p_mod, "run", &h);
int r = UDYNLINK_CALL(&h, int, (42));
```

`UDYNLINK_CALL` saves the caller's `r9`, invokes `UDYNLINK_PREPARE_CALL`, calls the function, and restores `r9` before returning.

For C++ hosts, `udynlink.hpp` provides typed `Func<Sig>` handles and an RAII `Context` that manages `r9` over an entire block — see [C++ API](integrating-as-host.md#c-api-udynlinkhpp) in the host integration guide.

### Why Remove the Fixed-Address Approach

Earlier versions of udynlink (and the original project) relied on either a callback at `0x1c` or a fixed RAM word at `0x20000000` (`UDYNLINK_LOT_BASE_ADDR`). Both approaches had drawbacks:
- The `0x1c` callback required a writable vector-table location and a function call on every module entry.
- The fixed-address approach required a dedicated RAM word, created a single point of contention for reentrant and cross-module calls, and still needed host setup before every call.

ABI v3.0 eliminates the memory word entirely. The host sets `r9` directly via inline assembly. This is faster (a single register move), safer (no shared global state), and simpler conceptually.

---

## Function Wrapping (Assembly Prologue)

### The Wrapper Concept

Every exported (non-static) function in a module receives a small assembly **wrapper**. The wrapper is the actual global symbol the host calls. Its job is to:

1. Save the caller's `r9` and `lr`.
2. Call the real function.
3. Restore the caller's `r9` and return.

This ensures that `r9` is preserved across the module boundary. The host must set `r9` to the correct module base via `UDYNLINK_PREPARE_CALL()` before calling the wrapper.

### Generated Assembly (armv7-m / armv8-m)

The `asm_template_armv7m.tmpl` (used for Cortex-M3, M4, M4F, M7) and `asm_template_armv8m.tmpl` (used for Cortex-M33, M55, M85) generate:

```asm
    .thumb_func
    .section .text_nogc, "x"
    .globl  my_func
    .type   my_func, %function
    .extern __abcdef123_my_func
    .type   __abcdef123_my_func, %function
my_func:
    push    {r9, lr}
    bl      __abcdef123_my_func
    pop     {r9, pc}

    .size   my_func, . - my_func
```

Instruction-by-instruction breakdown:

| Instruction | Purpose |
|-------------|---------|
| `push {r9, lr}` | Save caller's `r9` and link register. |
| `bl __abcdef123_my_func` | Branch to the real (renamed) function body. |
| `pop {r9, pc}` | Restore caller's `r9` and return. |

The prologue trusts that the host has already set `r9` to the correct module base via `UDYNLINK_PREPARE_CALL()`. It does not touch memory or literal pools.

### Cortex-M0 / M0+ Prologue (armv6-m)

Cortex-M0 lacks `ldm`/`stm` of arbitrary register sets and the `ldr Rd, [Rn, Rm]` addressing mode used by the compiler for LOT accesses. The wrapper saves the caller's `r9` via `r2` because `push`/`pop` on v6-m only supports the low registers:

```asm
    .thumb_func
    .section .text_nogc, "x"
    .globl  my_func
my_func:
    mov     r2, r9
    push    {r1, r2, lr}
    bl      __abcdef123_my_func
    ldr     r2, [sp, #4]
    mov     r9, r2
    pop     {r1, r2, pc}
```

On M0, `r2` is used as scratch instead of `r1` because the `push`/`pop` instructions are more restricted. The prologue does not load `r9` from memory; it expects the host to have set `r9` before the call.

### The `--public-symbols` Optimization

By default, `mkmodule` wraps **all** non-static functions. For modules with many exported functions but only a few that the host actually calls, this wastes flash/RAM in the wrapper code. The `--public-symbols func1,func2` flag tells `mkmodule` to wrap only the named functions (plus `__init_array` for C++). All other global functions remain unwrapped and are invisible to the host, reducing image size.

### Interaction with `--gc-sections`

The linker script places wrappers in a special section `.text_nogc`:

```ld
KEEP(*(.text_nogc))
```

This prevents `--gc-sections` from discarding the wrappers even if no other code in the module references them. The real function bodies (which are renamed to `__md5prefix__original_name`) are referenced by the wrapper's `bl` instruction, so they are retained naturally. Unused functions that are *not* in the public-symbols list are still garbage-collected because they have no wrapper pointing to them.

---

## The mkmodule Pipeline

The `scripts/mkmodule` script transforms C/C++ source files into a loadable binary module through four distinct stages.

### Step 1: Compile

Each source file is compiled to an object file with position-independent flags:

```bash
arm-none-eabi-gcc -fPIE -msingle-pic-base -mno-pic-data-is-text-relative \
    -ffunction-sections -fdata-sections -fomit-frame-pointer \
    -mcpu=<cpu> -mthumb -Os -c source.c -o source.o
```

For C++ sources (`.cpp` / `.cxx`), the toolchain automatically appends:

```bash
-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics
```

and also compiles `cpp_init_fini.c` into the object list. This file provides `__init_array`, which iterates over `.preinit_array` and `.init_array` to run global constructors.

**Function wrapping happens in this step:**

1. `mkmodule` discovers all public functions in the object file via `get_public_functions_in_object`.
2. Each public function is renamed to a mangled name (`__<md5prefix>__<original>`) using `objcopy --redefine-sym`. After linking, the renamed bodies are made ELF-local (`objcopy -L`, across **all** source files), so they never appear as exported symbols in the module's symbol table — the wrapper is the only visible export.
3. An assembly prologue file is generated from the Jinja2 template and assembled into a second object file.
4. Both object files (original + prologue) are passed to the linker.

If `--public-symbols` is given, only the listed functions are renamed and wrapped.

### Step 2: Link

Object files are linked with a custom linker script (`scripts/code_before_data.ld`) and special flags:

```bash
arm-none-eabi-gcc -mcpu=<cpu> -mthumb -T code_before_data.ld \
    -nostartfiles -nodefaultlibs -nostdlib \
    -Wl,--unresolved-symbols=ignore-in-object-files \
    -Wl,--emit-relocs -Wl,--gc-sections \
    -Wl,-e,0 -o module.elf *.o
```

Key linker script features:

```ld
MEMORY {
  all (RWX) : ORIGIN = 0x00000000, LENGTH = 0xFFFFFFFF
}
SECTIONS {
  .text : {
    KEEP(*(.text_nogc))
    *(.text) *(.text*) *(.rodata) *(.rodata*)
  } > all
  .data : {
    *(.data) *(.data*)
    KEEP(*(.init))
    __preinit_array_start = .; KEEP(*(.preinit_array))
    __init_array_start = .;    KEEP(*(.init_array))
  } > all
  .bss  : { *(.bss) *(.bss*) } > all
  .got  : { *(.got*) }
}
```

- **Origin at 0**: `.text` starts at address 0. Offsets within the module are therefore relative to the code base.
- **`.text` before `.data`**: The linker script enforces this ordering. `mkmodule` later verifies it when reading the ELF.
- **`--unresolved-symbols=ignore-in-object-files`**: Symbols the module needs from the host (e.g., `printf`) remain undefined in the ELF. They become "foreign" symbols in the module image.
- **`--gc-sections`**: Dead code elimination removes unused functions and data, reducing image size. Wrappers are protected by `.text_nogc` and `KEEP`.
- **`--emit-relocs`**: Preserves relocation information in the ELF so `mkmodule` can read it in Step 3.

### Step 3: Process (ELF Introspection)

`mkmodule` opens the linked ELF and:

1. **Reads sections**: Extracts `.text`, `.data`, and `.bss`. Verifies that `.text` starts at 0, `.data` follows `.text`, and `.bss` follows `.data`. All sizes must be multiples of 4.
2. **Classifies symbols** using `pyelftools`:
   - `STB_GLOBAL` + defined section = **exported**
   - `STB_GLOBAL` + `SHN_UNDEF` = **foreign** (unresolved)
   - `STB_LOCAL` = **local**
3. **Reads relocations**: Iterates all `RelocationSection` entries. For each relocation:
   - `R_ARM_THM_CALL` / `R_ARM_THM_JUMP24` — PC-relative, ignored.
   - `R_ARM_GOT_BREL` — LOT relocation (local or foreign).
   - `R_ARM_ABS32` / `R_ARM_TARGET1` — Data section relocation.
4. **Builds LOT mapping**: Deduplicates symbols so only the first relocation to each unique symbol gets a LOT slot. The LOT index for each symbol is `index * 4` (bytes).
5. **Patches code**: For every `R_ARM_GOT_BREL` relocation, patches the `.text` word at the relocation offset to contain the LOT index (in bytes) instead of the original addend.

### Step 4: Generate Binary Image

The final binary image is assembled as a byte array:

1. Write the 32-byte header (see [Module Binary Format](#module-binary-format)).
2. Write relocation table entries.
3. Write the symbol table. Symbol names and — for sectioned images — section names all live in the same string pool.
4. Write the section table (sectioned images only).
5. Pad to 4-byte alignment.
6. Append the section payloads in ascending VA order, skipping BSS sections (which have no payload). For an untagged module this is exactly `.text` then `.data`.

The result is a `.bin` file that can be embedded in host firmware as a byte array, stored in flash, or loaded from external storage.

---

## Module Binary Format

The module binary is a contiguous blob of bytes with the following layout. The
section table is present only when the header's `UDYNLINK_HDR_FLAG_SECTIONS`
bit is set (see [Multi-Region Memory Placement](#multi-region-memory-placement)):

```
+---------------------------------------------------+
| Header                    | 32 bytes              |
+---------------------------------------------------+
| Relocation Table          | num_rels * 8 bytes    |
+---------------------------------------------------+
| Symbol Table              | symt_size bytes       |
+---------------------------------------------------+
| Section Table (optional)  | n_sections * 24        |
+---------------------------------------------------+
| Padding to 4-byte align   | 0-3 bytes             |
+---------------------------------------------------+
| Section payloads          | ascending VA order,   |
|                           | BSS skipped           |
+---------------------------------------------------+
```

For an untagged module (no flag) the payload region is exactly `.text`
(`code_size` bytes) followed by `.data` (`data_size` bytes), as always.

### Header Fields (`udynlink_module_header_t`)

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0x00 | `sign` | 4 | Signature: `'U' 'D' 'L' 'M'` (little-endian `0x4D4C4455` or `(((uint32_t)'M'<<24)|...|'U')` depending on host endianness; the loader checks against `(((uint32_t)'M'<<24)|((uint32_t)'L'<<16)|((uint32_t)'D'<<8)|(uint32_t)'U')`). |
| 0x04 | `mod_version` | 2 | Module ABI version. Encoded as `(major << 8) \| minor`. |
| 0x06 | `udynlink_version` | 2 | Minimum loader ABI version required. Same encoding. |
| 0x08 | `arch_tag` | 2 | Target architecture + float ABI tag (see [ABI Versioning](#abi-versioning-and-architecture-tags)). |
| 0x0A | `num_lot` | 2 | Number of LOT entries (each is one 32-bit word). |
| 0x0C | `num_rels` | 2 | Total number of relocation entries in the relocation table. |
| 0x0E | `flags` | 2 | Flag bits. Bit 0 = `UDYNLINK_HDR_FLAG_SECTIONS` (the image carries a section table); bits 7:1 = the section count (1..63, meaningful only when bit 0 is set); bits 15:8 reserved, must be 0. The count lives in the header so image-size computations never depend on reading the table body. Old images (v1.0–v3.0) always wrote 0 here, which the loader accepts. |
| 0x10 | `symt_size` | 4 | Size of the symbol table in bytes. |
| 0x14 | `code_size` | 4 | Size of `.text` section in bytes. |
| 0x18 | `data_size` | 4 | Size of `.data` section in bytes. |
| 0x1C | `bss_size` | 4 | Size of `.bss` section in bytes. |

**ABI v1.0 backward compatibility**: For modules compiled with `--udynlink-version 1.0`, the header is also 32 bytes (same layout, with `flags` at 0x0E). The loader accepts v1.0 modules as long as `udynlink_version <= UDYNLINK_LOADER_ABI_VERSION`.

### Section Table (sectioned images)

Present only when `UDYNLINK_HDR_FLAG_SECTIONS` is set. It is located at
`sectab_offset = align4(symtab_offset + symt_size)` — contiguous with the
header's metadata block — and the section payloads start at
`code_offset = align4(sectab_offset + sectab_size)`. (Untagged images keep
`code_offset = align4(symtab_offset + symt_size)`.)

The table is exactly `num_sections` 24-byte entries — no leading count word;
the count comes from header `flags` bits 7:1, so image size is computable
from the header alone. `sectab_size = num_sections * 24`:

```c
uint32_t name_off;   /* byte offset of the NUL-terminated section name in the
                        symbol-table string pool; 0 = unnamed */
uint32_t va;         /* link-time VA of the section start */
uint32_t size;       /* bytes, multiple of 4 */
uint32_t align;      /* bytes, power of two, >= 4 */
uint32_t class;      /* 0 = CODE, 1 = DATA, 2 = BSS */
uint32_t flags;      /* hint bits, see "Hint flags" below; bit 31 = MAIN */
```

Properties the loader relies on:

- Entries are **sorted by `va` ascending**.
- The three main sections (`.text`, `.data`, `.bss`) carry the three lowest
  VAs, so they are indices 0/1/2 in both tagged and untagged modules.
- Payloads are concatenated in ascending VA order, skipping BSS (BSS has no
  payload in the image). For the default sections this keeps the historical
  `[text][data]` layout and keeps `data_offset == code_offset + code_size`.
- The count is 1..63, taken from header `flags` bits 7:1. An invalid
  combination — bit 0 clear with a nonzero count, or a count above 63 —
  is rejected with `UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE`.
- The loader validates the table: entries sorted by `va`, non-overlapping
  VA ranges, `size` a multiple of 4, `align` a power of two ≥ 4, a valid
  class, and `name_off` inside the string pool. Violations fail the load
  with `UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE`.
- Section names are interned in the symbol-table string pool (no second
  string table); `symt_size` covers them.

### Symbol Table Encoding

The symbol table begins with a 4-byte word containing the number of symbol entries, followed by 8 bytes per entry (two 32-bit words):

- **Word 0**: `name_offset | (type_data << 28)`
  - Bits `[27:0]` — byte offset from the start of the symbol table to the NUL-terminated name string.
  - Bit `30` — `1` if the symbol is in the code section, `0` if in data.
  - Bits `[29:28]` — visibility: `0` local, `1` exported, `2` external, `3` module name.
  - Internal symbols have `name_offset = 0` and no name string in the table.
- **Word 1**: `value` — the symbol's address (relative to its section base for local/exported symbols; undefined for external symbols). For sectioned images the value is the symbol's flat ELF link VA (same address space as the section table's `va` fields); untagged modules keep the historical convention (`val` relative to the code base, or arena-relative for data/bss).

The first entry (index 0) is always the **module name entry** (`type_data = 3`).

### Relocation Table Entries

Each relocation is an 8-byte pair:

```c
uint32_t lot_offset;   // index into LOT or data section (in words, not bytes)
uint32_t symt_offset;  // symbol table index, or special flag bits
```

- If `symt_offset` has **bit 31** set (`0x80000000`): this is an `R_ARM_ABS32` data relocation. The lower 31 bits contain the original addend/value.
- If `symt_offset` has **bit 30** set (`0x40000000`): this is a `.text` base relocation. The lower 30 bits contain the original addend.
- Otherwise: `symt_offset` is an index into the symbol table, and `lot_offset` tells the loader which LOT slot (or data word) to patch.

If `lot_offset < num_lot`, the relocation targets a LOT entry. If `lot_offset >= num_lot`, the relocation targets a word in `.data` at word offset `(lot_offset - num_lot)` — equivalently, the word whose link-time VA is `code_size + 4 * (lot_offset - num_lot)`.

---

## Relocation Processing

When `udynlink_load_module` processes a module, it iterates over all `(lot_offset, symt_offset)` pairs in the relocation table and applies them.

### `R_ARM_GOT_BREL` -> LOT Slot Allocation

The most common relocation is `R_ARM_GOT_BREL` (generated by the compiler for data accesses). At build time, `mkmodule` allocates one LOT slot per unique symbol and patches the `.text` word to contain the LOT index (in bytes). At load time, the loader resolves the symbol and writes its final address into the LOT:

```c
uint32_t *p_lot = (uint32_t *)ram_addr;
p_lot[lot_offset] = resolved_symbol_address;
```

### `R_ARM_ABS32` / `R_ARM_TARGET1` -> Data Section Relocation

These relocations occur when code contains absolute pointers to data (for example, arrays of function pointers or pointer tables). The `symt_offset` has bit 31 set. The loader patches the `.data` word:

```c
uint32_t *p_data = get_data_pointer(p_mod);
// symt_offset & 0x7FFFFFFF is the original addend
p_data[lot_offset - num_lot] += (uint32_t)p_data - (symt_offset & 0x7FFFFFFF);
```

This converts the linker-time absolute address into a runtime absolute address by adding the delta between the actual `.data` base and the link-time `.data` base (which was 0 in the linker script).

### `.text` Base Relocation (bit 30)

When the module contains absolute pointers to its own code (e.g., a jump table), `symt_offset` has bit 30 set. The loader patches the `.data` word:

```c
p_data[lot_offset - num_lot] = (uint32_t)get_text_pointer(p_mod) + *p_data_word;
```

Sectioned images resolve the same relocation through the VA map instead: the
patched word holds a link-time VA, and the loader writes
`runtime(va) = base[idx(va)] + (va - sec[idx].va)` for the section containing
it (see [Multi-Region Memory Placement](#multi-region-memory-placement)). Both
base-additive forms (bit 31 and bit 30) collapse into that one operation;
untagged modules keep the two-form path above unchanged.

### Deduplication

If a module references the same symbol multiple times (for example, an array where every element points to the same function), `mkmodule` ensures only the **first** relocation to that symbol consumes a LOT slot. Subsequent relocations reuse the same `lot_offset`. This keeps the LOT small.

### PC-Relative Relocations (`R_ARM_THM_CALL`, `R_ARM_THM_JUMP24`)

Branch and call instructions in Thumb-2 are PC-relative. They do not need runtime relocation because the offset from the caller to the callee is the same regardless of where the module is loaded. `mkmodule` ignores these relocations, and the loader does not process them.

### In-Place Relocation (Rebasing a Loaded Module)

`udynlink_relocate_module()` moves an already-loaded module's contiguous RAM region to a new buffer in the **same load mode**, preserving runtime state (mutated `.data`/`.bss`, already-resolved extern slots, weak overrides). It is the state-preserving alternative to unload+reload (which resets state and re-runs all relocations).

After copying the whole RAM block to the destination, the loader walks the same relocation table the load-time pass uses, but instead of recomputing each slot from the original formula it **adds a move delta** to every module-internal absolute pointer. Two deltas are involved:

- **`data_delta`** = `dest - old_ram_base`. The LOT/.data/.bss block moves with the region base, so every internal pointer into that block (LOT entries for data symbols, `R_ARM_ABS32` data-section pointers, `.data`-resident weak defaults) shifts by `data_delta`.
- **`code_delta`** = `data_delta` for `COPY_ALL`/`COPY_TEXT_DATA` (the code lives inside the moved block), and `0` for `XIP` (the code stays in flash). Internal pointers into code (LOT entries for code symbols, bit-30 code-base pointers, `.data`-resident function pointers) shift by `code_delta`.

Slots that are **not** rebased: EXTERN slots (host-absolute addresses) and weak slots that the host overrode at load time (host-absolute). For a weak slot, the loader compares the current value against the module's own old code/data base + symbol offset; a match means the module's own default still lives there (rebase it), otherwise the host override is preserved (leave it). The additive `R_ARM_ABS32` and bit-30 slots are absolutes after the one-time load transform, so a single `+= delta` rebases them.

Each delta applies exactly once per call, so successive relocates compose (each call shifts by its own delta). The invalidation contract is documented in the [API reference](api-reference.md#udynlink_relocate_module): cached symbol addresses and cross-module thunks/deps gateways embed the old `ram_base` and must be rebuilt after a relocate.

`udynlink_relocate_module()` moves only the module's **main block**. Tagged
(non-main) sections are absolute — the host placed them and the loader only
records their bases — so a plain block move stays valid for sectioned modules
too. To also move tagged sections, use `udynlink_relocate_module_sections()`:
the host first copies each section's payload to its new base (the loader does
not `memcpy` host-owned blocks) and passes a list of `{index, new_base}`
moves; the loader then re-applies, re-derived from the image's relocation
table, every module-internal relocation whose target or source lies in a
moved section, using per-section deltas (`0` for sections that did not
move). A moved section's contents must be byte-identical to its pre-move
contents — the relocations are re-derived from the image, not read back from
the moved data.

---

## Three Load Modes

udynlink supports three ways of bringing a module into memory, controlled by the `udynlink_load_mode_t` argument to `udynlink_load_module`.

### COPY_ALL

The entire module image (header, relocation table, symbol table, `.text`, and `.data`) is copied into RAM.

**Post-load RAM layout:**

```
+---------------------------------------------------+
| LOT entries               | num_lot * 4 bytes       |
+---------------------------------------------------+
| Non-main section bases   | sectioned images only;  |
+---------------------------------------------------+
|                          | 0 bytes when untagged   |
+---------------------------------------------------+
| Header + Relocs + Symtab + padding                  |
| (copied verbatim from image)                        |
+---------------------------------------------------+
| Code (.text)              | code_size bytes       |
+---------------------------------------------------+
| Data (.data)              | data_size bytes       |
+---------------------------------------------------+
| BSS (.bss)                | bss_size bytes (zeroed)|
+---------------------------------------------------+
```

This mode is the safest: after loading, the original image can be discarded. It uses the most RAM.

### COPY_TEXT_DATA

Only `.text` and `.data` are copied into RAM. The header, relocation table, and symbol table remain at the original `base_addr` (which must remain accessible for symbol lookups and future unloading).

**Post-load RAM layout:**

```
+---------------------------------------------------+
| LOT entries               | num_lot * 4 bytes       |
+---------------------------------------------------+
| Non-main section bases   | sectioned images only;  |
+---------------------------------------------------+
|                          | 0 bytes when untagged   |
+---------------------------------------------------+
| Code (.text)              | code_size bytes       |
+---------------------------------------------------+
| Data (.data)              | data_size bytes       |
+---------------------------------------------------+
| BSS (.bss)                | bss_size bytes (zeroed)|
+---------------------------------------------------+
```

`p_mod->p_header` still points to the original flash-resident header. This mode saves RAM by not copying metadata.

### XIP (Execute In Place)

Only `.data` is copied into RAM. `.text` remains in the original image (typically flash). The header also remains at `base_addr`.

**Post-load RAM layout:**

```
+---------------------------------------------------+
| LOT entries               | num_lot * 4 bytes       |
+---------------------------------------------------+
| Non-main section bases   | sectioned images only;  |
+---------------------------------------------------+
|                          | 0 bytes when untagged   |
+---------------------------------------------------+
| Data (.data)              | data_size bytes       |
+---------------------------------------------------+
| BSS (.bss)                | bss_size bytes (zeroed)|
+---------------------------------------------------+
```

**Caveat**: "XIP" does **not** mean "zero RAM required". Even if `.data` and `.bss` are empty, the module still needs RAM for the LOT and for applying relocations. Truly RAM-less modules are rare.

### RAM Size Calculation

The loader computes required RAM as:

```c
uint32_t ram = num_lot * sizeof(uint32_t)   /* LOT */
             + nonmain_bases_size           /* 0 bytes for untagged modules */
             + data_size + bss_size;
if (mode == COPY_TEXT_DATA)   ram += code_size;
if (mode == COPY_ALL)    ram += header_offset + code_size;
/* Sectioned modules: + per-section alignment padding inside the block.
   ram_size covers only the MAIN sections; tagged sections are allocated
   separately through udynlink_external_malloc. */
```

Where `header_offset = sizeof(header) + num_rels*8 + symt_size + section-table (if present) + padding`.

For **untagged** modules `nonmain_bases_size` is 0 and there is no alignment
work: every layout, size and offset above is bit-identical to the historical
loader. For **sectioned** modules, the base of each MAIN section inside the
block is aligned up to its declared `align` (the padding is included in
`ram_size`), and the main block's alignment requirement `main_align` — the
maximum `align` over MAIN sections, 4 for untagged modules — is passed to
`udynlink_external_malloc`. See
[Multi-Region Memory Placement](#multi-region-memory-placement).

---

## Multi-Region Memory Placement

By default a module is one contiguous RAM block: the LOT, the default
`.text`/`.data`/`.bss`, and (in `COPY_ALL`) the image metadata all live in a
single allocation. A sectioned image (`UDYNLINK_HDR_FLAG_SECTIONS`) may
additionally tag **named sections** that the host places in dedicated memory
regions — DTCM for latency-critical data, non-cacheable SRAM for DMA
buffers, CCM RAM, a second code bank, and so on. The feature is additive: a
module built without `--section` produces a byte-identical image and takes
the exact loader path described above.

### The VA Map

Symbols and relocations still encode link-time values. What changes for
sectioned images is how a link-time VA maps to a runtime address:

```
runtime(va) = sec_base[idx(va)] + (va - sec[idx].va)
```

where `idx(va)` is the section whose VA range `[va_start, va_start + size)`
contains `va` (the table is sorted by `va`, so the lookup is a small linear
or binary search). A sectioned image uses the flat ELF link-address space
throughout: symbol values and section-table `va` fields live in the same
space, with `.text` at 0, `.data` at `code_size`, `.bss` at
`code_size + data_size`, and each tagged region `k` linked at
`0x02000000 * (k + 1)` — the wide spacing turns an accidental cross-region
direct `bl` into a loud link-time error.

Untagged modules do **not** use this map. Their data/bss symbol values are
arena-relative (so their VA ranges overlap), and the loader keeps the
historical code-base / data-base resolution code verbatim. The three-entry
section view reported for untagged modules (below) is a reporting view only,
not a resolution mechanism. Extern and weak symbol handling is unchanged for
both kinds of module.

### Main Block vs Tagged Sections

MAIN-flagged sections (always the default `.text`/`.data`/`.bss`) live in
the module's main RAM block, with the LOT at offset 0 (`r9 = p_ram`) and the
non-main section base array immediately after it (see the load-mode layouts
above). Tagged sections are allocated one call each through the extended
allocator callbacks — `udynlink_external_malloc(size, name, align, flags)` —
documented in the [host guide](integrating-as-host.md#implementing-the-external-callbacks).
The host owns placement policy: which pool answers the call, whether to
refuse a placement, how `free` routes back to the right pool. The loader
never falls back on its own, which keeps `udynlink_compute_ram_size()` a
valid pre-load contract for the main block.

Tagged CODE sections are always copied to their resolved address, in every
load mode including XIP (the host asked for that memory explicitly); the
default `.text` still executes in place under XIP. Tagged DATA/BSS payloads
are copied/zeroed at their resolved bases.

### Alignment

The main block's alignment requirement is `main_align` — the maximum `align`
over MAIN sections — and it is passed to `udynlink_external_malloc`
(untagged modules: 4, exactly as before). Inside the block, each MAIN
section base is aligned up to its declared `align` (padding included in
`ram_size`), so a sectioned module gets honored `aligned(N)` semantics for
its default data too. Untagged modules keep today's 4-byte-only guarantee:
`aligned(N)` data in the default `.data`/`.bss` is **not** N-aligned at
runtime, and `mkmodule` warns at build time when packed data requires more.

Tagged sections are aligned by the host (it owns the pool) and validated by
the loader: a base that does not meet the declared `align` fails the load
with `UDYNLINK_ERR_LOAD_SECTION_UNALIGNED`; a failed placement (`NULL` from
the allocator) fails with `UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED`, after
freeing everything already allocated.

### Hint Flags

Each section-table entry carries a 32-bit hint word:

| Bits | Owner | Meaning |
|---|---|---|
| 7:0 | udynlink (v1 vocabulary) | `0x01` NOCACHE (host should map non-cacheable — DMA coherency), `0x02` DMA (must be reachable by the DMA controller), `0x04` SHARED (may be shared with other modules/host code); 5 bits free |
| 15:8 | host | opaque pass-through, never interpreted by the loader |
| 23:16 | udynlink, reserved | must be 0 in v1 |
| 31:24 | udynlink, internal | bit 31 `MAIN` (section lives in the module's main RAM block); never passed to the host |

Hints are the host's to interpret or ignore — the loader validates nothing
about them. Class (CODE/DATA/BSS) is a separate field, not a flag bit.

### Introspection and Movement

`udynlink_get_section_count()`, `udynlink_get_section_info()` and
`udynlink_get_section_base()` expose the section table and the resolved
runtime bases. For an untagged module they report the three implicit main
sections with today's exact bases. `udynlink_relocate_module()` keeps moving
only the main block — tagged sections are absolute and unaffected — and
`udynlink_relocate_module_sections()` additionally moves host-copied tagged
sections (see
[In-Place Relocation](#in-place-relocation-rebasing-a-loaded-module)).

---

## Symbol Resolution at Load Time

When the loader encounters an `UDYNLINK_SYM_TYPE_EXTERN` symbol during relocation processing, it must resolve the symbol to an actual address before writing it into the LOT or data section.

### Foreign Symbol Resolution

The loader calls the host-provided callback:

```c
uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod, const char *name);
```

The host firmware looks up `name` in its own symbol table and returns the address, or `0` if the symbol is not found. If the symbol cannot be resolved, loading fails with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

The host may also return `UDYNLINK_SYM_DEFERRED` (the sentinel value `(uint32_t)1`) to defer the symbol. When this happens, the loader writes `0` to the relocation slot and **continues loading**. This is useful when:
- A module references a host function that will be registered later (e.g., after hardware initialization).
- The module is designed to handle a NULL function pointer gracefully.

Deferred symbols can be resolved later via `udynlink_link_symbol()`, `udynlink_link_incremental()`, or `udynlink_relink_all()`.

### Hash-Based and Trie-Based O(1)/O(k) Resolution

For hosts with large symbol tables, the `scripts/mkhostsyms` tool can read a host firmware ELF and generate a C header with a const GNU hash table (default, `--format gnu-hash`) or a compact search trie (`--format trie`). The hash table gives O(1) average-case lookup; the trie gives O(k) worst-case lookup where k is the symbol name length, with a more compact representation for symbol sets with shared prefixes. See the [Host Guide](integrating-as-host.md) for details.

---

## ABI Versioning and Architecture Tags

### Version Fields

The module header contains two version fields and an architecture tag:

- `mod_version` — the module's own ABI version (e.g., `1.0`).
- `udynlink_version` — the minimum loader version required to load this module.
- `arch_tag` — encodes the target core family, FPU presence, and float ABI.

### Version Macros

```c
#define UDYNLINK_MAKE_VERSION(major, minor)   (((major) << 8) | (minor))
#define UDYNLINK_GET_MAJOR_VERSION(v)         (((v) >> 8) & 0xFF)
#define UDYNLINK_GET_MINOR_VERSION(v)         ((v) & 0xFF)
```

The loader's ABI version is fixed at compile time:

```c
#define UDYNLINK_LOADER_ABI_VERSION   UDYNLINK_MAKE_VERSION(3, 1)
```

At load time, the loader checks:

```c
if (p_header->udynlink_version > UDYNLINK_LOADER_ABI_VERSION)
    return UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
```

A module requiring loader 3.2 cannot be loaded by a 3.1 loader. A module requiring 1.0 or 2.0 can be loaded by a 3.1 loader. The 3.0 → 3.1 bump exists for the multi-region section table: a **sectioned** image (`UDYNLINK_HDR_FLAG_SECTIONS`) must declare `udynlink_version >= 3.1` (mkmodule enforces this), so an old 3.0 loader rejects it with `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` instead of misreading the image. Untagged images keep `udynlink_version 3.0` and stay loadable by 3.0 loaders.

### Architecture Tag Layout

`arch_tag` is a 16-bit value:

| Bits | Meaning |
|------|---------|
| `[3:0]` | Core family ID |
| `4` | FPU present (`1` = yes) |
| `[6:5]` | Float ABI: `00` soft, `01` softfp, `10` hard |
| `[15:7]` | Reserved |

### Architecture Tag Constants

```c
#define UDYNLINK_ARCH_TAG_CORTEX_M0         0x01
#define UDYNLINK_ARCH_TAG_CORTEX_M0PLUS     0x02
#define UDYNLINK_ARCH_TAG_CORTEX_M3         0x03
#define UDYNLINK_ARCH_TAG_CORTEX_M4         0x04
#define UDYNLINK_ARCH_TAG_CORTEX_M4F        0x54
#define UDYNLINK_ARCH_TAG_CORTEX_M7         0x57
#define UDYNLINK_ARCH_TAG_CORTEX_M33        0x08
#define UDYNLINK_ARCH_TAG_CORTEX_M55        0x59
#define UDYNLINK_ARCH_TAG_CORTEX_M85        0x5A
```

Examples:
- Cortex-M4 soft-float: family `0x04`, no FPU, soft ABI = `0x04`.
- Cortex-M4F hard-float: family `0x04`, FPU present, hard ABI = `0x54`.
- Cortex-M7 hard-float: family `0x07`, FPU present, hard ABI = `0x57`.

### Runtime Validation

At load time, the loader compares the module's `arch_tag` against the host's `UDYNLINK_HOST_ARCH_TAG`:

1. **Family mismatch**: `(mod_arch & 0x0F) != (host_arch & 0x0F)` -> `UDYNLINK_ERR_LOAD_ARCH_MISMATCH`
2. **Hard-float module on soft-float host**: module float ABI = hard, host != hard -> mismatch.
3. **Softfp module on soft-float host**: module float ABI = softfp, host = soft -> mismatch.

This prevents loading a hard-float VFP module on a soft-float Cortex-M3 host, which would crash as soon as the module executed a floating-point instruction.

### v1.0 / v2.0 Backward Compatibility

Modules compiled with `--udynlink-version 1.0` or `2.0` use the same 32-byte header layout as v3.x (with the `flags` field at offset 0x0E). The loader detects older versions by checking `udynlink_version <= UDYNLINK_LOADER_ABI_VERSION` and accepts them as long as they do not require a newer loader. The `flags` field must be 0 in all pre-3.1 images.

---

## Cross-Module Calls via Runtime Thunks

### The Problem

In ABI v3.0, every module has its own LOT base address in `r9`. When module A calls a function in module B, two things must happen:

1. The callee (module B) needs its own `r9` to access its LOT.
2. The caller (module A) needs its original `r9` restored after the call.

Direct function pointers cannot solve this because a single `r9` value cannot serve two modules simultaneously. The `udynlink_deps` optional layer solves this by generating small **thunks** in executable RAM at load time, using a two-level dispatch: one per-module **gateway** (18 bytes) plus one per-function **stub** (10 bytes).

### The Thunk Template

Each cross-module function reference gets a 10-byte stub, and each callee module gets a single shared 18-byte gateway, both allocated from a host-provided thunk pool. The stub loads the target function address into `r12` (IP), then branches to the module's gateway; the gateway switches `r9` to the callee module's LOT base, calls the function, and restores the caller's `r9`:

```asm
; Per-function stub (10 bytes, one per cross-module function reference)
    movw    ip, #func_lo16   ; load low 16 bits of target function address
    movt    ip, #func_hi16   ; load high 16 bits of target function address
    b.n     gateway           ; branch to the module's shared gateway

; Per-module gateway (18 bytes, one per callee module)
    push.w  {r9, lr}         ; save caller's r9 and return address
    ldr.w   r9, [pc, #4]     ; load callee's ram_base from literal pool
    blx     ip                ; call function (address in ip from stub)
    pop.w   {r9, pc}         ; restore r9, return to caller
    .word   ram_base          ; callee module's LOT base
```

The stub-to-gateway branch uses a Thumb-16 `b.n` instruction, which has a ±2 KB range; if a stub is too far from its gateway, allocation fails. Because the gateway is shared by all of a module's stubs, a module with `N` cross-module function references costs `18 + 10*N` bytes of thunk pool.

### Why R12 (IP)?

The AAPCS reserves `r12` (IP) as an intra-procedure-call scratch register. Using it to hold the target function address avoids clobbering `r0-r3`, which are used for argument passing under the ARM calling convention. This means the thunk is transparent to the caller's argument setup — the thunk can be called with the same register state as the real function.

### Thunk Pool Requirements

The thunk pool must be in RAM that is both readable and executable by the MCU. On Cortex-M this is typically a region of SRAM or ITCM. The host provides the buffer and initializes the pool:

```c
static uint8_t g_thunk_buf[512];
static udynlink_thunk_pool_t g_thunk_pool;
udynlink_thunk_pool_init(&g_thunk_pool, g_thunk_buf, sizeof(g_thunk_buf));
```

Each cross-module function reference consumes a `UDYNLINK_STUB_SIZE` (10)
byte stub, plus one `UDYNLINK_GATEWAY_SIZE` (18) byte gateway per callee
module. The pool is a simple bump allocator; there is no per-thunk free
operation because thunks are only invalidated when the callee module is
unloaded (at which point the entire pool can be reset or discarded).

### Declaring Dependencies: `UDYNLINK_REQUIRES`

Modules declare their dependencies using the `UDYNLINK_REQUIRES` macro:

```c
UDYNLINK_REQUIRES(math);
```

This expands to a function-pointer variable whose symbol is renamed to `.udynlink.mod.requires.math`, plus a `used` dummy function that forces the compiler to emit an `R_ARM_GOT_BREL` (LOT) relocation for it:

```c
typedef void (*_udynlink_dep_fn_math)(void);
_udynlink_dep_fn_math _udynlink_dep_math
    __asm__(".udynlink.mod.requires.math");
__attribute__((used)) void _udynlink_dep_ref_math(void) {
    volatile _udynlink_dep_fn_math f = _udynlink_dep_math;
    (void)f;
}
```

At link time, this becomes an `UDYNLINK_SYM_TYPE_EXTERN` symbol. At load time, the core loader calls `udynlink_external_resolve_symbol(p_mod, ".udynlink.mod.requires.math")`. A dependency-aware host can resolve this by looking up the module named `math` in its registry.

### Resolution Flow

When a module references a symbol that might be in another module, the host's `udynlink_external_resolve_symbol()` callback typically checks in this order:

1. **Is it a dependency declaration?** (`udynlink_dep_is_dependency`) → resolve via `udynlink_dep_resolve_dependency()`
2. **Is it a function in a loaded dependency?** → resolve via `udynlink_dep_resolve_func()`. If the exporting module declared a preallocated thunk export for the symbol (`UDYNLINK_THUNK_EXPORT`), the resolver returns the already-generated in-module thunk; otherwise it allocates a thunk from the dynamic pool
3. **Is it a data variable in a loaded dependency?** → resolve via `udynlink_dep_resolve_data()` (returns address directly, no thunk needed)
4. **Is it a host firmware symbol?** → return the host's own address

The gateway and stub are generated at load time: the gateway is patched with the callee module's `ram_base` and the stub with the target function's absolute address. After loading, the module's LOT slot contains the stub address, so subsequent calls go through the stub → gateway → target automatically.

### Preallocated Thunk Exports

Modules may preallocate the thunk space for exports they expect to be imported:
`UDYNLINK_THUNK_GATEWAY()` reserves an 18-byte gateway slot and
`UDYNLINK_THUNK_EXPORT(fn)` a 10-byte stub slot, both in the module's own
`.bss` (section `.bss.udynlink_thunk_pool`, kept alive under `--gc-sections`
by a `KEEP` in `scripts/code_before_data.ld`). `udynlink_dep_load()`
automatically generates the gateway and stub bytes into those slots after
loading (via `udynlink_dep_generate_thunks()`), so the thunks are ready
before any importer resolves the symbols and the shared dynamic thunk pool is
not touched for them. See `docs/writing-modules.md` → "Preallocating
Cross-Module Thunk Exports".

Because the slots live inside the module's RAM, `udynlink_relocate_module()`
invalidates their absolute immediates (gateway `ram_base`, stub function
addresses); the host must call `udynlink_dep_generate_thunks()` again after
relocating such a module.

### Circular Dependency Detection

The dependency manager maintains a loading stack (max depth `UDYNLINK_DEP_MAX_DEPTH` = 8). If module A requires module B, and module B requires module A, the loader detects the cycle when the same module name appears on the loading stack. It returns `UDYNLINK_SYM_DEFERRED` for the circular reference, allowing the load to continue if the module handles deferred symbols gracefully.

---

## Standalone Call Thunks (`udynlink_thunk`)

### The Problem: r9-Aware Callbacks

The host must set `r9` to a module's LOT base before every call into that module. This works for direct calls via `UDYNLINK_PREPARE_CALL()` or `UDYNLINK_CALL()`, but breaks when a module function pointer must be passed to a third-party consumer (an ISR, a driver library, a timer callback) that is unaware of the r9/LOT convention. The `udynlink_thunk` optional layer solves this by creating thunks that manage `r9` automatically.

### Gateway + Stub Design

The thunk pool uses a two-level dispatch that is more compact than a single inline thunk per function reference when a module has multiple exported functions:

- **Per-module gateway** (18 bytes): saves the caller's `r9`, loads the callee module's `ram_base`, branches to the function address in `r12` (IP), then restores the caller's `r9` on return:

```asm
    push.w  {r9, lr}         ; save caller's r9 and return address
    ldr.w   r9, [pc, #4]     ; load callee's ram_base from literal pool
    blx     ip                ; call function (address in ip from stub)
    pop.w   {r9, pc}         ; restore r9, return to caller
    .word   ram_base          ; callee module's LOT base
```

- **Per-function stub** (10 bytes): loads the target function address into `r12` (IP) via `movw+movt`, then branches to the module's gateway:

```asm
    movw    ip, #func_lo16   ; load low 16 bits of function address
    movt    ip, #func_hi16   ; load high 16 bits of function address
    b.n     gateway           ; branch to module's gateway
```

Using `r12` (IP) preserves `r0-r3` (argument registers), so stubs are transparent to the caller's argument setup. The stub-to-gateway branch uses a Thumb-16 `b.n` instruction, which has a ±2 KB range. If a stub is too far from its gateway, allocation fails.

### Pool Layout

Gateways are allocated from the **end** of the pool, growing downward. Stubs are allocated from the **start** of the pool, growing upward:

```
[stub1][stub2]...[free gap]...[gateway2][gateway1]
^                   ^                       ^
base               used                  gateway_top
```

This layout allows `udynlink_external_find_stub()` to scan only the stub region by stepping through it at `UDYNLINK_STUB_SIZE` (10-byte) intervals. The pool is full when `used >= gateway_top`.

### Stub Deduplication

When `udynlink_thunk_make_call()` is called, it first checks whether a stub already exists for the target function address via `udynlink_external_find_stub()`. If a matching stub is found, it is reused — no additional stub or gateway is allocated. This is important when the same module function is passed as a callback to multiple consumers.

The default `udynlink_external_find_stub()` implementation scans the stub region linearly. Hosts may override it with a faster lookup (e.g., a hash table) by providing a non-weak definition.

### `udynlink_thunk_make_call()`

The main convenience function creates a callable thunk for any exported module symbol:

```c
udynlink_thunk_pool_t pool;
udynlink_thunk_pool_init(&pool, thunk_buf, sizeof(thunk_buf));

// Load the module first
udynlink_module_t mod;
udynlink_load_module(&mod, module_blob, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);

// Create a callable thunk — no PREPARE_CALL needed
uintptr_t thunk = udynlink_thunk_make_call(&pool, &mod, "my_callback");
if (thunk != 0) {
    void (*cb)(void) = (void (*)(void))thunk;
    cb();                        // direct call, r9 handled by thunk
    register_timer_cb(cb);      // safe to pass as callback
}
```

The returned function pointer can be called directly or passed as a callback without any r9 management. This makes it suitable for ISRs, RTOS timer callbacks, driver registration functions, or any consumer that is unaware of udynlink's r9/LOT convention.

### Relationship to `udynlink_deps`

The `udynlink_deps` layer includes `udynlink_thunk.h` and delegates all thunk pool management to it. The `udynlink_external_find_stub` weak function was moved from `udynlink_deps` to `udynlink_thunk`. Hosts using the dependency system do not need to change their integration — the thunk pool is still initialized and passed in the same way.

---

## Non-Contiguous Image Loading

### The `udynlink_module_image_t` Descriptor

For modules stored on non-memory-mapped media (SD card, SPI flash, decompressed buffers), udynlink provides a descriptor-based loader that does not require the entire image to be contiguous in RAM first:

```c
typedef struct {
    const udynlink_module_header_t *p_header;      // module header
    const uint32_t                *p_relocations;  // relocation table
    const uint32_t                *p_symtab;       // symbol table base
    const uint8_t                 *p_code;         // .text section
    const uint8_t                 *p_data;         // .data section
} udynlink_module_image_t;
```

Each field points to one section of the module image independently. The loader reads relocation and symbol information through these pointers; it never assumes the image is contiguous. For a standard contiguous UDLM buffer, use `udynlink_image_from_memory()` to populate the descriptor:

```c
udynlink_module_image_t image;
udynlink_image_from_memory(base_addr, &image);
```

### Loading Function

```c
udynlink_error_t udynlink_load_module_image(
    udynlink_module_t *p_mod,
    const udynlink_module_image_t *image,
    void *load_addr,        // NULL = auto-allocate
    size_t load_size,       // size of load_addr region
    udynlink_load_mode_t load_mode
);
```

### Supported Modes

All three load modes are supported:
- **COPY_ALL** — copies header, metadata, code, and data into a single contiguous RAM buffer.
- **COPY_TEXT_DATA** — copies code and data into RAM; metadata stays at the source pointers.
- **XIP** — copies only data to RAM; code stays at `image->p_code` (must be in executable flash).

For `COPY_TEXT_DATA` and `XIP`, the metadata (header, relocation table, symbol table) must remain accessible via `image->p_header` for post-load symbol lookups. If your source layout does not satisfy this (e.g., decompressed metadata and code live in different buffers), use `COPY_ALL`.

### Custom Loading Pipelines

Advanced users can implement their own loading pipeline using the low-level primitives:

1. **Read and validate the header** — `udynlink_validate_header(&header)`
2. **Compute RAM size** — `udynlink_compute_ram_size(&header, mode)`
3. **Allocate RAM** — your own allocator or pool
4. **Copy sections** — from your I/O source into the RAM buffer at the right offsets
5. **Zero BSS** — `memset(data + header.data_size, 0, header.bss_size)`
6. **Apply relocations** — `udynlink_load_apply_relocations(p_mod, &header, relocs, symtab)`

This gives you full control over buffering strategy, I/O chunking, and memory layout.

### Planning APIs

Before loading, you can inspect a module image without allocating RAM:

```c
// Validate signature and ABI version
udynlink_error_t err = udynlink_validate_header(&header);

// Compute RAM needed
size_t ram = udynlink_compute_ram_size(&header, UDYNLINK_LOAD_MODE_COPY_ALL);

// Get module name directly from the symbol table
const char *name = udynlink_image_get_module_name(image.p_symtab);
```

---

## Cross-Reference

- [Testing Guide](testing.md) — How to run the QEMU test suite for all supported platforms.
- [API Reference](api-reference.md) — Complete listing of all public functions, macros, and error codes.
- [Module Guide](writing-modules.md) — Practical guide to writing C/C++ code that compiles into udynlink modules.
- [Host Guide](integrating-as-host.md) — How to integrate udynlink into your firmware, implement the externals, and use hash-based symbol resolution.
- [Examples](examples.md) — Sample modules and host sketches.
