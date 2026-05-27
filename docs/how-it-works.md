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
10. [Non-Contiguous Image Loading](#non-contiguous-image-loading)

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
| Header                    | 32 bytes              |
+---------------------------------------------------+
| Relocation Table          | num_rels * 8 bytes    |
+---------------------------------------------------+
| Symbol Table              | symt_size bytes       |
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
| 0x0E | `reserved` | 2 | Reserved (must be 0). |
| 0x10 | `symt_size` | 4 | Size of the symbol table in bytes. |
| 0x14 | `code_size` | 4 | Size of `.text` section in bytes. |
| 0x18 | `data_size` | 4 | Size of `.data` section in bytes. |
| 0x1C | `bss_size` | 4 | Size of `.bss` section in bytes. |

**ABI v1.0 backward compatibility**: For modules compiled with `--udynlink-version 1.0`, the header is also 32 bytes (same layout, with `reserved` at 0x0E). The loader accepts v1.0 modules as long as `udynlink_version <= UDYNLINK_LOADER_ABI_VERSION`.

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

The entire module image (header, relocation table, symbol table, `.text`, and `.data`) is copied into RAM.

**Post-load RAM layout:**

```
+---------------------------------------------------+
| LOT entries               | num_lot * 4 bytes       |
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

Where `header_offset = sizeof(header) + num_rels*8 + symt_size + padding`.

---

## Symbol Resolution at Load Time

When the loader encounters an `UDYNLINK_SYM_TYPE_EXTERN` symbol during relocation processing, it must resolve the symbol to an actual address before writing it into the LOT or data section.

### Foreign Symbol Resolution

The loader calls the host-provided callback:

```c
uint32_t udynlink_external_resolve_symbol(const char *name);
```

The host firmware looks up `name` in its own symbol table and returns the address, or `0` if the symbol is not found. If the symbol cannot be resolved, loading fails with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

The host may also return `UDYNLINK_SYM_DEFERRED` (the sentinel value `(uint32_t)1`) to defer the symbol. When this happens, the loader writes `0` to the relocation slot and **continues loading**. This is useful when:
- A module references a host function that will be registered later (e.g., after hardware initialization).
- The module is designed to handle a NULL function pointer gracefully.

Deferred symbols can be resolved later via `udynlink_link_symbol()`, `udynlink_link_incremental()`, or `udynlink_relink_all()`.

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
#define UDYNLINK_LOADER_ABI_VERSION   UDYNLINK_MAKE_VERSION(3, 0)
```

At load time, the loader checks:

```c
if (p_header->udynlink_version > UDYNLINK_LOADER_ABI_VERSION)
    return UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
```

A module requiring loader 3.1 cannot be loaded by a 3.0 loader. A module requiring 1.0 or 2.0 can be loaded by a 3.0 loader.

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

Modules compiled with `--udynlink-version 1.0` or `2.0` use the same 32-byte header layout as v3.0 (with `reserved` at offset 0x0E). The loader detects older versions by checking `udynlink_version <= UDYNLINK_LOADER_ABI_VERSION` and accepts them as long as they do not require a newer loader. The `reserved` field must be 0 in all valid images.

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
