# Plan: wasm2c → udynlink v2 (Redesign)

> Supersedes the v1 plan (Phases 1–2 PoC, see git history for the original document
> and its recorded results). Created 2026-09-07 after a ground-truth review of the
> prototype: core loader/PIC integration verified working; build script has
> verified defects (no-op flags, broken imports, missing wasm2c-mandated compile
> flags); Phases 2.5–5 never started.

---

## 0. Prototype status snapshot (reviewed 2026-09-07)

**Verified working** (evidence: QEMU + built artifacts):

| Capability | Evidence |
|---|---|
| wasm2c 1.0.34 output → udynlink module (PIC/LOT/prologues) | `test-wasm2c-{add,fac,hello}` green on MPS2-AN386, `-O3`+`-Os`, all 3 load modes |
| End-to-end script build for import-free modules | `hello.wasm` → 2972 B valid UDLM module |
| Custom bare-metal runtime (no libc, no setjmp) | ~1.6 KB text after `--gc-sections`; no unresolved symbols |
| Binary-format coverage | parser contract tests round-trip wasm2c-shaped bins |

**Verified defects** (all reproduced or proven by TU-level compile experiments):

| # | Defect | Where |
|---|---|---|
| D1 | `--static-memory` / auto-static / `--custom-page-size` are no-ops: defines land in shim TU only; runtime TU compiles dynamic-malloc path (proof: e2e module `bss=52` vs 65540 with define reaching runtime) | `scripts/mkwasm2c-module` `generate_shim` |
| D2 | Modules with imports fail to build: shim calls `wasm2c_X_instantiate(inst)` but wasm2c emits `(inst, w2c_env*)` | shim generator |
| D3 | `--trap-handler` dead: runtime calls weak `wasm_rt_trap_handler`; never reads `WASM_RT_TRAP_HANDLER` macro | `wasm-rt-udynlink.c:140` vs shim |
| D4 | `wasm_rt_mem_realloc` fallback copies **new** size from old buffer → OOB read on every `memory.grow` | `wasm-rt-udynlink.c:96` |
| D5 | Funcref/externref tables malloc'd, never zeroed → garbage funcref instead of null; `call_indirect` on unfilled slot = wild call | `wasm-rt-udynlink.c:248-269` |
| D6 | Export parser silently drops pointer-returning exports (e.g. `memory`) and anything sret/multi-value-shaped | `mkwasm2c-module` regex |
| D7 | wasm2c-mandated flags `-fno-optimize-sibling-calls -frounding-math` never passed → latent float-miscompilation (tests are integer-only, so hidden) | module compile command |
| D8 | `--gen-c-header` default `--header-path .` resolves **after** `chdir(tmpdir)` → header written into temp dir and deleted | `mkwasm2c-module:401` |
| D9 | Symtab bloat: 46 symbols / 1060 B (36% of image) for a 2-function module; runtime internals, wasm2c internals, libc names all exported | mkmodule defaults, no `--public-symbols` default |
| D10 | Test assets drift: `test-wasm2c-add` carries stale spike runtime; committed generated C; no `.wat` source; script path has zero CI coverage | `tests/test-wasm2c-*` |
| D11 | Stack exhaustion = real MCU stack overflow (depth counting disabled), not a wasm trap | `wasm-rt.h:86` |

---

## 1. Locked design decisions (2026-09-07)

| # | Decision | Choice | Rationale |
|---|---|---|---|
| K1 | First capability after stabilization | **Imports / host interop** | Prerequisite for nearly all real modules (anything touching a HAL/driver/SDK); gates OTA logic patches and scripting FFI |
| K2 | Import binding | **Symbol contract** | Host exports wasm2c-named symbols; loader resolves at load; tool generates the exact host header. Zero overhead, zero glue, zero runtime RAM — matches "udynlink is just a linker" |
| K3 | Instance model | **Singleton default, instances opt-in** | Keep today's DX (`add(1,2)` just works); `--instances` generates full create/destroy + instance-taking wrappers for per-connection/fleet state |
| K4 | Default linear-memory model | **Static (.bss)** | Deterministic, zero-malloc, heap-less-firmware friendly; RAM cost visible in module header; all modes available via `--memory=` |
| K5 | Trap recovery | **Both mechanisms**: host-registered recovery point (primary) + baked wrapper recovery (`--wrappers-recover`, opt-in) | Host-decided at runtime as primary; wrapper flavor for hosts wanting plain calls; fatal loop remains the zero-cost default |

Principle applied throughout: **host decides > build-time flag > never hardcode**. A feature is only baked into a module when the mechanism cannot live host-side. Every feature costs exactly zero when unused.

---

## 2. Design principles & threat model

### Principles

1. **Unopinionated, usage-agnostic** (repo charter): no imposed lifecycle, allocator, or trap policy. Defaults exist to make simple things simple; flags exist so users make the tradeoffs.
2. **Fail loudly**: no silent guessing. Unsupported wasm features (sret/multi-value exports, memory64, multi-memory in static mode) produce a named error with a fix hint, never a silently degraded module.
3. **Zero-cost when unused**: every optional feature compiles out completely when disabled.
4. **One source of truth for the runtime**: `udynlink/wasm2c_runtime/`. Test dirs consume it; never vendored copies.
5. **Every phase ships its test through the script**: a feature isn't done until a QEMU test builds *via `mkwasm2c-module`* and passes. (D1–D3 existed precisely because CI tested around the script.)

### Threat model (explicit, per project owner)

- **Primary concern: defective / misimplemented modules must not take down the whole system** — recoverable traps, stack-depth limits, and allocation-failure handling. **All containment is optional**, decided by the host (runtime hooks / registered recovery point) or at build time (flags), never imposed.
- **Not a goal: defense against malicious code.** wasm2c/udynlink is a native-code plugin mechanism with wasm-derived memory safety *within linear memory accesses* (bounds checks are mandatory and always on). It is **not** an interpreter-grade sandbox: module code runs on the host C stack, calls host imports with full privilege, and any memory-safety bug in wasm2c-emitted code is native code. Hosts needing hostile-code isolation should use an interpreter runtime (WAMR/wasm3) instead; the docs will say this plainly.
- Free partial isolation retained regardless of flags: wasm linear memory is bounds-checked (module cannot address outside its memory), and module code is PIC with r9-based data access.

---

## 3. Architecture v2

### 3.1 Build pipeline

```
foo.wat ──wat2wasm──▶ foo.wasm
                         │
                mkwasm2c-module  (pinned wabt, version-checked)
                         │
        ┌────────────────┼──────────────────────────┐
        ▼                ▼                          ▼
  wasm2c → foo.c/.h   wasm_rt_config.h          (optional) host header
  (patched: NDEBUG,     (generated: memory model,   foo_imports.h: required
   string builtins)      page size, trap flags,      host symbols + exact
                         hook overrides)             C prototypes
        └────────────────┴──────────────────────────┘
                         │
              mkmodule (udynlink flags + wasm2c-mandated flags)
                         │
              mod_foo.bin (+ optional mod_foo_module_data.h)
```

Config header fixes D1/D3 at the root: `wasm-rt.h` picks it up via
`#if __has_include("wasm_rt_config.h")`, so the defines reach **every TU**
(runtime, generated code, shim) instead of only the shim.

### 3.2 Import binding: symbol contract (K2)

- Script parses `/* import: '<env>' '<name>' */` declarations from generated header.
- The **shim defines `struct w2c_env { void* user; }`** (env is embedder-defined by
  wasm2c's contract; the shim is the embedder) and passes it to
  `wasm2c_X_instantiate(&inst, &__wasm_env)`. Host may set `user` via a generated
  setter for per-instance context.
- Import functions (`w2c_<env>_<name>`) remain **undefined symbols** in the module;
  the loader binds them through the existing `udynlink_external_resolve_symbol`.
  Host firmware implements them with the exact wasm2c signatures (leading
  `struct w2c_env*` parameter, usually ignored).
- Tool emits **`<mod>_imports.h`** from the `.wasm`: required symbol names + exact
  C prototypes + a comment block documenting the contract. (Synergy: `mkhostsyms`
  builds the host's O(1) table; this header tells the host *what to put in it*.)
- No mangled glue, no indirection, no per-import RAM. Namespace coupling is
  wasm's own: distinct import module names (`env`, `wasi_*`, vendor names) give
  natural namespacing.
- Future option (not built now): `--import-glue` vtable dispatch, if multi-vendor
  name decoupling is ever needed.

### 3.3 Trap policy (K5)

Three-tier, host decides at runtime wherever possible:

1. **Fatal (default, zero-cost)**: `wasm_rt_trap` → optional weak
   `wasm_rt_trap_handler` hook (D3 fixed: wired via config macro override) →
   `bkpt` loop. For hosts that treat module faults as system faults.
2. **Host-registered recovery (primary containment, opt-in)**:
   - Module built with `--recoverable-traps`: runtime references `longjmp`
     (resolved from host like any import; `<setjmp.h>` used header-only) —
     measured cost on Cortex-M4: **32 B text + 164 B bss per registered context**.
   - Host API: `wasm_rt_set_recovery(jmp_buf*)` before the call;
     `wasm_rt_last_trap()` / `wasm_rt_strerror()` after. If a trap fires with no
     recovery point registered → falls back to fatal tier.
   - Works with prebuilt modules; the decision is the host's, per call site, at
     runtime.
3. **Baked wrapper recovery (opt-in `--wrappers-recover`)**: generated export
   wrappers `setjmp` internally and return an error sentinel on trap
   (0 / NULL / void-return). Documented ambiguity: sentinel values are
   indistinguishable from real results; hosts wanting clean error channels use
   tier 2. One shared `jmp_buf` in the shim.

Related containment (all optional, build-time):
- `--stack-depth-limit=N`: enables `WASM_RT_USE_STACK_DEPTH_COUNT` with max N →
  wasm recursion becomes `TRAP_EXHAUSTION` (recoverable) instead of a silent
  native stack overflow (D11). ~3 instructions per call when enabled.
- Allocation failure during instantiate → new `WASM_RT_TRAP_OOM` code → same
  three-tier policy. (wasm2c's `*_instantiate` returns void, so trap is the only
  channel; static mode cannot fail.)

### 3.4 Linear memory models (K4)

`--memory=static|dynamic|external`, default **static**; auto-refinement: a module
with `memory.grow` always builds dynamic unless the user forces static (grow then
fails at runtime, as today).

| Mode | Mechanics | Host obligation | RAM story |
|---|---|---|---|
| `static` (default) | Buffer in module `.bss` (`WASM_RT_INITIAL_PAGES` × `WASM_RT_PAGE_SIZE`) | none — no allocator needed | full initial size in module RAM footprint (host sizes from header); grow impossible |
| `dynamic` | `wasm_rt_malloc/realloc/free` hooks (default → `udynlink_external_*`) | host allocator | small image; grow up to max_pages |
| `external` | host passes buffer before first call: `mod_set_memory(void* buf, size_t bytes)`; `wasm_rt_allocate_memory` validates `bytes >= initial_pages × page_size` | carved arena, DMA-capable RAM, MPU region, shared pool | host controls placement; grow supported if host manages the buffer (re-set after grow) |

- **Custom page size**: `--custom-page-size=N` shrinks `WASM_RT_PAGE_SIZE`
  (module must not rely on 64 KiB addresses; tool prints the resulting memory
  size). Toolchain-native custom page sizes (wat2wasm support) preferred once
  available; the runtime define remains the portable mechanism.
- Table allocations always come from the same hook family as the selected mode;
  zeroed after allocation (D5); failure → `WASM_RT_TRAP_OOM`.

### 3.5 Instance model (K3)

- **Default (singleton)**: as today — `static w2c_X __wasm_instance;` +
  lazy `__wasm_ensure_instantiated()` + bare wrappers named after the wasm
  exports. One instance per loaded module copy.
- **`--instances`**: generates
  `mod_inst_t* mod_create(void* mem_buf)` / `void mod_destroy(mod_inst_t*)` and
  instance-taking wrappers (`u32 add(mod_inst_t*, u32, u32)`).
  `mem_buf` is required for `external` memory, optional (NULL → dynamic) for
  dynamic; **`static` memory is rejected in `--instances` builds** (per-instance
  `.bss` cannot be baked — fail loudly with the two supported alternatives).
- State isolation test proves two instances have independent globals/memory.

### 3.6 Export wrappers & symbol-table policy (D6, D9)

- Wrappers named after wasm exports (nice DX), with **collision detection**:
  reject/warn on names colliding with libc memfuncs, `wasm_rt_*`, `wasm2c_*`,
  `w2c_*`; `--wrapper-prefix` as escape hatch.
- Pointer-returning exports parsed and wrapped (D6); sret/multi-value signatures
  → named build error (until Phase 4 multi-value support), never silent.
- **Default `--public-symbols` = wrapper names only** (+ module-name entry).
  `--export-all` restores today's behavior for debugging (symtab bloat becomes
  an informed choice).
- Singleton internals (`__wasm_ensure_instantiated`) stay `static` — unreachable
  from the symtab, closing the "host bypasses lazy-init" hole.

### 3.7 Compile flags (D7)

Always added for wasm2c-generated sources:
`-fno-optimize-sibling-calls -frounding-math` (wasm2c requirements), on top of
udynlink's PIC flags. Never "relaxed" for tail-calls (see Appendix — tail-call
support, if ever added, must use wasm2c's tailcallee machinery, not flag
relaxation). Target/float-ABI notes: soft-float module on hard-float host is
compatible but slower; `--target cortex-m4f` etc. selects hard-float; arch_tag
gate applies as for C modules.

### 3.8 Toolchain hygiene

- **wabt pinning**: `just setup-wabt` downloads a pinned wabt release into
  `tools/` (mirroring `setup-qemu`); `mkwasm2c-module` parses `wasm2c --version`,
  errors on missing, warns below minimum (emit details are load-bearing for the
  script's parsers); CI uses the pinned version.
- `os.system` string-building replaced with the repo's `execute` util +
  argument lists; compiler/wasm2c stderr passed through on failure;
  `--workdir`/`--keep` for debugging intermediates.

## 4. Feature flag matrix

Build-time flags (baked per module; all default to zero-cost):

| Flag | Default | Cost when off | Cost when on |
|---|---|---|---|
| `--memory=static\|dynamic\|external` | `static` (auto→`dynamic` if grow) | — | per mode (§3.4) |
| `--custom-page-size=N` | 65536 | 0 | smaller linear memory |
| `--recoverable-traps` | off | 0 | ~32 B text + `longjmp` import; 164 B bss per registered recovery point (host-side) |
| `--wrappers-recover` | off | 0 | setjmp per wrapper entry + one shared `jmp_buf` (164 B bss) |
| `--stack-depth-limit=N` | off | 0 | ~3 instr/call + 4 B global |
| `--instances` | off | 0 | create/destroy + instance-taking wrappers |
| `--trap-handler=NAME` | none | 0 | call per trap |
| `--malloc=NAME` / `--free=NAME` | `udynlink_external_*` | 0 | — |
| `--public-symbols=...` / `--export-all` | wrappers only | 0 | symtab size (informed choice) |
| `--wrapper-prefix=PFX` | none | 0 | — |
| `--gen-imports-header` (auto with `--gen-c-header`) | auto | 0 | host header file |

Runtime decisions (host-side, work with prebuilt modules):

| Host API | Default | Effect |
|---|---|---|
| `wasm_rt_set_recovery(jmp_buf*)` | none registered | trap → `longjmp` to caller + `wasm_rt_last_trap()`; without it → fatal tier |
| `wasm_rt_trap_handler` (weak) | no-op | logging/policy hook on the fatal path |
| `wasm_rt_malloc/mem_free/mem_realloc` (weak) | `udynlink_external_*` | allocator policy, static pools |
| `mod_set_memory(buf, bytes)` (external mode) | required before first call | buffer placement/pools/MPU regions |

## 5. Roadmap

Ordering per K1: stabilize → imports → traps → grow → instances. Docs and CI are
**per-phase acceptance criteria**, not a final phase (v1's ordering allowed
broken flags to ship green).

### Phase 2.1 — Stabilize the tool (gate for everything)

> **Status: implemented (2026-09-07).** All D1–D9/D11 fixes in; wasm QEMU tests green through
> the script on MPS2-AN386 + STM32F429 (both opt levels); 22 script unit tests in
> `tests/py/test_mkwasm2c_module.py`; `just setup-wabt` pins wabt 1.0.34; docs + AGENTS.md updated.
> Remaining for follow-up: run full `just ci`, then start Phase 2.2 (imports).
Fixes: D1 (config header, all TUs), D3 (wire trap handler via config), D4
(realloc old-size: hook becomes `(ptr, old_size, new_size)`; fallback copies
`old_size`), D5 (zero tables, NULL checks → `WASM_RT_TRAP_OOM`), D6 (pointer
exports; loud errors for sret/multi-value), D7 (wasm2c-mandated flags), D8
(header path resolved before chdir), D9 (default public symbols), D10 partial
(runtime single-source: test dirs include canonical runtime via build flags, no
copies), D11 (`--stack-depth-limit`), toolchain hygiene (§3.8), collision
detection (§3.6).

New tests (pytest, no QEMU needed): config-header generation per flag combination;
export parsing incl. pointer returns; collision rejection; memory-mode selection
(static/dynamic/external/grow-auto); header-path regression.

**Acceptance:** all three existing wasm2c QEMU tests still green (unchanged
`.wat` sources, now built *through the script* on CI); script unit tests green;
`just ci` unaffected for non-wasm tests.

### Phase 2.2 — Imports via symbol contract (K2) — *first big capability*

- `--gen-imports-header` (+ auto with `--gen-c-header`): required host symbols,
  exact prototypes, contract docs.
- Shim: defines `struct w2c_env { void* user; }`, passes it to instantiate
  (singleton: internal env + `mod_set_env_user()`; instances: per `mod_create`).
- Runtime: no changes (imports are loader-resolved undefined symbols).
- Docs: host-side how-to (implement, resolve, `mkhostsyms` synergy).

New QEMU test `test-wasm2c-imports`: `.wat` with `(import "env" "host_add")`;
host implements `w2c_env_host_add`, asserts `calc(20,3) == 64`; all 3 load
modes; built through the script.

**Acceptance:** import module loads and calls host functions on MPS2-AN386 +
STM32F429, both opt levels; missing import → clean loader error naming the
symbol.

### Phase 2.3 — Trap policy & containment (K5, optional containment)

- Runtime: **new** host APIs `wasm_rt_set_recovery(jmp_buf*)` /
  `wasm_rt_last_trap()` (only `wasm_rt_strerror` exists today);
  `--recoverable-traps` builds reference `longjmp` (host-resolved); fatal
  fallback preserved.
- Script: `--wrappers-recover` (setjmp in wrappers, sentinel returns),
  `--stack-depth-limit=N`.
- New trap code `WASM_RT_TRAP_OOM` for allocation failure.
- Docs: three-tier policy, RTOS guidance (task-per-call isolation as an
  alternative), explicit "not a hostile-code sandbox" statement.

New QEMU test `test-wasm2c-trap`: `unreachable` + div-by-zero module; host
registers recovery, calls, asserts error code and continued execution; repeated
call after trap works (recovery point reset). Fatal tier covered by unit tests
of `wasm_rt_trap` control flow (QEMU cannot test an intentional hang).

**Acceptance:** trapped module returns error to host; device keeps running;
`--recoverable-traps` off → fatal (verified by inspection + unit test); size
delta measured and documented (~50–100 B + host-side jmp_buf).

### Phase 2.5 — Dynamic memory & `memory.grow` completion

- Realloc hook final shape (old_size passed); grow returns old page count;
  max_pages enforced; `external` grow = host re-set (`mod_set_memory`) or fails
  cleanly.
- QEMU test `test-wasm2c-grow`: module grows memory, writes beyond initial
  size, host allocator hooked to a static pool (proves heap-less viability).

**Acceptance:** grow test green via script on both QEMU platforms; OOB-read
regression test at TU level (realloc fallback copies exactly `old_size`).

### Phase 3 — Instance model (K3) + numeric coverage

- `--instances`: create/destroy, instance-taking wrappers, static-memory
  rejection with fix hint; env `user` per instance.
- QEMU test `test-wasm2c-instances`: two instances, independent globals and
  linear memory; destroy frees (dynamic) and create re-succeeds.
- Float test `test-wasm2c-float`: f32/f64 arithmetic + float→int conversions
  (validates D7 flags on hard-float target `olimex_stm32_h405` too); i64 export
  wrapper test (AAPCS r0:r1 return documented).

**Acceptance:** instance isolation green; float/i64 tests green on soft and
hard float targets.

### Phase 4 — Optional / deferred (explicitly not scheduled)

| Item | Trigger | Note |
|---|---|---|
| Metering / fuel | If untrusted-ish code ever matters | wasm→wasm gas-instrumentation pass (no upstream binaryen pass exists — checked v125); injected `gas` import rides the K2 contract. Backlog only. |
| Multi-value / sret exports | Demand | Export parser + wrapper codegen for struct returns |
| Tail-call | Demand | Must use wasm2c tailcallee machinery + verification; never relax sibling-call flag |
| Multi-memory | Demand | Static mode needs per-memory buffers; LOW per v1 matrix, keep LOW |
| Host-side MPU recipe | Doc-only | MPU region over linear memory at load (host's job; no per-access callbacks) |
| Toolchain custom page size | wabt support | Prefer toolchain-native over runtime define |

## 6. Testing strategy

1. **Script unit tests (pytest)** — every codegen decision (config header,
   shim shape, import header, collision checks, mode selection). Fast; runs
   without ARM toolchain. Guards against D1-class regressions permanently.
2. **QEMU integration** — `test_driver.py` learns wasm tests: `test_data.py`
   gains `"wasm": "foo.wat"`; driver invokes `mkwasm2c-module` (pinned wabt),
   then builds the host firmware against the generated `.bin`/header. All wasm
   tests therefore exercise the real script path on CI.
3. **Platform gate** — every wasm test runs MPS2-AN386 (mainline M4) +
   STM32F429 (legacy M4); float tests add `olimex_stm32_h405`.
4. **Parser fixtures** — wasm2c bins stay in the round-trip contract suite.

## 7. Cleanup (with Phase 2.1)

- Regenerate `tests/test-wasm2c-{add,fac,hello}` from committed `.wat` sources
  through the script; delete stale spike runtime copies, committed generated C,
  and redundant `udynlink_externals.h` copies (driver already passes
  `-I<repo>/udynlink`).
- `test-wasm2c-add` gains its `add.wat` (lost in v1 spike).
- README + `docs/` gain `docs/wasm2c-modules.md` (Phase 2.2 ships the first
  cut); AGENTS.md updated: toolchain requirements (wabt), public-headers table
  (`wasm2c_runtime/`), docs table.

## 8. Non-goals

- Hostile-code sandboxing, metering-as-default, threads/atomics, SIMD, GC,
  exceptions, memory64 (per v1 matrix — still correct), multi-memory beyond
  opt-in future work.
- Recoverable-trap-by-default (fatal stays default; containment is opt-in).
- Any udynlink core/loader changes: the entire roadmap lives in the wasm
  runtime + script + tests + docs.

## 9. Appendix — corrected wasm feature matrix (supersedes v1 §8)

| # | Feature | Priority | v2 assessment |
|---|---|---|---|
| 1 | Core MVP | ✅ done (verify in 2.1) | integer-only proven; float coverage owed |
| 2 | Bulk memory | HIGH | works via memfunc hooks; perf note: byte-loop fallbacks fine for small ops, word-wise copy worth it for large fills |
| 3 | Sign-extension, non-trapping f2i | HIGH | plain C; only needs the mandated flags (D7) + tests |
| 4 | Mutable globals | ✅ | instance struct; proven |
| 5 | Multi-value / sret | MEDIUM | blocked on export parser + wrapper codegen (Phase 4) |
| 6 | Tail-call | MEDIUM-risk | v1's "relax the flag" is wrong and unsafe; tailcallee machinery unverified on this pipeline |
| 7 | Custom page sizes | MEDIUM | runtime define works now (fix via config header); toolchain-native preferred later |
| 8 | Multi-memory | LOW | "runtime already supports" was false in v1; per-memory static buffers = real work |
| 9 | Imports | **HIGH (elevated)** | v1 had it in Phase 4; it gates most real modules → Phase 2.2 |
| 10 | Trap recovery / containment | **HIGH (new)** | absent from v1 matrix; the feature that separates demo from deployment |
| 11 | Metering | optional | backlog; only if threat model ever expands |
| 12 | SIMD / threads / EH / memory64 / GC / refs | skip | unchanged rationale; correct for Cortex-M |
