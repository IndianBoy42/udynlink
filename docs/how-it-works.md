# How udynlink Works

## Table of Contents

1. [Overview: Position-Independent Code and Data](#overview-position-independent-code-and-data)
2. [The Linker Offset Table (LOT)](#the-linker-offset-table-lot)
3. [Function Wrapping (Assembly Prologue)](#function-wrapping-assembly-prologue)
4. [The mkmodule Pipeline](#the-mkmodule-pipeline)
5. [Module Binary Format](#module-binary-format)
6. [Relocation Processing](#relocation-processing)
7. [Three Load Modes](#three-load-modes)
8. [Symbol Resolution at Load Time](#symbol-resolution-at-load-time)
9. [ABI Versioning and Architecture Tags](#abi-versioning-and-architecture-tags)
10. [Module Dependencies (ABI 2.0+)](#module-dependencies-abi-20)
11. [Streaming I/O Loading](#streaming-io-loading)

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

The original udynlink used a function-pointer callback at address `0x1c` to compute `r9` from the current PC. The eh2k fork replaces this with a **fixed memory address** convention:

```c
#define UDYNLINK_LOT_BASE_ADDR  0x20000000
```

This address is configurable at compile time by defining `UDYNLINK_LOT_BASE_ADDR` before including `udynlink.h`. The default `0x20000000` is chosen because it lies in SRAM on most Cortex-M devices and is unlikely to collide with legitimate firmware data at that exact word.

### The Host's Responsibility

Before calling any exported function from a loaded module, the host firmware **must** write the module's RAM base address to the fixed LOT base location:

```c
*(uint32_t *)UDYNLINK_LOT_BASE_ADDR = p_mod->ram_base;
```

Each module's wrapper prologue (see next section) then loads `r9` from this address. Because the wrapper runs before the real function body, `r9` is correctly set for all subsequent PIC data accesses inside that function.

### Why the Change from 0x1c

The original approach stored a function pointer at `0x1c` that took the caller's PC and returned the correct `r9` value. This required:
- A writable `0x1c` location
- A function call on every module entry (overhead)
- Host firmware to implement and install the callback

The fixed-address approach is simpler and faster: a single 32-bit load from a known address replaces a function call and return. The only requirement is that the host writes one word before crossing the module boundary.

---

## Function Wrapping (Assembly Prologue)

### The Wrapper Concept

Every exported (non-static) function in a module receives a small assembly **wrapper**. The wrapper is the actual global symbol the host calls. Its job is to:

1. Save the caller's `r9` and `lr`.
2. Load `r9` from the fixed LOT base address.
3. Call the real function.
4. Restore the caller's `r9` and return.

This ensures that `r9` is valid for the duration of the call without forcing the host firmware to manage it manually.

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
    push    {r1}
    ldr     r1, .L1my_func
    ldr     r9, [r1]          ; r9 = *(uint32_t *)LOT_BASE
    pop     {r1}
    bl      __abcdef123_my_func
    pop     {r9, pc}

.L1my_func:
    .word   0x20000000
    .size   my_func, . - my_func
```

Instruction-by-instruction breakdown:

| Instruction | Purpose |
|-------------|---------|
| `push {r9, lr}` | Save caller's `r9` and link register. |
| `push {r1}` | Save `r1` (caller argument register) so we can use it as scratch. |
| `ldr r1, .L1my_func` | Load the address of the LOT base literal pool word into `r1`. |
| `ldr r9, [r1]` | Dereference it: `r9 = *(uint32_t *)0x20000000`. |
| `pop {r1}` | Restore caller's `r1`. |
| `bl __abcdef123_my_func` | Branch to the real (renamed) function body. |
| `pop {r9, pc}` | Restore caller's `r9` and return. |

### Cortex-M0 / M0+ Prologue (armv6-m)

Cortex-M0 lacks `ldm`/`stm` of arbitrary register sets and the `ldr Rd, [Rn, Rm]` addressing mode used by the compiler for LOT accesses. The wrapper is slightly different to work around these limitations:

```asm
    .thumb_func
    .section .text_nogc, "x"
    .globl  my_func
my_func:
    mov     r2, r9
    push    {r1, r2, lr}
    ldr     r1, .L1my_func
    ldr     r2, [r1]
    mov     r9, r2            ; r9 = *(uint32_t *)LOT_BASE
    bl      __abcdef123_my_func
    ldr     r2, [sp, #4]
    mov     r9, r2
    pop     {r1, r2, pc}

.L1my_func:
    .word   0x20000000
```

On M0, `r2` is used as scratch instead of `r1` because the `push`/`pop` instructions are more restricted.

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
-fno-exceptions -fno-rtti -fno-use-cxa-atexit
```

and also compiles `cpp_init_fini.c` into the object list. This file provides `__init_array`, which iterates over `.preinit_array` and `.init_array` to run global constructors.

**Function wrapping happens in this step:**

1. `mkmodule` discovers all public functions in the object file via `get_public_functions_in_object`.
2. Each public function is renamed to a mangled name (`__<md5prefix>__<original>`) using `objcopy --redefine-sym`.
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

1. Write the 36-byte header (see [Module Binary Format](#module-binary-format)).
2. Write relocation table entries.
3. Write the symbol table.
4. Write the dependency string table (if any).
5. Pad to 4-byte alignment.
6. Append `.text` and `.data` sections.

The result is a `.bin` file that can be embedded in host firmware as a byte array, stored in flash, or loaded from external storage.

---

## Module Binary Format

The module binary is a contiguous blob of bytes with the following layout:

```
+---------------------------------------------------+
| Header                    | 36 bytes              |
+---------------------------------------------------+
| Relocation Table          | num_rels * 8 bytes    |
+---------------------------------------------------+
| Symbol Table              | symt_size bytes       |
+---------------------------------------------------+
| Dependency String Table   | deps_strtab_size bytes|
+---------------------------------------------------+
| Padding to 4-byte align   | 0-3 bytes             |
+---------------------------------------------------+
| Code (.text)              | code_size bytes       |
+---------------------------------------------------+
| Data (.data)              | data_size bytes       |
+---------------------------------------------------+
```

### Header Fields (`udynlink_module_header_t`)

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0x00 | `sign` | 4 | Signature: `'U' 'D' 'L' 'M'` (little-endian `0x4D4C4455` or `(((uint32_t)'M'<<24)|...|'U')` depending on host endianness; the loader checks against `(((uint32_t)'M'<<24)|((uint32_t)'L'<<16)|((uint32_t)'D'<<8)|(uint32_t)'U')`). |
| 0x04 | `mod_version` | 2 | Module ABI version. Encoded as `(major << 8) \| minor`. |
| 0x06 | `udynlink_version` | 2 | Minimum loader ABI version required. Same encoding. |
| 0x08 | `arch_tag` | 2 | Target architecture + float ABI tag (see [ABI Versioning](#abi-versioning-and-architecture-tags)). |
| 0x0A | `num_lot` | 2 | Number of LOT entries (each is one 32-bit word). |
| 0x0C | `num_rels` | 2 | Total number of relocation entries in the relocation table. |
| 0x0E | `num_deps` | 2 | Number of dependency names (ABI 2.0+; 0 for v1.0 modules). |
| 0x10 | `symt_size` | 4 | Size of the symbol table in bytes. |
| 0x14 | `code_size` | 4 | Size of `.text` section in bytes. |
| 0x18 | `data_size` | 4 | Size of `.data` section in bytes. |
| 0x1C | `bss_size` | 4 | Size of `.bss` section in bytes. |
| 0x20 | `deps_strtab_size` | 4 | Size of dependency string table in bytes (ABI 2.0+; 0 for v1.0 modules). |

**ABI v1.0 backward compatibility**: For modules compiled with `--udynlink-version 1.0`, the header is only 32 bytes. The `num_deps` and `deps_strtab_size` fields do not exist. The loader detects v1.0 by checking `udynlink_version < UDYNLINK_MAKE_VERSION(2, 0)` and computes header size as 32 instead of 36.

### Symbol Table Encoding

The symbol table begins with a 4-byte word containing the number of symbol entries, followed by 8 bytes per entry (two 32-bit words):

- **Word 0**: `name_offset | (type_data << 28)`
  - Bits `[27:0]` — byte offset from the start of the symbol table to the NUL-terminated name string.
  - Bit `30` — `1` if the symbol is in the code section, `0` if in data.
  - Bits `[29:28]` — visibility: `0` local, `1` exported, `2` external, `3` module name.
  - Internal symbols have `name_offset = 0` and no name string in the table.
- **Word 1**: `value` — the symbol's address (relative to its section base for local/exported symbols; undefined for external symbols).

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

If `lot_offset < num_lot`, the relocation targets a LOT entry. If `lot_offset >= num_lot`, the relocation targets a word in `.data` at word offset `(lot_offset - num_lot)`.

### Dependency String Table

For ABI 2.0+ modules with dependencies, the dependency string table sits between the symbol table and the code section. It contains null-terminated module names in the order they were declared via `--depends`:

```
"mod_a\0mod_b\0mod_c\0"
```

The size is given by `deps_strtab_size`. The table is padded to a 4-byte boundary to ensure the following `.text` section is aligned.

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

### Deduplication

If a module references the same symbol multiple times (for example, an array where every element points to the same function), `mkmodule` ensures only the **first** relocation to that symbol consumes a LOT slot. Subsequent relocations reuse the same `lot_offset`. This keeps the LOT small.

### PC-Relative Relocations (`R_ARM_THM_CALL`, `R_ARM_THM_JUMP24`)

Branch and call instructions in Thumb-2 are PC-relative. They do not need runtime relocation because the offset from the caller to the callee is the same regardless of where the module is loaded. `mkmodule` ignores these relocations, and the loader does not process them.

---

## Three Load Modes

udynlink supports three ways of bringing a module into memory, controlled by the `udynlink_load_mode_t` argument to `udynlink_load_module`.

### COPY_ALL

The entire module image (header, relocation table, symbol table, dependency string table, `.text`, and `.data`) is copied into RAM.

**Post-load RAM layout:**

```
+---------------------------------------------------+
| LOT entries               | num_lot * 4 bytes       |
+---------------------------------------------------+
| Header + Relocs + Symtab + Deps strtab + padding    |
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

Only `.text` and `.data` are copied into RAM. The header, relocation table, symbol table, and dependency string table remain at the original `base_addr` (which must remain accessible for symbol lookups and future unloading).

**Post-load RAM layout:**

```
+---------------------------------------------------+
| LOT entries               | num_lot * 4 bytes       |
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
| Data (.data)              | data_size bytes       |
+---------------------------------------------------+
| BSS (.bss)                | bss_size bytes (zeroed)|
+---------------------------------------------------+
```

**Caveat**: "XIP" does **not** mean "zero RAM required". Even if `.data` and `.bss` are empty, the module still needs RAM for the LOT and for applying relocations. Truly RAM-less modules are rare.

### RAM Size Calculation

The loader computes required RAM as:

```c
uint32_t ram = num_lot * sizeof(uint32_t) + data_size + bss_size;
if (mode == COPY_TEXT_DATA)   ram += code_size;
if (mode == COPY_ALL)    ram += header_offset + code_size;
```

Where `header_offset = sizeof(header) + num_rels*8 + symt_size + deps_strtab_size + padding`.

---

## Symbol Resolution at Load Time

When the loader encounters an `UDYNLINK_SYM_TYPE_EXTERN` symbol during relocation processing, it must resolve the symbol to an actual address before writing it into the LOT or data section.

### Foreign Symbol Resolution

The loader calls the host-provided callback:

```c
uint32_t udynlink_external_resolve_symbol(const char *name);
```

The host firmware looks up `name` in its own symbol table (or in any loaded modules) and returns the address, or `0` if the symbol is not found. If the symbol cannot be resolved, loading fails with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

### Three-Tier Resolution (ABI 2.0+)

With the introduction of module dependencies, resolution follows a three-tier search order:

1. **Critical host symbols** — `udynlink_external_resolve_critical_symbol(name)` is called first. This is intended for core firmware services that should always shadow any module-provided symbol.
2. **Dependency modules** — If the critical lookup returns 0, the loader searches already-loaded modules that were declared as dependencies via `mkmodule --depends`. It calls `udynlink_lookup_symbol(dep_mod, name, &sym)` on each dependency in order.
3. **Fallback host symbols** — If still unresolved, the loader calls `udynlink_external_resolve_symbol(name)` as a final fallback.

This allows modules to depend on symbols exported by other modules without the host firmware needing to re-export them explicitly.

### Hash-Based O(1) Resolution

For hosts with large symbol tables, the `scripts/mkhostsyms` tool can read a host firmware ELF and generate a C header with a const GNU hash table. The host implements `udynlink_external_resolve_symbol` as a hash table lookup for O(1) resolution. See the [Host Guide](integrating-as-host.md) for details.

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
#define UDYNLINK_LOADER_ABI_VERSION   UDYNLINK_MAKE_VERSION(2, 0)
```

At load time, the loader checks:

```c
if (p_header->udynlink_version > UDYNLINK_LOADER_ABI_VERSION)
    return UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
```

A module requiring loader 2.1 cannot be loaded by a 2.0 loader. A module requiring 1.0 can be loaded by a 2.0 loader.

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

### v1.0 Backward Compatibility

Modules compiled with `--udynlink-version 1.0` omit the `num_deps` and `deps_strtab_size` header fields, making the header 32 bytes. The loader detects this via `p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 0)` and adjusts all header-derived offsets accordingly. Dependency tracking is unavailable for v1.0 modules.

---

## Module Dependencies (ABI 2.0+)

### Declaring Dependencies at Build Time

Use the `--depends` flag when building a module:

```bash
python3 mkmodule --depends mod_a,mod_b --gen-c-header my_module.c
```

This embeds the dependency names into the module binary. The `udynlink_version` is automatically bumped to at least 2.0 if `--depends` is used.

### Header Fields

- `num_deps` — number of dependency names.
- `deps_strtab_size` — total size of the null-terminated name list in bytes.

### Load-Time Validation

During `udynlink_load_module`:

1. The loader reads each dependency name from the dependency string table.
2. For each name, it calls `udynlink_external_get_module_handle(dep_name)`.
3. If any dependency is not found, loading fails with `UDYNLINK_ERR_LOAD_MISSING_DEP`.
4. Valid dependency handles are stored in `p_mod->deps[]`, and each dependency's `dep_refcount` is incremented.

### Unload Protection

A module with `dep_refcount > 0` cannot be unloaded:

```c
if (p_mod->dep_refcount > 0)
    return UDYNLINK_ERR_MODULE_HAS_DEPENDENTS;
```

When a module is unloaded, the loader decrements the `dep_refcount` of all its dependencies. This prevents a host from accidentally unloading a shared library module while other modules still reference its symbols.

### Circular Dependencies

By default, circular dependencies are **rejected** at load time. If `mod_a` depends on `mod_b` and `mod_b` depends on `mod_a`, neither can load in the normal order because each requires the other to already be loaded.

**Default behavior:**
- `mod_a` tries to load → loader looks for `mod_b` → not loaded yet → `UDYNLINK_ERR_LOAD_MISSING_DEP`
- `mod_b` tries to load → loader looks for `mod_a` → not loaded yet → `UDYNLINK_ERR_LOAD_MISSING_DEP`

Self-dependencies (a module listing itself in `--depends`) are always detected and rejected with `UDYNLINK_ERR_LOAD_CIRCULAR_DEP`.

#### Opt-In Deferred Loading for Circular Graphs

Hosts that need to load circular dependency graphs can use **deferred dependency loading**. The host signals the loader to skip a dependency (rather than fail) by returning `UDYNLINK_DEP_DEFERRED` from `udynlink_external_get_module_handle()`:

```c
#define UDYNLINK_DEP_DEFERRED ((udynlink_module_t*)1)
```

When the loader sees this sentinel, it skips the dependency: no entry is added to `deps[]`, no `dep_refcount` is incremented, and loading continues. The deferred symbol's LOT slot is left at `0`.

After both modules are loaded, the host calls `udynlink_link_dependency(a, b)` to link them symmetrically:

1. Checks if `a` declares `b` as a dependency and `b` is not yet in `a->deps[]`. If so, adds `b` and increments `b->dep_refcount`, then re-runs the three-tier EXTERN resolution chain for `a`.
2. Repeats the check in the reverse direction (`b` → `a`).
3. Returns `UDYNLINK_OK` even if nothing changed (idempotent).

**Why re-resolve:** During initial load, an `EXTERN` symbol might have resolved from the host fallback (tier 3) because the dependency wasn't in `deps[]` yet. After linking, tier 2 (dependency module) should take precedence. A full re-scan is correct and the overhead is negligible for typical embedded modules.

**Example:**

```c
// Host pre-registers both as "loading"
host_mark_loading("mod_a");
host_mark_loading("mod_b");

// Load A (B is deferred)
udynlink_load_module(&mod_a, mod_a_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register_module(&mod_a);

// Load B (A is already loaded)
udynlink_load_module(&mod_b, mod_b_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register_module(&mod_b);

// Link the deferred direction
udynlink_link_dependency(&mod_a, &mod_b);
```

#### Circular Dependencies and Unload

Circular deps still create **un-unloadable modules** because each module's `dep_refcount` remains ≥ 1. This is intentional: the loader does not support `udynlink_unlink_dependency()`, and a cycle means no module can be unloaded individually. The host must either:
- Accept that circular modules live for the lifetime of the firmware, or
- Unload all modules in the cycle simultaneously via a host-managed bulk teardown.

#### Optional Dependencies

Deferred loading also enables **optional dependencies**. If a host returns `UDYNLINK_DEP_DEFERRED` for an optional dependency, the module loads successfully but the deferred symbol resolves to `0`. The module can detect this at runtime:

```c
extern void log_printf(const char *fmt, ...);

void my_init(void) {
    if (log_printf != NULL) {
        log_printf("module initialized\n");
    }
}
```

Later, if the optional module is loaded, the host calls `udynlink_link_dependency(consumer, optional)` and the symbol is re-resolved from the newly loaded dependency.

**Caveat:** If the host provides a fallback stub for the optional symbol, the LOT slot is non-zero and the module's `NULL` check will falsely succeed. Hosts should not provide stubs for optional symbols if they want modules to detect absence.

#### Deferred Symbol Resolution

Individual symbols (not just whole dependencies) can be deferred via `UDYNLINK_SYM_DEFERRED`:

```c
#define UDYNLINK_SYM_DEFERRED ((uint32_t)1)
```

When `udynlink_external_resolve_critical_symbol()` or `udynlink_external_resolve_symbol()` returns this sentinel, the loader writes `0` to the relocation slot and **continues loading** instead of failing. This is useful when:
- A module references a host function that will be registered later (e.g., after hardware initialization).
- A symbol from another module is known but not yet available.
- The module is designed to handle a NULL function pointer gracefully.

**Important:** The critical-symbol callback returning `UDYNLINK_SYM_DEFERRED` weakens the "critical" semantics. Hosts should only do this for truly deferrable critical symbols.

#### Direct Symbol Patching

For host-mediated symbol injection without re-running the three-tier chain, use `udynlink_link_symbol()`:

```c
udynlink_error_t udynlink_link_symbol(udynlink_module_t *mod,
                                      const char *sym_name,
                                      uint32_t sym_addr);
```

This scans the module's relocation table for entries referencing `sym_name` and overwrites the slot directly with `sym_addr`. It does not update `deps[]`, `dep_refcount`, or the symbol table. Use cases:
1. **Deferred host symbols** — the host knows the address now and wants to patch it directly.
2. **Hot-patching** — replace a module's extern reference with a different implementation at runtime (e.g., a mock for testing).
3. **Dynamic symbol tables** — the host maintains its own symbol table and pushes updates into loaded modules.

---

## Streaming I/O Loading

### The `udynlink_io_t` Interface

For modules stored on non-memory-mapped media (SD card, SPI flash, serial stream), udynlink provides a streaming loader that does not require the entire image to be in RAM first:

```c
typedef int32_t (*udynlink_read_cb_t)(void *pv_ctx, void *buf,
                                      uint32_t num_bytes, uint32_t offset);
typedef int32_t (*udynlink_get_size_cb_t)(void *pv_ctx);

typedef struct {
    udynlink_read_cb_t      read;
    udynlink_get_size_cb_t  get_size;
    void                   *pv_ctx;
} udynlink_io_t;
```

- `read(pv_ctx, buf, num_bytes, offset)` — read `num_bytes` from `offset` into `buf`. Returns bytes read or `-1` on error.
- `get_size(pv_ctx)` — return total image size or `-1` on error.
- `pv_ctx` — opaque user pointer (e.g., a file handle or flash device descriptor).

### Loading Function

```c
udynlink_error_t udynlink_load_module_from_stream(
    udynlink_module_t *p_mod,
    const udynlink_io_t *p_io,
    void *load_addr,        // NULL = auto-allocate
    uint32_t load_size,     // size of load_addr region
    udynlink_load_mode_t load_mode,
    void *scratch_buf,     // scratch buffer (min 132 bytes)
    uint32_t scratch_buf_size  // must be >= UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE
);
```

### Supported Modes and Restrictions

- **COPY_ALL** and **COPY_TEXT_DATA** are supported.
- **XIP is not supported** — returns `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED`. Streaming from a non-memory-mapped source directly into executable flash is not practical in the general case.

### Work Buffer

The caller provides a scratch buffer for chunked I/O. The minimum size is **64 bytes** (`UDYNLINK_STREAM_MIN_WORK_BUF_SIZE`). The optimal size, which allows the loader to read all metadata (header + relocations + symbol table) in a single `read()` call, is:

```c
uint32_t optimal = udynlink_get_stream_metadata_size(p_io);
// = byte offset to the code section (header + relocs + symtab + deps strtab + padding)
```

A typical default is 512 bytes (matching FatFS sector size), defined as `UDYNLINK_STREAM_BUF_SIZE`.

### On-Demand Symbol Resolution

Unlike the memory-mapped path, the streaming loader does not hold the entire symbol table in RAM. During relocation processing, it reads individual symbol entries and their names from the stream as needed. This minimizes RAM usage for large modules with large symbol tables.

### COPY_TEXT_DATA Streaming Internals

When `COPY_TEXT_DATA` is requested via the streaming loader, the implementation internally loads the module as if `COPY_ALL` was requested (copying header + metadata + code + data into RAM). After loading, the module structure is adjusted so that `udynlink_lookup_symbol` and subsequent operations work correctly. The effect on the caller is the same: text and data sections are in RAM, metadata is accessible.

---

## Cross-Reference

- [Testing Guide](testing.md) — How to run the QEMU test suite for all supported platforms.
- [API Reference](api-reference.md) — Complete listing of all public functions, macros, and error codes.
- [Module Guide](writing-modules.md) — Practical guide to writing C/C++ code that compiles into udynlink modules.
- [Host Guide](integrating-as-host.md) — How to integrate udynlink into your firmware, implement the externals, and use hash-based symbol resolution.
- [Examples](examples.md) — Sample modules and host sketches.
