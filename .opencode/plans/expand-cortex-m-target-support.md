# udynlink: Expand Target Support & MCU Configurability

## Goal

Make `udynlink` work across the broader ARM Cortex-M family (M0/M0+, M3, M4, M7, and future cores) and allow users to easily configure memory mappings (especially RAM/LOT base address) for their specific microcontroller, instead of being locked to STM32F429 defaults.

## Current State Summary

udynlink currently has **STM32F429I-Discovery hardcoded throughout** the stack:

| Layer | Hardcoded Assumption | Location(s) |
|-------|----------------------|-------------|
| **Runtime loader** | LOT base fixed at `0x20000000` | `udynlink.c:323`, `udynlink.h` (implicit) |
| **Toolchain** | CPU = `cortex-m4`, arch = `armv7-m` | `scripts/mkmodule` (compile/asm/link), `scripts/asm_template.tmpl` |
| **Test host** | STM32F429 HAL, `mem.ld` with 192K RAM @ 0x20000000 | `tests/qemu_host/ldscripts/mem.ld`, makefiles, `test_driver.py` |
| **Test utilities** | LOT base hardcoded | `tests/qemu_host/src/test_utils.c:45` |

The module image format itself is architecture-agnostic, but the **code generated inside modules is not** — it uses CPU-specific compiler flags and architecture-specific assembly prologues.

## Key Decision Points (Need User Input)

### 1. LOT Base Configuration Strategy

The LOT (Linker Offset Table) base is the RAM address where `r9` is loaded from. Currently hardcoded to `0x20000000` (STM32F4 RAM base).

**Options:**
- **A. Compile-time macro** (`UDYNLINK_LOT_BASE_ADDR`) — zero runtime overhead, simple. Requires rebuilding host firmware for each target.
- **B. Runtime API** (`udynlink_set_lot_base(uint32_t addr)`) — one-time setup call, more flexible. Adds a global variable + indirection in `udynlink_cpp_init` and assembly prologue.
- **C. Per-module API** — store LOT base in `udynlink_module_t`, most flexible but changes ABI and adds per-module overhead.

**Tradeoffs:**
- A is simplest and aligns with embedded "compile for your target" philosophy.
- B is needed if the same host firmware binary must support multiple MCU variants at runtime.
- C is overkill unless you plan to load modules with different RAM bases simultaneously (unlikely).

> **Open question:** Do you need runtime configurability (same firmware binary for multiple board variants), or is compile-time sufficient?

### 2. Target CPU / Architecture Selection

The toolchain currently hardcodes `-mcpu=cortex-m4 -mthumb` and `.arch armv7-m`.

**Proposed approach:**
Add a `--target` (or `--cpu`) flag to `mkmodule` that sets:
- Compiler/assembler/linker `-mcpu=` flag
- Assembly template `.arch` directive
- Optional: `-mfloat-abi=` and FPU flags for M4F/M7

**Supported targets (initial):**
| Target | CPU flag | Arch directive | Notes |
|--------|----------|----------------|-------|
| `cortex-m0` | `cortex-m0` | `armv6-m` | Thumb only, no `cbz`/`cbnz` in prologue? |
| `cortex-m0plus` | `cortex-m0plus` | `armv6-m` | Same as M0 |
| `cortex-m3` | `cortex-m3` | `armv7-m` | Thumb-2, no FPU |
| `cortex-m4` | `cortex-m4` | `armv7-m` | Current default, optional `-mfloat-abi=hard` |
| `cortex-m7` | `cortex-m7` | `armv7-m` | Optional DP FPU, cache lines matter for XIP |

**Open question:** Do you need M0/M0+ support now, or is M3/M4/M7 sufficient for the first phase? M0 lacks some Thumb-2 instructions; we should validate the prologue assembly.

### 3. Test Infrastructure Strategy

Current tests depend on Eclipse-generated STM32F4 makefiles and `qemu-system-gnuarmeclipse -board STM32F429I-Discovery`.

**Options:**
- **A. Keep STM32F429 as the sole QEMU test target** — easiest. Add `--target` to `mkmodule` but don't expand QEMU tests. Document that other targets need manual validation.
- **B. Parameterize QEMU tests per target** — if `qemu-system-gnuarmeclipse` supports other STM32 boards (M0/M3/M7), we could parameterize the board name. Requires investigating QEMU board support.
- **C. Migrate to modern QEMU** — mainline QEMU has STM32 support via `qemu-system-arm -machine stm32f429` or similar. This could simplify the niche `gnuarmeclipse` dependency.

**Open question:** Is expanding QEMU test coverage to multiple targets a priority, or is making the *toolchain* configurable the immediate goal?

### 4. Module ABI Versioning

The module header has commented-out `mod_version` and `udynlink_version` fields. As we expand target support, modules compiled for one CPU/architecture may be incompatible with a host expecting another.

**Proposed:** Uncomment version fields and add a **target architecture tag** (or minimum loader version) to the module header so the runtime can reject incompatible modules at load time.

**Open question:** Should we add a simple "architecture identifier" field to the module header (e.g., `0x01` for Thumb-2, `0x02` for Thumb), or rely on version numbers alone?

## Proposed Architecture

### Runtime (`udynlink/`)

1. **Configurable LOT base** (via compile-time macro or runtime API).
2. **Remove `#include <stdio.h>`** from `udynlink.c` (replace `snprintf` usage — actually there is no `snprintf` usage; the include appears unused).
3. **Fix `UDYNLINK_MAKE_VERSION` / `UDYNLINK_GET_MAJOR_VERSION`** macro mismatch (shifts by 8 vs 16).
4. **Uncomment and use module header version fields** for ABI checking.

### Toolchain (`scripts/`)

1. **`mkmodule` target selection**:
   - Add `--target <cpu>` CLI flag (default: `cortex-m4` for backward compat).
   - Pass target through to `compile_cmd`, `asm_cmd`, `link_cmd`.
   - Select correct `.arch` in `asm_template.tmpl` via Jinja2 variable.
2. **Make linker script origin configurable?** — `code_before_data.ld` uses `ORIGIN = 0x00000000` which is fine (it's a module-relative link, not absolute). No change needed.
3. **Pass LOT base to assembly template** so the prologue loads from the configured address instead of `0x20000000`.

### Tests (`tests/`)

1. **Update `test_utils.c`** to use configurable LOT base.
2. **Update `test_driver.py`** to pass `--target` to `mkmodule` if parameterized tests are desired.
3. **Document** how to add new target-specific tests.

## Task Breakdown

### Phase 1: Foundation — Configurable LOT Base & Cleanups
**Goal:** Make the most impactful change (LOT base) and fix existing known issues.

| Task | Files | Agent | Notes |
|------|-------|-------|-------|
| 1a. Remove `#include <stdio.h>` from `udynlink.c` | `udynlink/udynlink.c` | `quick` | Verify no `snprintf`/`printf` usage exists |
| 1b. Fix `UDYNLINK_MAKE_VERSION` macro shift mismatch | `udynlink/udynlink.h` | `quick` | Decide on 8-bit or 16-bit major/minor fields |
| 1c. Fix typos in `udynlink.h` | `udynlink/udynlink.h` | `quick` | "ownershsip" → "ownership", "correponding" → "corresponding" |
| 1d. Add LOT base configuration mechanism | `udynlink/udynlink.h`, `udynlink/udynlink.c`, `udynlink/udynlink_externals.h` | `agent` | Either compile-time macro or runtime API + update `udynlink_cpp_init` |
| 1e. Update assembly template to use configurable LOT base | `scripts/asm_template.tmpl`, `scripts/mkmodule` | `agent` | Pass LOT base as template variable |
| 1f. Update test utilities and host to use configurable LOT base | `tests/qemu_host/src/test_utils.c`, `tests/qemu_host/src/main.c` | `quick` | Must match runtime choice |
| 1g. Run full test suite to verify no regressions | `tests/` | `shell` | `python test_driver.py` |

### Phase 2: Target CPU / Architecture Selection
**Goal:** Allow `mkmodule` to generate modules for different Cortex-M cores.

| Task | Files | Agent | Notes |
|------|-------|-------|-------|
| 2a. Add `--target` CLI argument to `mkmodule` | `scripts/mkmodule` | `agent` | Parse and validate target name |
| 2b. Make compile/link/asm commands target-aware | `scripts/mkmodule` | `agent` | Substitute `-mcpu=` and `.arch` |
| 2c. Validate assembly prologue compatibility for each target | `scripts/asm_template.tmpl` | `agent` | May need target-specific templates for M0 vs M3/M4/M7 |
| 2d. Add target architecture tag to module header | `udynlink/udynlink.h`, `scripts/mkmodule` | `agent` | For loader to reject incompatible modules |
| 2e. Update runtime to check architecture tag | `udynlink/udynlink.c` | `agent` | New error code: `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` |
| 2f. Run test suite for default target (cortex-m4) | `tests/` | `shell` | Backward compatibility check |

### Phase 3: Test Infrastructure & Documentation
**Goal:** Make the test system less STM32F429-centric and document target configuration.

| Task | Files | Agent | Notes |
|------|-------|-------|-------|
| 3a. Parameterize test driver target selection (optional) | `tests/test_driver.py` | `agent` | If user wants multi-target QEMU tests |
| 3b. Investigate modern QEMU as `gnuarmeclipse` replacement | `.github/workflows/ci.yml`, `tests/test_driver.py` | `investigate` | Long-term: reduce niche dependency |
| 3c. Update README with target configuration guide | `README.md` | `quick` | `--target`, LOT base setup, per-MCU notes |
| 3d. Update AGENTS.md with new known constraints | `AGENTS.md` | `quick` | Document configurable parameters |

## Review Checklist (Per Phase)

Before merging each phase:
1. All existing tests pass (`python test_driver.py` with no failures).
2. No new compiler warnings introduced.
3. Documentation updated to reflect changes.
4. Backward compatibility maintained (default target = `cortex-m4`, default LOT base = `0x20000000`).

## Guiding Questions for User Review

1. **LOT base:** Compile-time macro vs runtime API — which fits your use case?
2. **Target scope:** Should Phase 2 include Cortex-M0/M0+ immediately, or start with M3/M4/M7?
3. **Tests:** Do you need QEMU tests for multiple targets, or is STM32F429 test coverage + configurable toolchain enough?
4. **ABI versioning:** Should we add an architecture tag to the module header now, or defer to a later phase?
5. **Float ABI:** For Cortex-M4F/M7, do you need hard-float (`-mfloat-abi=hard`) module support, or soft-float only?
