# Dmod — Portable Features for udynlink

Source: https://github.com/choco-technologies/dmod  
Analyzed: /tmp/dmod

## Feature 1: Automated IRQ Handler Registration & Routing

Modules define IRQ handlers via `DMOD_IRQ_HANDLER(IRQ_NUMBER)`. At load time,
`Dmod_Irq_RegisterModule()` (`src/system/public/dmod_irq.c`) automatically
extracts these and registers them to a central interrupt routing table.

**udynlink currently**: Hardware interrupts are entirely the host's problem.
Modules that handle interrupts require tedious manual hook wiring via
`udynlink_external_resolve_symbol`.

**Integration plan** (no-heap):
1. mkmodule: scan for symbols matching `udynlink_irq_N`, embed `{irq_num, func_offset}`
   array in `UDLM` header
2. Add host hooks to `udynlink_externals.h`:
   `udynlink_external_register_irq(int irq, void* handler)` /
   `udynlink_external_unregister_irq(int irq)`
3. Loader iterates IRQ table after relocation, calls host registration hook
4. Host firmware can implement via RAM-based VTOR (Vector Table Offset Register)

**Complexity**: Medium  
**Trade-off**: Host needs ~400 bytes static RAM for relocated vector table

## Feature 2: DIF (Dynamic Interface Discovery / Plugin Pattern)

dmod's "1-to-N" interface: modules declare signatures, multiple implementors can
be discovered via `Dmod_GetNextDifModule()` (`src/system/private/dmod_rmod.c`).

**udynlink currently**: Direct symbol linking only. No way to ask "give me all
modules that implement a filesystem."

**Integration plan** (no-heap):
1. Add API to `udynlink.c`:
   `udynlink_module_t* udynlink_find_by_symbol(udynlink_module_t* prev, const char* sym_name)`
2. Iterates static `module_table[]`, checking if each module exports the symbol
3. Modules export a struct of function pointers (e.g., `__dif_vfs_driver`)

**Complexity**: Low

## Feature 3: FastLZ Payload Decompression (for COPY_ALL / COPY_CODE)

dmod supports `.dmfc` (compressed DMF) files, using FastLZ to decompress
before relocation (`src/system/dmod_system.c:489`).

**udynlink currently**: Uncompressed binaries. For constrained flash or OTA
transfer (LoRa/BLE), modules can be large.

**Integration plan** (no-heap):
1. mkmodule: add `--compress` flag, compress output with FastLZ, prefix with
   `UDLZ` magic + original size
2. Loader: if module starts with `UDLZ`, decompress into the user-provided
   `ram_base` buffer before proceeding with standard relocation
3. FastLZ is bare-metal friendly, no heap, ~1KB ROM for decompressor
4. XIP mode: not applicable (code must be uncompressed in flash)

**Complexity**: Medium  
**Trade-off**: ~1KB ROM overhead for decompressor

## Feature 4: Standardized Module State Machine & Lifecycle Hooks

dmod's binary header includes function pointers for `Preinit`, `Init`, `Main`,
`Deinit`, and `Signal` (`inc/dmod_types.h:122`). Loader controls module state
(Loaded → Enabled → Running).

**udynlink currently**: Only C++ `__init_array` (global constructors). No
standardized init/deinit for C modules. Unloading leaves hanging state.

**Integration plan** (no-heap):
1. mkmodule: identify `udynlink_module_init` / `udynlink_module_deinit` symbols,
   write offsets into `UDLM` header
2. Loader: expose `udynlink_start_module(mod)` → calls init hook
3. Loader: expose `udynlink_stop_module(mod)` → calls deinit hook
4. Add `state` enum (UNLOADED, LOADED, RUNNING) to module struct
5. Refuse unload unless state is STOPPED

**Complexity**: Low  
**Trade-off**: ~8-12 bytes added to UDLM header

## Feature 5: API Versioning / ABI Integrity Checks

dmod embeds version strings in API signatures (e.g., `my_api@1.0`).
Incompatible versions cause loader refusal (`Dmod_ApiSignature_AreCompatible`).

**udynlink currently**: Raw string matching in `udynlink_external_resolve_symbol`.
If host updates a struct layout, old modules load but silently corrupt memory.

**Integration plan** (no-heap):
- Option A: mkmodule appends version suffix to critical symbols automatically
  (e.g., `host_uart_send` → `host_uart_send_v2`)
- Option B: Generate 32-bit CRC of public headers during compilation, embed in
  `UDLM` header. Host passes its own CRC at load time. Mismatch → refuse load.

**Complexity**: Medium  
**Trade-off**: Option A increases symbol table; Option B is cheaper but requires
build system integration for header hashing
