# Host Sanitizer & Fuzz Testing of the Loader

This guide covers the host-side **sanitizer** and **libFuzzer** harnesses that
exercise the udynlink **loader** — `udynlink_load_module`, relocation
application, symbol-table walks, and image deserialization in
`udynlink/udynlink.c`. It is distinct from [host testing of module
logic](host-testing.md): module-logic host tests compile module *sources* and
never link the loader; this harness compiles the loader and never executes
module code.

- [What is fuzzed](#what-is-fuzzed)
- [The trust model being tested](#the-trust-model-being-tested)
- [Building and running](#building-and-running)
- [Reading a crash](#reading-a-crash)
- [Corpus policy](#corpus-policy)
- [Known findings](#known-findings)

## What is fuzzed

The loader's public entry points, across all three load modes:

- `udynlink_validate_header`, `udynlink_get_image_metadata_size`
- `udynlink_load_module` (contiguous image → `udynlink_image_from_memory`)
- `udynlink_load_module_image` (non-contiguous image descriptor)
- `udynlink_lookup_symbol`, `udynlink_get_symbol_value`, `udynlink_get_module_name`
- `udynlink_relocate_module` (exercises `rebase_module_pointers`)
- `udynlink_unload_module`

The harness's allocator stub implements the loader ABI 3.1 callback signature
(`udynlink_external_malloc(size, section, align, flags)` / the matching `free`)
routed to the same backing `malloc`, so both the untagged main-block calls and
any sectioned-module allocation requests exercise the stub. Mutated inputs can
set the header's section-table flag, so the pre-load gate uses
**`udynlink_get_image_size_bounded(buf, size)`**, not the plain
`udynlink_get_image_size()`: the latter dereferences only the header and cannot
see tagged-section payloads, so a sectioned image could declare a 256 KiB
tagged payload in a 508-byte buffer and pass a gate built on it — the loader
would then `memcpy` those 256 KiB out of the input buffer (`udynlink.c`, the
per-tagged-section copy). The bounded variant returns the true total or `0`
when it cannot be established within the buffer; the harness rejects both `0`
and any result larger than the buffer. `just fuzz-seeds` therefore plants a
**truncated** sectioned seed (`sections_truncated.bin`) alongside the valid one,
because the sanitizer corpus is not mutated and the "declared payload longer
than the buffer" case would otherwise go uncovered. Section-table fields join
the header-derived values the loader must bound-check.

After a successful load, the harness also calls `udynlink_relocate_module(&mod,
NULL, 0)` to exercise the rebase path with a NULL destination (the loader then
asks our `malloc` stub for the move target). The harness treats **any**
`udynlink_error_t` return as success — many fuzz inputs are legitimately
malformed and must be rejected gracefully, not crash. The only failure the
harness cares about is an ASan/UBSan report or a signal.

## The trust model being tested

The loader's documented contract is that every module image begins with a full
`udynlink_module_header_t` (32 bytes). The harness gates on that contract at
its boundary (`tests/fuzz/fuzz_harness.c`): inputs shorter than 32 bytes are
rejected by the harness itself, never handed to the loader, so the fuzzer
spends its budget on the real fuzz surface — **header-derived lengths**
(`num_rels`, `symt_size`, `code_size`, `data_size`, `bss_size`, `num_lot`)
used as `memcpy` sizes, `malloc` sizes, and array indices — rather than
tripping on every short input.

This is exactly the surface where sanitizers and fuzzers pull their weight.
The harness copies each fuzzer-supplied buffer into a freshly `malloc`'d
region of exactly `size` bytes before exercising the loader. A dedicated
`malloc(size)` (rather than reusing libFuzzer's input arena) places an ASan
redzone exactly at the buffer end, so any header-derived length walking past
`size` is caught as a `heap-buffer-overflow`.

Module code is ARM Thumb and **must never execute on the host**. The harness
never calls `udynlink_cpp_init` and never dereferences module function
pointers. `udynlink_external_resolve_symbol` returns a non-zero sentinel
(`0x1000`) so EXTERN relocations take their write branch (better coverage than
the weak default, which would reject every extern-bearing module); the
sentinel address need not be valid because the code is never invoked.

## Building and running

Two opt-in CMake targets live under `tests/fuzz/` and are guarded by the
`UDYNLINK_BUILD_FUZZERS` CMake option (OFF by default so downstream consumers
never pull in host-compiler-only fuzz targets):

| Target | Compilers | Sanitizers | Entry point |
|---|---|---|---|
| `udynlink_fuzz_load` | clang only | ASan + UBSan + libFuzzer | `LLVMFuzzerTestOneInput` |
| `udynlink_san_load` | gcc or clang | ASan + UBSan | `int main` — iterates `tests/fuzz/corpus/*.bin` |

Both compile `udynlink/udynlink.c` natively as a host object. One macro
override is required: `udynlink_cpp_init` expands `UDYNLINK_PREPARE_CALL`,
whose body is `__asm volatile ("mov r9, %0" ...)` — invalid on x86-64. The
header guards the macro with `#ifndef`, so the fuzz/san targets redefine it
as a no-op on the compile line:
`-DUDYNLINK_PREPARE_CALL(p_mod)=((void)(p_mod))`.

### `just` recipes (recommended)

```bash
just fuzz-seeds        # Regenerate the seed corpus from in-repo module sources
just test-san          # Build and run the ASan+UBSan regression gate (gcc or clang)
just fuzz              # Build and run the libFuzzer harness for 60 seconds (needs clang)
just fuzz 10           # Run for 10 seconds instead
```

`just fuzz-seeds` regenerates `tests/fuzz/corpus/*.bin` deterministically from
the in-repo test module sources via `scripts/mkmodule`. It covers extern
relocations, weak symbols, data relocations, cross-module dependencies, and
C++ `__init_array` paths. Generated corpus files are gitignored (covered by
`*.bin` in `.gitignore`); the recipe regenerates them.

### Manual CMake

```bash
# Sanitizer gate (gcc or clang)
cmake -B build-san -S . -DUDYNLINK_BUILD_FUZZERS=ON
cmake --build build-san --target udynlink_san_load
ctest --test-dir build-san -R udynlink_san_load --output-on-failure

# libFuzzer harness (clang only)
cmake -B build-fuzz -S . -DUDYNLINK_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang
cmake --build build-fuzz --target udynlink_fuzz_load
./build-fuzz/tests/fuzz/udynlink_fuzz_load tests/fuzz/corpus \
    -max_total_time=60 -print_final_stats=1
```

## Reading a crash

When the fuzzer finds a bug it writes a `crash-*` / `oom-*` file to the
current directory (or to `-artifact_prefix=...`). Reproduce a single input
directly:

```bash
./build-fuzz/tests/fuzz/udynlink_fuzz_load <crash-file>
```

libFuzzer prints the ASan/UBSan report with the stack frame in the loader
(`udynlink/udynlink.c:LINE`) and a base64 encoding of the crashing input.
Decode the input to inspect the mutated header fields, or feed it back to
`just fuzz` as a seed once the underlying bug is fixed (so the fixed path
stays covered). Minimize a crashing input with:

```bash
./build-fuzz/tests/fuzz/udynlink_fuzz_load -minimize_crash=1 <crash-file> \
    -exact_artifact_path=tests/fuzz/findings/minimized.bin
```

## Corpus policy

The seed corpus under `tests/fuzz/corpus/` is **regenerated, not committed**.
`just fuzz-seeds` rebuilds it from in-repo module sources with `mkmodule`,
plus two minimality seeds (`seed_1byte.bin` — sub-header input the harness
short-circuits; `seed_udlm_sig.bin` — the bare 4-byte `UDLM` signature).
`just clean` removes the corpus dir along with the fuzz build dirs.

If you discover a new crash, save the (minimized) input under
`tests/fuzz/findings/documented/` and reference the loader bug it exposed.
Do **not** add crash files to the committed corpus — they belong to a specific
loader version and would mask regressions once fixed.

## Known findings

The harness has surfaced real loader bugs. Each was fixed at the source in
`udynlink/udynlink.c`; the crashing inputs that originally reproduced them are
kept under `tests/fuzz/findings/documented/` for regression reference.

1. **OOB write via `lot_offset`** — `udynlink_load_apply_relocations` indexed
   `p_lot`/`p_data` with an untrusted `lot_offset` from the relocation table.
   Fixed by bounding `lot_offset < num_lot + data_size/4` and guarding the
   two `p_data + (lot_offset - num_lot)` branches against unsigned underflow.
   Repro: `crash-a516ec2c33c119cd84e806ccfc40ee698391dc39`.
2. **OOB read of `*p_symt` entry count** — `compute_num_named_syms_raw` /
   `get_sym_at_raw` trusted the symtab's entry-count word and `name_off`
   without bounding them against `p_header->symt_size`. Fixed by threading
   `symt_size_bytes` through the (file-static) helpers and clamping the name
   pointer offset.
3. **OOM via oversized header sizes** — `udynlink_load_module_image` passed
   the header-derived RAM size directly to `udynlink_external_malloc`; a
   mutated `code_size` of ~1.3 GB drove a multi-GB allocation. Fixed by a
   sanity cap (`UDYNLINK_MAX_RAM_SIZE`, 16 MiB — >1000× headroom over real
   modules) that rejects implausibly large headers before allocation. A
   second cap, `UDYNLINK_MAX_IMAGE_SIZE` (4 MiB), bounds the total
   header-derived image size for the XIP path (whose `ram_size` excludes
   `code_size`) and the metadata-only paths.
4. **NULL-deref UB in zero-size memcpy** — `udynlink_load_module_image` called
   `memcpy(NULL, ..., 0)` for modules with `ram_size == 0` (all size fields
   zero). On Cortex-M this is a no-op but it is UB; UBSan flagged
   `nonnull` argument violations. Fixed by guarding the COPY_TEXT_DATA/XIP
   `memcpy` and the BSS `memset` with `> 0` size checks.
5. **64-bit size_t truncation in alignment** — `get_code_offset_from_header`
   computed `(res + 3) & ~3U`. On 64-bit `size_t`, `~3U` is 32-bit
   (`0xfffffffc`), zero-extended when promoted, silently **zeroing the high
   half of `res`**: a header with `symt_size = 0xffffffbc` computed a tiny
   `code_offset = 20` instead of ~4 GiB, bypassing
   `UDYNLINK_MAX_IMAGE_SIZE` and driving `memcpy(..., symt_size = 4.3 GB)`.
   Silent on 32-bit Cortex-M; surfaced only on host. Fixed by
   `& ~(size_t)3`. This finding recontextualizes the third-bug discussion:
   the cap *does* work when the alignment is correct.
6. **strcmp OOB on unterminated symtab string** — `compute_num_named_syms_raw`
   clamped `name_off` against `symt_size_bytes` but called `strcmp(prev, cur)`,
   which reads through to NUL regardless of the bound. A malformed image
   omitting the terminator within `symt_size` let `strcmp` walk past the
   loader's own RAM copy of the symtab. Fixed by replacing `strcmp` with
   `strncmp(prev, cur, min(remaining_at_prev, remaining_at_cur))` so the
   compare cannot read past either string's bound.
7. **Caller-contract violation (harness-side, not a loader bug)** — the
   loader has no API to receive the source buffer size, so it cannot
   self-defend against a header that claims more code/data/symtab than the
   buffer actually holds (the COPY_ALL `memcpy(p_temp8 + code_offset,
   image->p_code, p_header->code_size)` reads as many bytes as the header
   claims, not as many as the caller actually supplied). The harness now
   enforces the loader's documented contract at the caller boundary before
   invoking any load entry point — originally `size >=
   udynlink_get_image_size(buf)`, and since sectioned images gained their own
   payloads, `size >= udynlink_get_image_size_bounded(buf, size)` so that
   tagged-section payloads (which the header-only variant cannot see) are
   bounded too. This is the boundary check the harness was always meant
   to make; it is not a loader fix. A future ABI extension that gives the
   loader the source size would let the loader self-defend (planned follow-up
   for a hard-fault-only hardening target).
