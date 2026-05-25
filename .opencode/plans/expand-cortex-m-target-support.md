# udynlink: Expand Target Support & MCU Configurability

## Goal

Make `udynlink` work across the entire ARM Cortex-M family (M0, M0+, M3, M4, M4F, M7, M33, M55, M85) with **compile-time, zero-cost** memory/arch/platform configuration. Add ABI versioning and architecture tags to the module header for compatibility checking. Expand test coverage to at least one processor per M-series (mainly STM32, plus MPS2/MPS3 for M7/M55/M85).

## Constraints & Decisions (Locked)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **LOT base / memory config** | Compile-time macros only | Zero runtime overhead. Host firmware is rebuilt per target. |
| **Target scope** | M0/M0+, M3, M4/M4F, M7, M33, M55, M85 | Maximum coverage with available GCC/binutils and QEMU. |
| **Float ABI** | Supported (soft, softfp, hard) per target | Architecture tag encodes float ABI so runtime can reject mismatches. |
| **ABI versioning** | Mandatory | Module header gets `mod_version`, `udynlink_version`, and `arch_tag`. Runtime validates all three. |
| **Test strategy** | Phase 0–2 use existing STM32F429 host. Phase 3 adds per-target QEMU hosts. | Keeps early phases small and testable without massive infrastructure churn. |

## Current State Summary

`udynlink` is currently locked to **STM32F429I-Discovery / Cortex-M4** assumptions:

| Layer | Hardcoded Value | Files |
|-------|-----------------|-------|
| **Runtime** | LOT base `0x20000000` (implicit via host write) | `tests/qemu_host/src/test_utils.c` |
| **Toolchain** | `-mcpu=cortex-m4 -mthumb`, `.arch armv7-m` | `scripts/mkmodule`, `scripts/asm_template.tmpl` |
| **Test host** | STM32F4 HAL, `mem.ld` (192K RAM @ 0x20000000) | `cmake/platforms/stm32f429_discovery.cmake`, `tests/qemu_host/` |
| **Module ABI** | No version or architecture checking | `udynlink/udynlink.h` (fields commented out) |
| **Max handles** | Silent default of 1 | `udynlink/udynlink.c` |

## Architecture Overview

### Compile-Time Configuration Interface

All platform-specific constants are supplied via **C macros** that the user defines before including `udynlink.h`, or via a user-provided `udynlink_config.h`.

```c
/* udynlink.h (excerpt) */
#ifndef UDYNLINK_LOT_BASE_ADDR
#define UDYNLINK_LOT_BASE_ADDR 0x20000000
#endif

#ifndef UDYNLINK_MAX_HANDLES
#error "UDYNLINK_MAX_HANDLES must be defined before including udynlink.h"
#endif
```

The assembly template receives `{{lot_base}}` as a hex literal.

### Target Database (`scripts/targets.py`)

A single source of truth mapping target names to compiler/assembler/runtime metadata:

| Target | `mcpu` | `.arch` | FPU | float_abi | `arch_tag` |
|--------|--------|---------|-----|-----------|------------|
| `cortex-m0` | `cortex-m0` | `armv6-m` | — | `soft` | `0x01` |
| `cortex-m0plus` | `cortex-m0plus` | `armv6-m` | — | `soft` | `0x02` |
| `cortex-m3` | `cortex-m3` | `armv7-m` | — | `soft` | `0x03` |
| `cortex-m4` | `cortex-m4` | `armv7e-m` | — | `soft` | `0x04` |
| `cortex-m4f` | `cortex-m4` | `armv7e-m` | `fpv4-sp-d16` | `hard` | `0x14` |
| `cortex-m7` | `cortex-m7` | `armv7e-m` | `fpv5-d16` | `hard` | `0x17` |
| `cortex-m33` | `cortex-m33` | `armv8-m.main` | — | `soft` | `0x08` |
| `cortex-m55` | `cortex-m55` | `armv8.1-m.main` | `auto` | `hard` | `0x19` |
| `cortex-m85` | `cortex-m85` | `armv8.1-m.main` | `auto` | `hard` | `0x1A` |

`arch_tag` layout (uint16_t):
- Bits `[3:0]` — core family ID
- Bit `4` — FPU present
- Bits `[6:5]` — float ABI (`00`=soft, `01`=softfp, `10`=hard)
- Bits `[15:7]` — reserved

### Assembly Prologue Templates

The current prologue uses `push {r9, lr}`, which is **invalid on Cortex-M0/M0+** because `r9` is a high register and Thumb-1 `PUSH` only supports R0–R7 + LR.

We need **three template families**:
1. **`asm_template_armv6m.tmpl`** — M0/M0+. Saves `r9` via `str` to stack using low-register indirection.
2. **`asm_template_armv7m.tmpl`** — M3/M4/M7. Current prologue (or refined). Uses `.arch armv7-m` / `armv7e-m`.
3. **`asm_template_armv8m.tmpl`** — M33/M55/M85. Same prologue as v7m, but `.arch armv8-m.main` / `armv8.1-m.main`.

### Module Header Changes

```c
typedef struct {
    uint32_t sign;               // 'UDLM'
    uint16_t mod_version;        // Module ABI version (was commented out)
    uint16_t udynlink_version;   // Loader ABI version (was commented out)
    uint16_t arch_tag;           // NEW: target architecture + float ABI
    uint16_t num_lot;
    uint16_t num_rels;
    uint32_t symt_size;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t bss_size;
} udynlink_module_header_t;
```

Runtime checks on `udynlink_load_module`:
- `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` if `udynlink_version` > loader's supported version.
- `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` if `arch_tag` core family or FPU/ABI mismatch.

### Test Host Strategy

| Phase | Target | QEMU Machine / Board | QEMU Binary |
|-------|--------|--------------------|-------------|
| 0–2 | STM32F429 (M4) | `STM32F429I-Discovery` | `qemu-system-gnuarmeclipse` |
| 3a | STM32F103 (M3) | `NUCLEO-F103RB` or `STM32-H103` | `qemu-system-gnuarmeclipse` |
| 3b | STM32F051 (M0) | `STM32F0-Discovery` | `qemu-system-gnuarmeclipse` |
| 3c | STM32F407 (M4F hard-float) | `STM32F4-Discovery` | `qemu-system-gnuarmeclipse` |
| 3d | MPS2-AN500 (M7) | `mps2-an500` | `qemu-system-arm` |
| 3e | MPS3-AN547 (M55) | `mps3-an547` | `qemu-system-arm` |
| 3f | MPS3-AN552 (M85) | `mps3-an552` | `qemu-system-arm` |

For each new host we need:
- `tests/platforms/<name>/platform.cmake`
- `tests/platforms/<name>/mem.ld` (RAM/Flash origin & size)
- Minimal startup + vector table (generic CMSIS-style, not full HAL)
- `tests/platforms/<name>/system_init.c` (clock setup, or nop if QEMU tolerant)

The existing STM32F4 host is **migrated** into `tests/platforms/stm32f429_discovery/` so the top-level `tests/qemu_host/CMakeLists.txt` becomes a thin dispatcher.

---

## Phase Breakdown

### Phase 0: ABI Foundation & Cleanups
**Goal:** Fix existing known issues and lay the ABI versioning groundwork. End with all STM32F429 tests passing.

| # | Task | Files | Agent | Deliverable / Test |
|---|------|-------|-------|--------------------|
| 0a | Fix `UDYNLINK_MAKE_VERSION` shift mismatch (`<<8` vs `>>16`) | `udynlink/udynlink.h` | `quick` | Macro round-trips correctly |
| 0b | Fix typos in `udynlink.h` | `udynlink/udynlink.h` | `quick` | "ownershsip" → "ownership", etc. |
| 0c | Remove unused `#include <stdio.h>` from `udynlink.c` | `udynlink/udynlink.c` | `quick` | Compiles without it |
| 0d | Make `UDYNLINK_MAX_HANDLES` a required compile-time constant | `udynlink/udynlink.h`, `udynlink/udynlink.c` | `agent` | Build fails cleanly if unset; test host defines it |
| 0e | Design & add architecture tag constants to C header | `udynlink/udynlink.h` | `agent` | C enum / `#define` set |
| 0f | Mirror architecture constants in Python | `scripts/targets.py` (new) | `agent` | Python dict matches C values |
| 0g | Uncomment `mod_version` / `udynlink_version` in header | `udynlink/udynlink.h` | `quick` | Struct size changes; update size checks |
| 0h | Update `mkmodule` to embed version fields and arch tag | `scripts/mkmodule`, `scripts/targets.py` | `agent` | Generated modules have valid header |
| 0i | Update runtime to validate version & arch on load | `udynlink/udynlink.c` | `agent` | New error codes; rejects bad modules |
| 0j | Update test host to define `UDYNLINK_MAX_HANDLES` | `tests/qemu_host/src/main.c` or CMake | `quick` | |
| 0k | Run full test suite | `tests/` | `shell` | All STM32F429 tests pass |

**Review gate:** `python test_driver.py` passes with zero failures. No new warnings.

---

### Phase 1: Compile-Time LOT Base & Memory Config
**Goal:** Make RAM/LOT base and max handles compile-time configurable. End with all STM32F429 tests passing.

| # | Task | Files | Agent | Deliverable / Test |
|---|------|-------|-------|--------------------|
| 1a | Add `UDYNLINK_LOT_BASE_ADDR` macro with default `0x20000000` | `udynlink/udynlink.h` | `quick` | Macro present, zero-cost |
| 1b | Update `udynlink_cpp_init` to use macro instead of literal | `udynlink/udynlink.c` | `quick` | Verify no literal `0x20000000` remains in core |
| 1c | Update assembly template to accept `{{lot_base}}` | `scripts/asm_template.tmpl`, `scripts/mkmodule` | `agent` | `--lot-base` CLI arg or auto from `--target` |
| 1d | Update test utilities to use `UDYNLINK_LOT_BASE_ADDR` | `tests/qemu_host/src/test_utils.c` | `quick` | No hardcoded address |
| 1e | Run full test suite | `tests/` | `shell` | All tests pass |

**Review gate:** Same as Phase 0.

---

### Phase 2: Toolchain Target Selection
**Goal:** `mkmodule` can generate correct modules for every supported Cortex-M core. End with compile-and-link validation for each target (QEMU tests still use default M4 host).

| # | Task | Files | Agent | Deliverable / Test |
|---|------|-------|-------|--------------------|
| 2a | Create `scripts/targets.py` with full target database | `scripts/targets.py` | `agent` | 9 entries, validated against GCC docs |
| 2b | Update `mkmodule` to use target database | `scripts/mkmodule` | `agent` | `--target <name>` flag; validates name |
| 2c | Generate correct compiler flags per target | `scripts/mkmodule` | `agent` | `-mcpu`, `-mfpu`, `-mfloat-abi` substituted |
| 2d | Generate correct assembler `.arch` per target | `scripts/mkmodule`, templates | `agent` | Select template family based on target arch |
| 2e | Write `asm_template_armv6m.tmpl` (M0-compatible) | `scripts/asm_template_armv6m.tmpl` | `agent` | No `push {r9, lr}`; uses low-register stack ops |
| 2f | Write `asm_template_armv7m.tmpl` (M3/M4/M7) | `scripts/asm_template_armv7m.tmpl` | `agent` | Current prologue, `.arch armv7-m` / `armv7e-m` |
| 2g | Write `asm_template_armv8m.tmpl` (M33/M55/M85) | `scripts/asm_template_armv8m.tmpl` | `agent` | `.arch armv8-m.main` / `armv8.1-m.main` |
| 2h | Embed architecture tag in module header | `scripts/mkmodule` | `agent` | `arch_tag` written correctly |
| 2i | Update runtime arch-check logic for all families | `udynlink/udynlink.c` | `agent` | Rejects core-family mismatch and float-ABI mismatch |
| 2j | Add `--target` support to `test_driver.py` for module compilation | `tests/test_driver.py` | `agent` | Can compile modules for any target (even if host is M4) |
| 2k | Validation: compile a "hello world" module for every target | `tests/` | `shell` | `mkmodule --target <name>` succeeds for all 9 targets |

**Review gate:** All 9 targets can compile `tests/test-helloworld/hello.c` into a valid module image. STM32F429 QEMU tests still pass.

---

### Phase 3: Multi-Target QEMU Test Hosts
**Goal:** Test the runtime and modules on actual QEMU emulations for each target family. This is the largest phase.

#### 3a: Refactor existing host into platform directory
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3a.1 | Create `tests/platforms/` layout | new dirs | `agent` | `tests/platforms/stm32f429_discovery/` with all current files |
| 3a.2 | Rewrite `tests/qemu_host/CMakeLists.txt` as dispatcher | `tests/qemu_host/CMakeLists.txt` | `agent` | `UDYNLINK_PLATFORM` selects subdirectory |
| 3a.3 | Migrate STM32F4 HAL, linker scripts, startup | moved files | `agent` | Build succeeds, all current tests pass |

#### 3b: Add STM32F103 (Cortex-M3) host
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3b.1 | Create `tests/platforms/stm32f103_bluepill/` | new files | `agent` | `platform.cmake`, `mem.ld` (20K RAM @ 0x20000000, 128K Flash @ 0x08000000), minimal startup |
| 3b.2 | Write generic CMSIS-style vector table + startup | `tests/platforms/stm32f103_bluepill/startup_stm32f103.s` | `agent` | Resets to main, sets SP |
| 3b.3 | Write minimal system init (or nop) | `tests/platforms/stm32f103_bluepill/system_init.c` | `agent` | QEMU tolerant |
| 3b.4 | Update `test_driver.py` to build/run M3 tests | `tests/test_driver.py` | `agent` | Uses `qemu-system-gnuarmeclipse -board NUCLEO-F103RB` |
| 3b.5 | Run all tests on M3 host | `tests/` | `shell` | Pass |

#### 3c: Add STM32F051 (Cortex-M0) host
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3c.1 | Create `tests/platforms/stm32f051_discovery/` | new files | `agent` | `platform.cmake`, `mem.ld` (8K RAM @ 0x20000000, 64K Flash @ 0x08000000) |
| 3c.2 | Write M0-compatible startup | `tests/platforms/stm32f051_discovery/startup.s` | `agent` | Uses Thumb-1 only instructions |
| 3c.3 | Update `test_driver.py` for M0 | `tests/test_driver.py` | `agent` | Uses `qemu-system-gnuarmeclipse -board STM32F0-Discovery` |
| 3c.4 | Run all tests on M0 host | `tests/` | `shell` | Pass |

#### 3d: Add STM32F407 (Cortex-M4F hard-float) host
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3d.1 | Create `tests/platforms/stm32f407_discovery/` | new files | `agent` | `platform.cmake` with `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| 3d.2 | `mem.ld` (128K RAM @ 0x20000000, 1M Flash @ 0x08000000) | `tests/platforms/stm32f407_discovery/mem.ld` | `agent` | |
| 3d.3 | Minimal startup (can reuse generic M4) | `tests/platforms/stm32f407_discovery/startup.s` | `agent` | |
| 3d.4 | Update `test_driver.py` for M4F | `tests/test_driver.py` | `agent` | Uses `qemu-system-gnuarmeclipse -board STM32F4-Discovery` |
| 3d.5 | Run all tests on M4F host | `tests/` | `shell` | Pass |

#### 3e: Add MPS2-AN500 (Cortex-M7) host
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3e.1 | Create `tests/platforms/mps2_an500/` | new files | `agent` | `platform.cmake`, `mem.ld` (AN500 has 8MB SRAM @ 0x60000000? Actually AN500 has 8MB ZBTSRAM @ 0x60000000 and 4MB BRAM @ 0x20000000. Need to verify.) |
| 3e.2 | Write startup for AN500 | `tests/platforms/mps2_an500/startup.s` | `agent` | SP from vector table, Reset_Handler |
| 3e.3 | Update `test_driver.py` for M7 (mainline QEMU) | `tests/test_driver.py` | `agent` | Uses `qemu-system-arm -machine mps2-an500` |
| 3e.4 | Run all tests on M7 host | `tests/` | `shell` | Pass |

#### 3f: Add MPS3-AN547 (Cortex-M55) host
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3f.1 | Create `tests/platforms/mps3_an547/` | new files | `agent` | `platform.cmake`, `mem.ld` (AN547 memory map: 8MB SSRAM @ 0x60000000, 4MB BRAM @ 0x20000000, etc.) |
| 3f.2 | Write startup for AN547 (ARMv8.1-M) | `tests/platforms/mps3_an547/startup.s` | `agent` | TrustZone-aware? No, use non-secure startup. |
| 3f.3 | Update `test_driver.py` for M55 | `tests/test_driver.py` | `agent` | Uses `qemu-system-arm -machine mps3-an547` |
| 3f.4 | Run all tests on M55 host | `tests/` | `shell` | Pass |

#### 3g: Add MPS3-AN552 (Cortex-M85) host
| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 3g.1 | Create `tests/platforms/mps3_an552/` | new files | `agent` | `platform.cmake`, `mem.ld` |
| 3g.2 | Write startup for AN552 | `tests/platforms/mps3_an552/startup.s` | `agent` | |
| 3g.3 | Update `test_driver.py` for M85 | `tests/test_driver.py` | `agent` | Uses `qemu-system-arm -machine mps3-an552` |
| 3g.4 | Run all tests on M85 host | `tests/` | `shell` | Pass |

**Review gate:** `test_driver.py` (or a new wrapper) passes on every supported target platform.

---

### Phase 4: Documentation, CI, and Polish
**Goal:** Document the new capabilities and ensure CI covers all testable targets.

| # | Task | Files | Agent | Deliverable |
|---|------|-------|-------|-------------|
| 4a | Update README with target configuration guide | `README.md` | `quick` | `--target`, LOT base setup, architecture tag explanation |
| 4b | Update AGENTS.md with new constraints and build commands | `AGENTS.md` | `quick` | |
| 4c | Add target compatibility matrix | `README.md` or `docs/TARGETS.md` | `quick` | Table of supported targets, QEMU status, float ABI |
| 4d | Update CI workflow to install both QEMU variants | `.github/workflows/ci.yml` | `agent` | `qemu-system-gnuarmeclipse` + `qemu-system-arm` |
| 4e | Update CI to run tests for all targets | `.github/workflows/ci.yml` | `agent` | Matrix or sequential runs for each platform |
| 4f | Add `cmake/platforms/` documentation | `cmake/platforms/README.md` | `quick` | How to add a new platform |

**Review gate:** CI passes green. README is accurate.

---

## Sub-Planning Documents

Because Phase 3 (multi-target hosts) is large, its detailed step-by-step implementation plan lives in:

- `.opencode/plans/phase3-multi-target-hosts.md`

The main plan above references it; future orchestrator agents should read the sub-plan before assigning Phase 3 work.

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| M0 assembly prologue is incorrect | Medium | High | Validate with QEMU M0 host in Phase 3c before declaring Phase 2 done. |
| `qemu-system-gnuarmeclipse` missing in newer distros | Medium | High | CI pins xPack release. Long-term: migrate M0/M3/M4 tests to mainline QEMU `stm32` machines (Phase 4 stretch goal). |
| AN500/AN547/AN552 memory maps differ from docs | Medium | Medium | Verify with NuttX/Zephyr linker scripts as reference during Phase 3e–3g. |
| Module header size increase breaks existing consumers | Low | High | Keep header 32-bit aligned. Document that this is a **breaking ABI change** requiring host firmware rebuild. |
| Hard-float module on soft-float host crashes | Medium | High | Runtime arch tag checks float ABI; rejected at load time with clear error. |
| `UDYNLINK_MAX_HANDLES` change breaks downstream projects | Medium | Medium | Document in README: "Previously defaulted to 1; now must be explicitly defined." |

---

## Status

- **Plan approved by user on 2026-05-25.**
- Phase 0 is ready for execution.

## Next Steps

1. **Phase 0 execution** can begin immediately (no external dependencies).
2. After Phase 0 review gate passes, proceed to Phase 1, then Phase 2, then Phase 3.
3. For Phase 3 detailed sub-tasks, see `.opencode/plans/phase3-multi-target-hosts.md` (to be created when Phase 3 planning begins).
