# QEMU Testing Strategy: Semihosting vs UART vs SEGGER_RTT

## Research Findings

### 1. -O2/-O3 Builds

**Status: ✅ WORKING**

Both `-O2` and `-O3` compile and run correctly on all tested platforms:
- Verified by compiling `test-globals1` module with `--build_flags="-O2"` and `--build_flags="-O3"`
- Tests pass on MPS2-AN386 (mainline QEMU 9.2.4) with both optimization levels
- No issues with the dynamic linker or relocation handling at higher optimization levels

**Note:** The default for `mkmodule` is `-Os` (size optimize). `--no-opt` uses `-O0`. There is no dedicated `-O2`/`-O3` flag, but they can be passed via `--build_flags="-O2"`.

---

### 2. Semihosting Performance

**Current bottleneck:** `qemu-system-arm` mainline is inherently slow at Cortex-M emulation.

| Test | Time (MPS2-AN386, QEMU 9.2.4) | Output Method |
|------|-------------------------------|---------------|
| test-globals1 (3 load modes) | ~60s | SYS_WRITE0 buffered |
| 1M empty loop iterations | ~10s | N/A |
| 100 SYS_WRITE0 calls | ~5s | SYS_WRITE0 |

The slowness is primarily QEMU's ARM emulation speed, not the semihosting mechanism itself.

**Optimized semihosting implementation** (now in `tests/platforms/mps2_an386/semihosting.c`):
- Buffers output in a 512-byte RAM buffer
- Flushes on `\n` or buffer full via `SYS_WRITE0` (single `bkpt` for entire string)
- Avoids per-character `SYS_WRITEC` calls
- This is already implemented and committed

**Remaining issue:** Even with buffered output, tests take ~60s because QEMU 9.2.4's Cortex-M4 emulation is slow. The STM32F429 tests with the legacy `qemu-system-gnuarmeclipse` were faster because that fork had STM32-specific optimizations.

---

### 3. UART Approach (CMSDK APB UART on MPS2)

**Status: ❌ NOT WORKING in non-interactive mode**

The MPS2-AN386 has 5 CMSDK APB UARTs at:
- UART0: `0x40004000`
- UART1: `0x40005000`
- UART2: `0x40006000`
- UART3: `0x40007000`
- UART4: `0x40009000`

**Findings:**
- QEMU does emulate the CMSDK UART (verified via `info qtree`)
- Register reads/writes work (verified with GDB)
- The `-serial stdio` option connects UART0 to stdio
- **However:** When running under `timeout` or in non-interactive contexts, QEMU's stdio chardev backend does not accept data — writes to UART DATA register result in `TXFULL` + `TXOVERRUN` state
- This appears to be a QEMU stdio chardev limitation in non-TTY environments

**Workaround attempts tried:**
- `-serial stdio -display none` ❌
- `-chardev file,id=serial0,path=/tmp/out.txt -serial chardev:serial0` ❌
- `-chardev stdio,id=serial0,signal=off -serial chardev:serial0` ❌

None produced output. The UART works in principle but QEMU's chardev plumbing fails in scripted/CI contexts.

---

### 4. SEGGER_RTT + probe-rs

**Status: ✅ EXCELLENT for hardware, ❌ COMPLEX for QEMU**

**How SEGGER_RTT works:**
1. Target writes to a ring buffer in RAM at a known address
2. Debug probe (J-Link, ST-Link) polls this buffer via SWD/JTAG
3. Host software reads the buffer and displays output

**probe-rs support:**
- `probe-rs run --chip <chip>` can flash, run, and display RTT output
- Supports many Cortex-M chips with built-in RTT target descriptions
- **Does NOT natively support QEMU** — probe-rs expects a physical probe

**For QEMU, would require:**
- A script that reads guest RAM at the RTT buffer address via QEMU's gdbstub or monitor
- Or a custom QEMU chardev backend that reads from RTT buffer memory
- Or using QEMU's `-device loader` with a memory region that maps to a host file

**Conclusion:** SEGGER_RTT is the best approach for **real hardware testing** but requires significant work to adapt for QEMU. The buffered semihosting approach is the most practical for QEMU CI/testing.

---

## Recommended Strategy

| Platform | Testing Method | Rationale |
|----------|---------------|-----------|
| **QEMU CI (all platforms)** | Buffered semihosting (`SYS_WRITE0`) | Works reliably, no extra tooling |
| **Real hardware / dev boards** | SEGGER_RTT + probe-rs | Fast, non-intrusive, no semihosting overhead |
| **Future optimization** | Memory-mapped UART if QEMU chardev issue resolved | Fastest for QEMU if we can make stdio work |

---

## Next Steps for probe-rs Integration (Hardware)

1. Add `_SEGGER_RTT` block to test host firmware (or keep in a separate RTT-enabled build)
2. Create `probe-rs` target YAML for supported boards
3. Add `just run-hw` or similar command that uses `probe-rs run --chip <chip> <elf>`
4. Document RTT output structure for test framework integration

---

## Notes

- **xPack QEMU 9.2.4** only includes `qemu-system-arm` and `qemu-system-aarch64` — the `qemu-system-gnuarmeclipse` fork is discontinued
- Mainline QEMU has gained basic STM32 support (`olimex-stm32-h405`, `stm32vldiscovery`) but lacks STM32F429
- For MPS2/MPS3 platforms, mainline QEMU is the best option despite slower emulation
- The M3/M0 Flash→RAM call quirk is **not present** in mainline QEMU — it was specific to `qemu-system-gnuarmeclipse`

---

*Document written after testing QEMU 9.2.4 with MPS2-AN386 platform*
