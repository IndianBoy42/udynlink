# Plan: R_ARM_REL32 and R_ARM_GOT_PREL Relocation Support

## Goal

Investigate and add support for `R_ARM_REL32` and `R_ARM_GOT_PREL` relocations to
udynlink. The motivation is to enable Rust/Zig (and other LLVM-based toolchains) to
produce loadable modules. These toolchains appear to generate `R_ARM_GOT_PREL` (and
potentially `R_ARM_REL32`) in PIC mode instead of GCC's `R_ARM_GOT_BREL`.

**Key constraints:**
- Support all three load modes: `COPY_ALL`, `COPY_TEXT_DATA`, and `XIP`
- Minimize code size and complexity increase in the loader
- Maintain backward compatibility with existing `R_ARM_GOT_BREL` modules

---

## 1. Research Summary

### 1.1 ARM ELF Relocation Formulas

| Type | Formula | Class | Semantics |
|------|---------|-------|-----------|
| `R_ARM_GOT_BREL` | `GOT(S) + A - GOT_ORG` | Static, Data | **Base-relative** offset from GOT origin to GOT entry. The code loads the offset and then uses `[r9, offset]` to dereference. The offset is invariant regardless of where the module is loaded. |
| `R_ARM_GOT_PREL` | `GOT(S) + A - P` | Static, Data | **PC-relative** offset from the relocation site to the GOT entry. The code loads the offset, computes `PC + offset` to get the GOT entry address, then dereferences it. The offset depends on the runtime distance between the code and the GOT. |
| `R_ARM_REL32` | `((S + A) \| T) - P` | Static, Data | **PC-relative** offset directly to the symbol. The code loads the offset and computes `PC + offset` to get the symbol address. No GOT indirection. |

Where:
- `S` = symbol value
- `A` = addend
- `P` = place (address of the relocated word)
- `T` = Thumb interworking bit
- `GOT_ORG` = base/origin of the GOT (the LOT base in udynlink)
- `GOT(S)` = address of the GOT entry for symbol S

### 1.2 How the Current Model Works (R_ARM_GOT_BREL)

udynlink currently uses `R_ARM_GOT_BREL` exclusively. The compiler (GCC) generates code like:

```asm
    ldr r3, [pc, #12]      ; load the LOT index from the literal pool
    ldr r3, [r9, r3]       ; load the resolved symbol address from the LOT
```

The `mkmodule` toolchain:
1. Reads `R_ARM_GOT_BREL` relocations from the linked ELF
2. Allocates one LOT slot per unique symbol
3. **Patches the `.text` word** at the relocation offset to contain the LOT index (in bytes)
4. The loader, at load time, **writes the resolved symbol address into the LOT slot**

**Critical property:** The code is **never patched at load time**. The LOT index is invariant across all load modes. The loader only writes to the LOT (in RAM), never to the code.

### 1.3 What LLVM Generates (R_ARM_GOT_PREL)

When LLVM compiles ARM code with `-fPIC`, it may generate `R_ARM_GOT_PREL` instead of `R_ARM_GOT_BREL`. The code sequence looks like:

```asm
    ldr r3, [pc, #12]      ; load the PC-relative offset to the GOT entry
    add r3, pc, r3          ; r3 = address of the GOT entry
    ldr r3, [r3]            ; r3 = resolved symbol address from the GOT entry
```

The `.word` in the literal pool contains the **PC-relative offset** from the word's address to the GOT entry address.

**Key difference:** The offset depends on the actual runtime distance between the code (where the `.word` lives) and the GOT entry. If the code and GOT are at different relative positions at runtime than they were at link time, the offset is wrong.

### 1.4 R_ARM_REL32

`R_ARM_REL32` is a direct PC-relative offset to the symbol, bypassing the GOT entirely:

```asm
    ldr r3, [pc, #12]      ; load the PC-relative offset to the symbol
    add r3, pc, r3          ; r3 = address of the symbol
```

- **For local symbols**: The linker can resolve this at link time. However, if the module is loaded with sections at different relative positions (e.g., XIP where code is in flash and data is in RAM), the offset may need adjustment.
- **For external symbols**: The linker cannot resolve this because the symbol's address is unknown. The dynamic linker would need to patch it at load time. In udynlink, we'd need to either reject it or use a trampoline.

---

## 2. Architectural Analysis

### 2.1 The Core Problem: Load-Time Code Patching

Both `R_ARM_GOT_PREL` and `R_ARM_REL32` (for cross-section references) require patching the **code** at load time because the PC-relative offset depends on the runtime layout.

**Current model (R_ARM_GOT_BREL):**
- Code is never patched at load time
- Works in all three load modes (COPY_ALL, COPY_TEXT_DATA, XIP)

**New model (R_ARM_GOT_PREL / R_ARM_REL32):**
- Code must be patched at load time to adjust PC-relative offsets
- Works in COPY_ALL and COPY_TEXT_DATA (code is in RAM)
- **Does NOT work in XIP** (code is in flash, not writable)

This is the fundamental architectural tension.

### 2.2 Memory Layout Implications

In the linked ELF (linker script origin = 0):

```
.text @ 0
.data @ code_size
.bss  @ code_size + data_size
.got  @ code_size + data_size + bss_size   (if present)
```

**Current udynlink load-time layouts:**

**COPY_ALL:**
```
RAM base:
  [LOT entries]                    <- num_lot * 4 bytes
  [Header + Relocs + Symtab]       <- copied from image
  [.text]                          <- copied from image
  [.data]                          <- copied from image
  [.bss]                           <- zeroed
```

**COPY_TEXT_DATA:**
```
RAM base:
  [LOT entries]                    <- num_lot * 4 bytes
  [.text]                          <- copied from image
  [.data]                          <- copied from image
  [.bss]                           <- zeroed
```

**XIP:**
```
RAM base:
  [LOT entries]                    <- num_lot * 4 bytes
  [.data]                          <- copied from image
  [.bss]                           <- zeroed

Flash:
  [Header + Relocs + Symtab]
  [.text]
```

**For R_ARM_GOT_PREL:**
The PC-relative offset from a word in `.text` to the GOT entry is:

- **At link time:** `got_link_addr - word_link_addr`
- **At runtime (COPY_ALL):** `got_ram_addr - word_ram_addr`

For COPY_ALL: `word_ram_addr = ram_base + lot_size + metadata_size + word_offset`
`got_ram_addr = ram_base + lot_size + metadata_size + code_size + got_offset` (if GOT is in data section)

Wait, the GOT is not in the current udynlink layout. In the current model, the LOT is the "GOT" and it's at the start of RAM, before the code. So:

- `got_ram_addr = ram_base + got_entry_index * 4`
- `word_ram_addr = ram_base + lot_size + metadata_size + word_offset` (COPY_ALL)

The link-time GOT address is different from the runtime GOT address. The link-time GOT is in the `.got` section (or wherever the linker placed it). The runtime GOT is the LOT at the start of RAM.

**This is the crux:** `R_ARM_GOT_PREL` assumes the GOT is at a fixed position relative to the code. In udynlink's current model, the LOT is at the start of RAM, and the code is at a different offset depending on the load mode. The relative distance between code and LOT is **not** the same as it was at link time.

### 2.3 Options for Handling the PC-Relative Offset

#### Option A: Patch Code at Load Time (Recommended for COPY_ALL / COPY_TEXT_DATA)

**How it works:**
1. `mkmodule` records `R_ARM_GOT_PREL` relocations in the relocation table with a new flag
2. At load time, the loader computes the actual runtime PC-relative offset from the code word to the LOT entry
3. The loader patches the code word in RAM

**Pros:**
- Simple and direct
- No changes to the module binary format
- Works for both COPY_ALL and COPY_TEXT_DATA

**Cons:**
- Does NOT work for XIP (code is in flash)
- Requires the loader to know the runtime addresses of both code and LOT
- Increases loader complexity slightly

**For XIP:** We have two sub-options:
- **A1:** Reject modules with R_ARM_GOT_PREL in XIP mode (return error)
- **A2:** Transparently fall back to COPY_TEXT_DATA (copy code to RAM)

#### Option B: Include a .got Section in the Binary and Place it at a Fixed Offset

**How it works:**
1. `mkmodule` reads the `.got` section from the ELF
2. The `.got` section is placed at a fixed offset from the code in the binary image
3. At load time, the loader patches the `.got` entries (not the code) to contain resolved addresses
4. The code's PC-relative offsets remain correct because the code-to-GOT distance is fixed

**Pros:**
- Code is never patched (same as current model)
- Works for XIP if the code and GOT are both in flash

**Cons:**
- Requires reading and preserving the `.got` section in the binary image
- Changes the module binary format
- The GOT in flash is not writable, so we'd need to copy the GOT to RAM anyway
- The `.got` section would be in the binary image, increasing image size
- For XIP, if the GOT is copied to RAM but the code references it with PC-relative offsets, the offsets would be wrong (flash-to-RAM distance)

**Verdict:** This doesn't solve the XIP problem either. If the code references a GOT in RAM with PC-relative offsets, the offsets depend on the flash-to-RAM distance.

#### Option C: Transform R_ARM_GOT_PREL to R_ARM_GOT_BREL at Build Time

**How it works:**
1. `mkmodule` detects `R_ARM_GOT_PREL` relocations
2. Instead of recording them as-is, it patches the code to use the `R_ARM_GOT_BREL` instruction sequence
3. The relocation is transformed into a `R_ARM_GOT_BREL`-style LOT index

**Pros:**
- No loader changes needed
- Works for all load modes including XIP
- Maintains the current architecture

**Cons:**
- **Extremely fragile.** Requires understanding the exact instruction sequence generated by the compiler and replacing it.
- Compiler-specific (different LLVM versions may generate different sequences)
- Risk of mis-patching code, leading to subtle bugs
- Very hard to test comprehensively

**Verdict:** Rejected. Too fragile and compiler-specific.

#### Option D: Require a New Memory Layout for R_ARM_GOT_PREL Modules

**How it works:**
1. For modules with `R_ARM_GOT_PREL`, the LOT is placed immediately after the code (or within the code section)
2. This ensures the code-to-LOT distance is invariant

**Pros:**
- Works for all load modes

**Cons:**
- Changes the load-time memory layout for some modules
- Increases complexity in the loader (different layouts for different relocation types)
- The LOT is in RAM, so if code is in flash, the LOT must also be in flash (or the code must be in RAM)

**Verdict:** Doesn't solve the fundamental problem. If the LOT is in RAM and code is in flash (XIP), the PC-relative offset from flash to RAM is not fixed.

### 2.4 R_ARM_REL32 Specifics

`R_ARM_REL32` is simpler than `R_ARM_GOT_PREL` because it bypasses the GOT:

**For local symbols within the same section:**
- The linker resolves the offset at link time
- The offset is invariant when the section is moved (all offsets are relative)
- **No patching needed at load time**
- Works in all load modes

**For local symbols across sections (e.g., code to data):**
- The linker resolves the offset at link time based on the link-time section layout
- At runtime, if the section layout is different (e.g., XIP), the offset may need adjustment
- **Requires patching at load time for XIP**

**For external symbols:**
- The linker cannot resolve the offset
- Requires a dynamic linker to patch at load time
- In udynlink, we would need to either:
  - Reject such modules
  - Use a trampoline (a small RAM-resident stub that contains the resolved address)

**Assessment:**
- Most uses of `R_ARM_REL32` in PIC code are for **local symbols within the same section** (e.g., local labels, function pointer tables, local data). These are already resolved by the linker and require **no loader changes**.
- Cross-section `R_ARM_REL32` for local symbols is less common but can be handled by patching at load time (similar to R_ARM_GOT_PREL).
- External `R_ARM_REL32` is rare in module code and should be rejected for now.

### 2.5 Recommended Architecture

**For R_ARM_GOT_PREL:**

1. **Build-time (`mkmodule`):**
   - Detect `R_ARM_GOT_PREL` relocations
   - Record them in the relocation table with a new flag (e.g., `symt_offset` bit 29)
   - Do NOT patch the `.text` word at build time (leave the link-time value)

2. **Load-time (`udynlink.c`):**
   - In `udynlink_load_apply_relocations()`, detect the new flag
   - Compute the runtime PC-relative offset from the code word to the LOT entry:
     ```
     runtime_offset = (lot_runtime_addr + lot_entry_offset) - (code_runtime_addr + word_offset)
     ```
   - Patch the code word in RAM with the runtime offset
   - Also resolve the LOT entry to the symbol address (as before)

3. **XIP handling:**
   - If the module contains `R_ARM_GOT_PREL` relocations and XIP mode is requested:
     - **Option A1:** Return `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED` (or a new error)
     - **Option A2:** Silently fall back to `COPY_TEXT_DATA`
   - **Decision needed:** Which approach? (see Decision Point 1)

**For R_ARM_REL32:**

1. **Build-time (`mkmodule`):**
   - Detect `R_ARM_REL32` relocations
   - Distinguish between:
     - **Intra-section** (symbol and relocation site are in the same section): Ignore. The linker has already resolved it correctly.
     - **Inter-section** (symbol and relocation site are in different sections): Record with a new flag and the symbol table index.
     - **External**: Reject with an error.

2. **Load-time (`udynlink.c`):**
   - For inter-section `R_ARM_REL32`, compute the runtime offset:
     ```
     runtime_offset = (sym_runtime_addr) - (code_runtime_addr + word_offset)
     ```
   - Patch the code word in RAM

3. **XIP handling:**
   - Same as R_ARM_GOT_PREL: inter-section R_ARM_REL32 requires code patching

**Key loader change:**

The loader needs to know:
- `code_runtime_addr` — the address where the code is loaded
- `lot_runtime_addr` — the address where the LOT is loaded
- `word_offset` — the offset of the relocated word within the code section

These are already computable from the module state and the relocation table.

---

## 3. Decision Points

### Decision Point 1: XIP Mode for R_ARM_GOT_PREL / R_ARM_REL32

**Option A1:** Reject modules with these relocations in XIP mode with a clear error.
- **Pros:** Explicit, safe, no hidden behavior
- **Cons:** Users cannot use XIP for LLVM-generated modules

**Option A2:** Transparently fall back to COPY_TEXT_DATA (copy code to RAM).
- **Pros:** Seamless user experience; modules just work
- **Cons:** Violates the user's explicit request for XIP; uses more RAM; may surprise users

**Option A3:** Support XIP by adding a "RAM trampoline" layer.
- **Pros:** True XIP support
- **Cons:** Very complex; requires a small RAM-resident table that the code references, which then points to the actual data; fragile and hard to test

**My recommendation:** Option A1 (reject in XIP) with a clear error message. Users can explicitly choose COPY_TEXT_DATA if they want RAM-resident code. Option A2 is too magical. Option A3 is too complex.

### Decision Point 2: Module Binary Format Compatibility

The current relocation table entry format is:

```c
uint32_t lot_offset;    // index into LOT or data section
uint32_t symt_offset;   // symbol table index, or special flag bits
```

Flag bits used:
- Bit 31: `R_ARM_ABS32` data relocation
- Bit 30: `.text` base relocation

We need to add flags for:
- Bit 29: `R_ARM_GOT_PREL` relocation
- Bit 28: `R_ARM_REL32` relocation

**Question:** Is this sufficient? Do we need more bits? Do we need to bump the ABI version?

**My recommendation:** Use bits 29 and 28 for the new relocation types. This does not require bumping the ABI version because the flags are only set by new `mkmodule` versions, and old loaders will reject them as `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE`. However, we should bump the module ABI version to 3.1 to signal that modules using these new relocations require a loader that understands them.

### Decision Point 3: Error Handling Strategy

When the loader encounters a relocation it cannot handle (e.g., `R_ARM_GOT_PREL` in XIP mode), should it:

- **Fail the entire load** with a specific error code?
- **Warn and skip** the relocation (dangerous, code will crash)?

**My recommendation:** Fail the load. Partially relocated code is worse than no code.

### Decision Point 4: Scope of R_ARM_REL32 Support

Should we support:
- **A:** Only intra-section R_ARM_REL32 (no loader changes needed, linker already resolved it)
- **B:** Intra-section + inter-section R_ARM_REL32 (requires loader code patching)
- **C:** Intra-section + inter-section + external R_ARM_REL32 (requires trampolines, very complex)

**My recommendation:** Start with **B**. Intra-section is free (already works). Inter-section requires the same code-patching logic as R_ARM_GOT_PREL. External R_ARM_REL32 is rare and can be deferred.

---

## 4. Complexity and Code Size Estimate

### 4.1 Loader Changes (`udynlink.c`)

**Current relocation loop:** ~90 lines in `udynlink_load_apply_relocations()`

**New code needed:**
- Add flag constants for bit 29 (R_ARM_GOT_PREL) and bit 28 (R_ARM_REL32)
- In `udynlink_load_apply_relocations()`:
  - Detect new flags
  - Compute `code_runtime_addr` and `word_runtime_addr`
  - Compute `lot_runtime_addr` and `lot_entry_addr`
  - Compute runtime PC-relative offset
  - Patch the code word
  - Resolve the LOT entry (for R_ARM_GOT_PREL)
  - Resolve the symbol address (for R_ARM_REL32)
- In `udynlink_load_module_image()`:
  - Check for new relocations in XIP mode and return error
- In `apply_extern_relocations_impl()`:
  - Skip new relocation types (they are handled in the initial load)

**Estimated code size increase:** ~80-120 lines of C
**Estimated complexity:** Medium. The logic is straightforward (compute offsets, patch words), but requires careful handling of address arithmetic and load modes.

### 4.2 Toolchain Changes (`mkmodule`, `udynlink_utils.py`)

**Current relocation processing:** ~120 lines in `mkmodule`

**New code needed:**
- In `get_relocations_in_elf()`: Read `R_ARM_GOT_PREL` and `R_ARM_REL32` relocations
- In `mkmodule`:
  - Classify `R_ARM_GOT_PREL` and `R_ARM_REL32` relocations
  - For `R_ARM_GOT_PREL`: Record in the relocation table with the new flag
  - For `R_ARM_REL32`: Determine if intra-section or inter-section; ignore or record
  - Do NOT patch the `.text` word for these new types (leave the link-time value)

**Estimated code size increase:** ~40-60 lines of Python
**Estimated complexity:** Low. Mostly adding new cases to existing relocation classification logic.

### 4.3 Test Changes

**New tests needed:**
- A test module compiled with `clang` (or a mock) that generates `R_ARM_GOT_PREL`
- A test module compiled with `clang` that generates `R_ARM_REL32`
- Test for all three load modes
- Test for XIP rejection (if Option A1 is chosen)

**Challenge:** Generating `R_ARM_GOT_PREL` from GCC is difficult. We need to use `clang` or manually craft an ELF file. Alternatively, we can create a hand-written assembly test module that contains the `R_ARM_GOT_PREL` relocation.

**Estimated effort:** Medium. Requires setting up a `clang` cross-compiler or hand-crafting assembly.

### 4.4 Documentation Changes

**Updates needed:**
- `docs/how-it-works.md`: Add section on R_ARM_GOT_PREL and R_ARM_REL32
- `docs/api-reference.md`: Document new error codes
- `docs/writing-modules.md`: Add note about LLVM toolchains
- `README.md`: Update supported relocation types

---

## 5. Proposed Task Breakdown

### Phase 1: Validation & Reproduction (Investigation)

**Goal:** Confirm that LLVM generates R_ARM_GOT_PREL / R_ARM_REL32 for udynlink-style modules, and understand the exact code sequences.

**Task 1.1:** Compile a simple C module with `clang` (targeting `thumbv7m-none-eabi`) using `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative` and examine the generated relocations.
- **Deliverable:** A report listing the exact relocation types generated by `clang` for a simple module that accesses global data and calls external functions.
- **Agent:** `search` (fast, can run shell commands and grep)

**Task 1.2:** Create a minimal assembly test module that uses `R_ARM_GOT_PREL` and `R_ARM_REL32` to verify the loader behavior.
- **Deliverable:** A hand-crafted assembly file and a Python script that creates a valid udynlink module with these relocations.
- **Agent:** `agent` (can write multi-file code)

### Phase 2: Core Loader Implementation

**Goal:** Add support for R_ARM_GOT_PREL and R_ARM_REL32 in the loader.

**Task 2.1:** Add new relocation flag constants to `udynlink.h` and `udynlink.c`.
- **Deliverable:** Updated header and source with constants for bit 29 and bit 28.
- **Agent:** `quick`

**Task 2.2:** Implement `udynlink_load_apply_relocations()` changes for R_ARM_GOT_PREL.
- **Deliverable:** Loader code that detects R_ARM_GOT_PREL, computes runtime offsets, and patches code in RAM.
- **Agent:** `agent`

**Task 2.3:** Implement `udynlink_load_apply_relocations()` changes for R_ARM_REL32.
- **Deliverable:** Loader code that handles intra-section (no-op) and inter-section (patch) R_ARM_REL32.
- **Agent:** `agent`

**Task 2.4:** Add XIP rejection logic for modules with R_ARM_GOT_PREL / inter-section R_ARM_REL32.
- **Deliverable:** Updated `udynlink_load_module_image()` that checks relocation types against load mode.
- **Agent:** `quick`

**Task 2.5:** Update `apply_extern_relocations_impl()` to skip new relocation types.
- **Deliverable:** Updated function that correctly skips new flags during incremental relinking.
- **Agent:** `quick`

### Phase 3: Toolchain Implementation

**Goal:** Update `mkmodule` to detect, classify, and emit the new relocation types.

**Task 3.1:** Update `udynlink_utils.py` to read `R_ARM_GOT_PREL` and `R_ARM_REL32` from ELF.
- **Deliverable:** Updated `get_relocations_in_elf()` that returns these types.
- **Agent:** `quick`

**Task 3.2:** Update `mkmodule` to classify and record new relocation types.
- **Deliverable:** Updated `process()` function that handles new relocation types correctly.
- **Agent:** `agent`

**Task 3.3:** Update `mkmodule` to NOT patch `.text` words for R_ARM_GOT_PREL and R_ARM_REL32.
- **Deliverable:** Updated `process()` that leaves the link-time value in the code.
- **Agent:** `quick`

### Phase 4: Testing

**Goal:** Add integration tests that validate the new relocation types.

**Task 4.1:** Create a test module that generates R_ARM_GOT_PREL (using clang or hand-crafted assembly).
- **Deliverable:** A `test-rgotprel/` directory with module source and test harness.
- **Agent:** `agent`

**Task 4.2:** Create a test module that generates R_ARM_REL32.
- **Deliverable:** A `test-rrel32/` directory with module source and test harness.
- **Agent:** `agent`

**Task 4.3:** Run the full test suite to verify no regressions.
- **Deliverable:** Test output showing all existing tests pass.
- **Agent:** `shell` (run `just test-f429` or similar)

### Phase 5: Documentation

**Goal:** Update all documentation to reflect the new capabilities.

**Task 5.1:** Update `docs/how-it-works.md` with new relocation types.
- **Deliverable:** Updated documentation with formulas and code sequences.
- **Agent:** `quick`

**Task 5.2:** Update `docs/api-reference.md` with new error codes.
- **Deliverable:** Updated API reference.
- **Agent:** `quick`

**Task 5.3:** Update `README.md` and `AGENTS.md`.
- **Deliverable:** Updated project-level documentation.
- **Agent:** `quick`

---

## 6. Open Questions for the User

Before proceeding, we need the user to weigh in on the following:

### Q1: XIP Mode Behavior

> **How should the loader behave when an XIP load is requested for a module containing R_ARM_GOT_PREL or inter-section R_ARM_REL32?**
>
> - **Option A (Recommended):** Return a clear error (`UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED`). The user must explicitly choose `COPY_TEXT_DATA` or `COPY_ALL`.
> - **Option B:** Silently fall back to `COPY_TEXT_DATA` (copy code to RAM). This uses more RAM but "just works."
> - **Option C:** Investigate a "RAM trampoline" approach where the code in flash references a small RAM table that contains the correct offsets. This is complex but would enable true XIP.

### Q2: ABI Version

> **Should we bump the module ABI version to 3.1 to signal that modules using R_ARM_GOT_PREL require a loader that understands them?**
>
> - **Option A (Recommended):** Yes, bump to 3.1. New modules set `udynlink_version = 3.1`. Old 3.0 loaders will reject them with `UDYNLINK_ERR_LOAD_VERSION_MISMATCH`. This is the safest approach.
> - **Option B:** No, keep 3.0. The loader will detect the unknown flag bits and return `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE`. This also works but is less explicit.

### Q3: R_ARM_REL32 Scope

> **What level of R_ARM_REL32 support do we need?**
>
> - **Option A (Recommended):** Support intra-section (free, no-op) and inter-section (requires code patching). Reject external R_ARM_REL32.
> - **Option B:** Support intra-section only. Inter-section R_ARM_REL32 is rejected. Simpler but less flexible.
> - **Option C:** Full support including external R_ARM_REL32. This requires trampolines and is significantly more complex.

### Q4: Toolchain Priority

> **Which LLVM toolchain should we target for testing?**
>
> - **Option A (Recommended):** `clang` with `arm-none-eabi` target. We need to know if the user has `clang` available in their environment or if we should rely on hand-crafted assembly tests.
> - **Option B:** Rust `thumbv7em-none-eabi` target. We can create a Rust test module if the user has a Rust cross-compiler.
> - **Option C:** Zig cross-compiler. Similar to Rust.

### Q5: LOT Placement

> **Should we consider placing the LOT at a fixed offset from the code (instead of at the start of RAM) for modules with R_ARM_GOT_PREL?**
>
> - **Option A (Recommended):** No. Keep the LOT at the start of RAM. Patch the code at load time for COPY_ALL/COPY_TEXT_DATA. This is simpler and maintains consistency.
> - **Option B:** Yes, investigate placing the LOT immediately after the code. This would make the code-to-LOT distance invariant. But it changes the load-time layout and may complicate the loader.

---

## 7. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| `clang` generates different code sequences than expected | High | High | Use hand-crafted assembly tests; do not rely on compiler-specific sequences |
| Code patching in loader is fragile (e.g., pointer arithmetic errors) | Medium | High | Extensive testing with hand-crafted modules; fuzz the offset computation |
| XIP modules from LLVM toolchains become unusable | High (if A1) | Medium | Document clearly; users can use COPY_TEXT_DATA |
| Binary format incompatibility with old loaders | Low | Medium | Bump ABI version; old loaders reject gracefully |
| External R_ARM_REL32 is needed later | Low | High | Design the architecture to allow adding trampolines later |

---

## 8. Summary of Expected Changes

| Component | Files | Lines Added | Complexity |
|-----------|-------|-------------|------------|
| Loader core | `udynlink/udynlink.c`, `udynlink/udynlink.h` | ~120 | Medium |
| Toolchain | `scripts/mkmodule`, `scripts/udynlink_utils.py` | ~60 | Low |
| Tests | `tests/test-rgotprel/*`, `tests/test-rrel32/*` | ~200 | Medium |
| Documentation | `docs/*.md`, `README.md` | ~100 | Low |
| **Total** | | **~480 lines** | |

---

## 9. Next Steps

1. **User Review:** The user reviews this plan and answers the 5 Decision Questions.
2. **Refinement:** Update the plan based on user feedback.
3. **Execution:** Proceed with Phase 1 (Validation & Reproduction) using the appropriate agents.
4. **Iterative Review:** After each phase, review results and adjust subsequent phases.

---

*Plan version: 1.0*
*Date: 2026-05-28*
