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
| **Test strategy** | **Hybrid QEMU strategy** (see below) | Baseline on xPack fork; new platforms on mainline QEMU. |

## QEMU Strategy (Updated 2026-05-25)

After extensive testing of xPack QEMU 7.2.5 vs mainline QEMU 9.2.4, we are adopting a **hybrid approach**:

| Platform | QEMU Binary | Machine | Status |
|----------|-------------|---------|--------|
| **STM32F429 (M4)** | `qemu-system-gnuarmeclipse` (xPack fork) | `STM32F429I-Discovery` | ✅ Baseline / regression host. All 22 tests pass. |
| **MPS2-AN386 (M4)** | `qemu-system-arm` (mainline) | `mps2-an386` | 🔄 In progress. Self-contained startup, custom semihosting. |
| **MPS2-AN500 (M7)** | `qemu-system-arm` (mainline) | `mps2-an500` | 🔄 Planned. Large SRAM at 0x60000000. |
| **micro:bit (M0)** | `qemu-system-arm` (mainline) | `microbit` | 🔄 Planned. Cortex-M0, 16K RAM. |
| **STM32VLDiscovery (M3)** | `qemu-system-arm` (mainline) | `stm32vldiscovery` | 🔄 Planned. STM32F100, 8K RAM. |
| **Olimex STM32-H405 (M4F)** | `qemu-system-arm` (mainline) | `olimex-stm32-h405` | 🔄 Planned. Hard-float validation. |
| **MPS2-AN505 (M33)** | `qemu-system-arm` (mainline) | `mps2-an505` | 📋 Future. |
| **MPS3-AN547 (M55)** | `qemu-system-arm` (mainline) | `mps3-an547` | 📋 Future. |
| **MPS3-AN552 (M85)** | `qemu-system-arm` (mainline) | `mps3-an552` | 📋 Future. |

### Why the split?

1. **xPack fork (`qemu-system-gnuarmeclipse`)** has excellent STM32 peripheral emulation (RCC, GPIO, USART, etc.) and our existing HAL-based test host works out of the box. However, it has an **undocumented bug on STM32F103/STM32F051**: calling Flash-resident host functions from RAM-loaded modules hangs. This blocks M3/M0 testing on xPack.

2. **Mainline QEMU (`qemu-system-arm`)** supports more Cortex-M machines (MPS2, MPS3, micro:bit, lm3s) but has a **semihosting limitation**: `SYS_WRITE` (buffered I/O, used by newlib `printf`) returns all bytes as "unwritten", so stdout is silently swallowed. `SYS_WRITE0` (null-terminated string) and `SYS_WRITEC` (single char) work fine. This requires a **custom semihosting syscall layer** that uses `SYS_WRITE0` for string output.

3. **Hybrid is pragmatic**: Keep the proven F429 host on xPack for fast regression testing. Build new, self-contained test hosts on mainline QEMU for all other targets. Each mainline host uses:
   - Generic CMSIS-style vector table (no HAL)
   - Custom semihosting layer (`SYS_WRITE0`/`SYS_WRITEC` for output, `SYS_EXIT` to terminate)
   - Self-contained linker scripts matching the machine's memory map

## Architecture Overview

### Compile-Time Configuration Interface

All platform-specific constants are supplied via **C macros** that the user defines before including `udynlink.h`, or via a user-provided `udynlink_config.h`.

```c
/* udynlink.h (excerpt) */
#ifndef UDYNLINK_LOT_BASE_ADDR
#define UDYNLINK_LOT_BASE_ADDR 0x20000000
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

Three template families:
1. **`asm_template_armv6m.tmpl`** — M0/M0+. Low-register stack ops.
2. **`asm_template_armv7m.tmpl`** — M3/M4/M7. Uses `.arch armv7-m` / `armv7e-m`.
3. **`asm_template_armv8m.tmpl`** — M33/M55/M85. Uses `.arch armv8-m.main` / `armv8.1-m.main`.

### Module Header

```c
typedef struct {
    uint32_t sign;               // 'UDLM'
    uint16_t mod_version;        // Module ABI version
    uint16_t udynlink_version;   // Loader ABI version
    uint16_t arch_tag;           // Target architecture + float ABI
    uint16_t num_lot;
    uint16_t num_rels;
    uint32_t symt_size;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t bss_size;
} udynlink_module_header_t;
```

Runtime checks:
- `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` if `udynlink_version` > loader's supported version.
- `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` if `arch_tag` core family or FPU/ABI mismatch.

## Phase Breakdown

### Phase 0: ABI Foundation & Cleanups ✅
**Status:** Complete. All 22 STM32F429 tests pass.

### Phase 1: Compile-Time LOT Base & Memory Config ✅
**Status:** Complete. `UDYNLINK_LOT_BASE_ADDR` macro added. All tests pass.

### Phase 2: Toolchain Target Selection ✅
**Status:** Complete. All 9 targets compile successfully.

### Phase 3: Multi-Target QEMU Test Hosts (In Progress)

#### Hybrid QEMU Architecture

Each mainline QEMU host lives in `tests/platforms/<name>/` and is **self-contained**:
- No STM32 HAL dependency
- Generic CMSIS-style vector table
- Custom semihosting layer (`SYS_WRITE0` for output, `SYS_EXIT` to quit)
- `platform.cmake` with `-mcpu` and `-mfloat-abi` for the target
- `mem.ld` matching the machine's documented memory map

#### 3a: Refactor existing host into platform dispatcher ✅
Complete. STM32F429 host moved to `tests/platforms/stm32f429_discovery/`.

#### 3b–3c: STM32F103 (M3) and STM32F051 (M0) on xPack QEMU ⚠️
Skeleton hosts created but **Flash→RAM call quirk** prevents full test execution. Documented as QEMU emulation bug. These will be **re-implemented on mainline QEMU** (see 3h–3i below).

#### 3d: MPS2-AN386 (Cortex-M4) on mainline QEMU 🔄 **CURRENT TASK**
**Goal:** Prove the self-contained host pattern on mainline QEMU.

| # | Task | Files | Notes |
|---|------|-------|-------|
| 3d.1 | Create `tests/platforms/mps2_an386/` | new | `platform.cmake`, `mem.ld` (SRAM @ 0x00000000 or 0x20000000? Verify MPS2 AN386 memory map) |
| 3d.2 | Generic startup with vector table | `startup.s` | No HAL. Sets SP, copies .data, zeros .bss, calls main. |
| 3d.3 | Custom semihosting layer | `semihosting.c` | `SYS_WRITE0` for string output, `SYS_WRITEC` for single char, `SYS_EXIT` to quit. Replaces newlib `_write`. |
| 3d.4 | Update `test_driver.py` for mainline QEMU | `test_driver.py` | Detect platform, use `qemu-system-arm` with `-machine mps2-an386 -cpu cortex-m4 -semihosting` |
| 3d.5 | Run all tests | `tests/` | Validate that module load, internal calls, and host function calls all work. |

**MPS2 AN386 memory map (to verify):**
- 4MB ZBTSRAM @ 0x00000000 (code + data)
- 4MB BRAM @ 0x20000000 (alias or additional RAM)
- Peripherals @ various addresses

The linker script should place `.text` and `.data` in RAM because QEMU loads the kernel directly into RAM (no Flash emulation for MPS2). Vector table at 0x00000000.

#### 3e: MPS2-AN500 (Cortex-M7) on mainline QEMU
**Priority:** High (next after M4 proof-of-concept)

| # | Task | Files | Notes |
|---|------|-------|-------|
| 3e.1 | Create `tests/platforms/mps2_an500/` | new | `platform.cmake`, `mem.ld` (AN500: 8MB ZBTSRAM @ 0x60000000, 4MB BRAM @ 0x20000000) |
| 3e.2 | Generic startup | `startup.s` | Same pattern as AN386, adapted for M7 |
| 3e.3 | Custom semihosting | `semihosting.c` | Reuse from AN386 |
| 3e.4 | Update `test_driver.py` | `test_driver.py` | `qemu-system-arm -machine mps2-an500 -cpu cortex-m7 -semihosting` |
| 3e.5 | Run tests | `tests/` | Verify M7 module compilation + runtime |

#### 3f: micro:bit (Cortex-M0) on mainline QEMU
**Priority:** Medium (validates M0 template + Thumb-1 only)

| # | Task | Files | Notes |
|---|------|-------|-------|
| 3f.1 | Create `tests/platforms/microbit/` | new | `platform.cmake`, `mem.ld` (16K RAM @ 0x20000000) |
| 3f.2 | M0-compatible startup | `startup.s` | Thumb-1 only. No high-register PUSH. |
| 3f.3 | Custom semihosting | `semihosting.c` | Reuse from AN386 |
| 3f.4 | Update `test_driver.py` | `test_driver.py` | `qemu-system-arm -machine microbit -cpu cortex-m0 -semihosting` |
| 3f.5 | Run tests | `tests/` | Validate M0 module compilation + runtime |

#### 3g: STM32VLDiscovery (Cortex-M3) on mainline QEMU
**Priority:** Medium (validates M3 on mainline, avoids xPack quirk)

| # | Task | Files | Notes |
|---|------|-------|-------|
| 3g.1 | Create `tests/platforms/stm32vldiscovery/` | new | `platform.cmake`, `mem.ld` (8K RAM @ 0x20000000, 128K Flash @ 0x08000000) |
| 3g.2 | Generic startup | `startup.s` | Vector table at 0x08000000. Data init from Flash to RAM. |
| 3g.3 | Custom semihosting | `semihosting.c` | Reuse from AN386 |
| 3g.4 | Update `test_driver.py` | `test_driver.py` | `qemu-system-arm -machine stm32vldiscovery -cpu cortex-m3 -semihosting` |
| 3g.5 | Run tests | `tests/` | Should pass where xPack STM32F103 failed |

#### 3h: Olimex STM32-H405 (Cortex-M4F hard-float) on mainline QEMU
**Priority:** Medium (validates hard-float ABI)

| # | Task | Files | Notes |
|---|------|-------|-------|
| 3h.1 | Create `tests/platforms/olimex_stm32_h405/` | new | `platform.cmake` with `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| 3h.2 | `mem.ld` | new | Match STM32F405 memory map |
| 3h.3 | Generic startup | `startup.s` | Same as M4 but with FPU init |
| 3h.4 | Custom semihosting | `semihosting.c` | Reuse from AN386 |
| 3h.5 | Update `test_driver.py` | `test_driver.py` | `qemu-system-arm -machine olimex-stm32-h405 -cpu cortex-m4 -semihosting` |
| 3h.6 | Run tests | `tests/` | Validate hard-float module on hard-float host |

#### 3i: MPS2-AN505 (Cortex-M33) on mainline QEMU
**Priority:** Low (newer architecture, less urgent)

#### 3j: MPS3-AN547 (Cortex-M55) on mainline QEMU
**Priority:** Low

#### 3k: MPS3-AN552 (Cortex-M85) on mainline QEMU
**Priority:** Low

**Review gate:** `test_driver.py` passes on every supported target platform.

---

### Phase 4: Documentation, CI, and Polish

| # | Task | Files | Agent | Deliverable |
|---|---|---|---|---|
| 4a | Update README with target configuration guide | `README.md` | `quick` | `--target`, LOT base setup, architecture tag explanation |
| 4b | Update AGENTS.md with new constraints and build commands | `AGENTS.md` | `quick` | |
| 4c | Add target compatibility matrix | `README.md` or `docs/TARGETS.md` | `quick` | Table of supported targets, QEMU status, float ABI |
| 4d | Update CI workflow to install both QEMU variants | `.github/workflows/ci.yml` | `agent` | `qemu-system-gnuarmeclipse` + `qemu-system-arm` |
| 4e | Update CI to run tests for all targets | `.github/workflows/ci.yml` | `agent` | Matrix or sequential runs for each platform |
| 4f | Document mainline QEMU host creation | `tests/platforms/README.md` | `quick` | How to add a new self-contained platform |

**Review gate:** CI passes green. README is accurate.

---

## Sub-Planning Documents

- `.opencode/plans/phase3-multi-target-hosts.md` — Detailed step-by-step for each new platform

---

## Risk Register (Updated)

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| M0 assembly prologue is incorrect | Medium | High | Validate with mainline QEMU `microbit` host. |
| `qemu-system-gnuarmeclipse` missing in newer distros | Medium | High | CI pins xPack release. F429 baseline stays on xPack. All new platforms use mainline QEMU. |
| **Mainline QEMU `SYS_WRITE` semihosting broken** | **Confirmed** | **High** | Custom semihosting layer uses `SYS_WRITE0` instead. Documented. |
| **xPack QEMU M3/M0 Flash→RAM call hang** | **Confirmed** | **High** | Documented as known QEMU bug. M3/M0 testing moves to mainline QEMU. |
| AN500/AN547/AN552 memory maps differ from docs | Medium | Medium | Verify with ARM MPS2/MPS3 documentation and NuttX/Zephyr linker scripts. |
| Module header size increase breaks existing consumers | Low | High | Keep header 32-bit aligned. Document as breaking ABI change. |
| Hard-float module on soft-float host crashes | Medium | High | Runtime arch tag checks float ABI; rejected at load time. |


---

## Status

- **Phases 0–4 (original plan) completed on 2026-05-25.**
- **New Phase 3 sub-targets (mainline QEMU) in progress.**
- Current focus: **MPS2-AN386 (Cortex-M4) proof-of-concept** on mainline QEMU.

## Next Steps

1. **Implement MPS2-AN386 host** with self-contained startup + custom semihosting.
2. Validate all 22 tests pass on AN386.
3. Proceed to M7 (AN500), then M0 (micro:bit), M3 (STM32VL), M4F (Olimex H405).
4. Update CI to run both xPack (F429) and mainline QEMU (new platforms) tests.

(End of file)
