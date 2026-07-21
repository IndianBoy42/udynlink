# Thread Safety: Modules in Multithreaded Hosts

This guide covers the safety and conditions of using udynlink modules from a
multithreaded host (e.g. FreeRTOS, Zephyr) on a single-core Cortex-M MCU. It
answers two questions the general [Thread Safety and Concurrency][ts] section of
the host guide treats only briefly:

1. Is it safe for one thread running module **A** to be preempted by another
   thread that calls into module **B**?
2. When is it safe for **multiple threads to call into the same module**?

## The Two Worlds: Execution vs Lifecycle

Everything in udynlink splits cleanly into two categories with *different*
thread-safety rules. Conflating them is the single most common mistake.

| | **Execution** (calling module functions) | **Lifecycle** (load / unload / link / lookup) |
|---|---|---|
| **What it touches** | `r9` register, the module's own RAM region (LOT / `.data` / `.bss`) | The `udynlink_module_t` handle fields, the host module registry, the relocation table |
| **Shared mutable state in udynlink core?** | None during a call | Yes — `p_mod` fields are written during load and read by `lookup_symbol`/`unload` |
| **Thread-safe by design?** | Yes, with the `r9` model below | **No** — serialize with a mutex |
| **What breaks it** | Using raw `UDYNLINK_PREPARE_CALL` in reentrant contexts; sharing one module instance across threads without module-level locking | Any concurrent load/unload/lookup on the same handle |

The key insight: **executing loaded module code never re-enters the udynlink
core** (unless the module calls a host symbol that itself calls a loader API).
The core's only file-scope mutable is `debug_level`, which is never read on the
module-execution path. So execution safety is governed entirely by the `r9`
register and the module's own data — not by any library lock.

## The `r9` Model and Why Preemption Is Safe

udynlink modules are position-independent: every data access goes through a
Linker Offset Table (LOT) whose base address is held in register `r9`. Before
calling any module function the host sets `r9` to that module's `ram_base`
(`udynlink_module_t.ram_base`). Module code only ever reads `r9`-relative; it
never writes `r9`.

Two layers make `r9` correct across preemption:

### 1. `r9` is callee-saved (AAPCS) and FreeRTOS swaps it per-task

On Cortex-M, hardware exception entry auto-stacks only r0–r3, r12, lr, pc,
xPSR. The RTOS context-switch handler (PendSV) explicitly pushes and pops the
**full callee-saved set r4–r11** into each task's stack frame. So when Thread A
is preempted mid-execution in module A, `r9` (module A's `ram_base`) is saved
into A's task context; when A resumes, `r9` is restored to exactly that value —
regardless of what the preempting thread did to `r9` in the meantime.

> This is *stronger* than the bare-ISR case documented in the host guide. A raw
> ISR prologue only pushes r4–r11 that the ISR's own compiler frame uses; an RTOS
> task switch always swaps the complete set cooperatively.

### 2. `UDYNLINK_CALL` saves/restores `r9` on the caller's own stack

```c
/* udynlink/udynlink_call.h */
uint32_t _udynlink_prev_r9;
__asm volatile ("mov %0, r9" : "=r"(_udynlink_prev_r9) : :);  /* save  */
UDYNLINK_PREPARE_CALL((p_func)->p_mod);                        /* r9 = ram_base */
... call ...
__asm volatile ("mov r9, %0" :: "r"(_udynlink_prev_r9) : "r9"); /* restore */
```

`_udynlink_prev_r9` is a stack local — per-thread by construction. The
save/restore window is bounded and the restore is deterministic even if the
thread is preempted between the set and the call.

**Rule:** Always invoke module functions through `UDYNLINK_CALL` (or its C++
`udynlink::Func` / `Context` equivalents). Never call a module function via a
raw `sym.val` function pointer without first setting `r9`, and prefer
`UDYNLINK_CALL` over a bare `UDYNLINK_PREPARE_CALL` whenever reentrancy or
preemption is possible.

## Scenario 1 — Different Modules, Different Threads: Safe

```
Thread A: running module X  ──┐                    (one module per thread)
Thread B: running module Y  ──┘                    (X != Y, separate instances)
```

This is **safe at the execution level**:

- `r9` is per-task under the RTOS, so each thread carries the correct LOT base
  for its own module. Preemption between A and B causes no `r9` corruption.
- Module X's LOT / `.data` / `.bss` live at X's `ram_base`; module Y's at Y's
  `ram_base`. Each module's code accesses data only through its own `r9`-relative
  LOT slots. Two threads touching two modules touch two disjoint RAM regions —
  no shared mutable state through udynlink's machinery.
- `UDYNLINK_CALL` save/restores `r9` on each caller's stack, so even the window
  between setting `r9` and branching into the module is bounded.

**Conditions:**

1. Use `UDYNLINK_CALL` (never a raw `sym.val` call) for every module invocation.
2. The host symbol table / `udynlink_external_resolve_symbol` implementation is
   *your* code — if it touches shared host state from two threads, synchronize
   it in your host code. udynlink provides nothing here.
3. Application-level shared state reached via host symbols (e.g. both modules
   call a host `g_counter++`) is a normal multi-threaded data-race problem; use
   the RTOS's usual primitives.

## Scenario 2 — Same Module, Multiple Threads: Conditionally Safe

```
Thread A and Thread B both call functions in module M (one shared instance).
```

This is the case that requires care. `r9` is *not* the problem — it is still
per-task and each thread sets it before each call. The hazard is the module's
**own mutable data**.

A single loaded module instance has exactly one LOT, one `.data`, and one `.bss`
region, all at the same `ram_base`. If two threads concurrently execute code in
that one instance:

- **Read-only module functions** (pure computation, no writes to `.data`/`.bss`,
  no host calls with side effects) are safe to call concurrently — they share no
  mutable state through udynlink.
- **Functions that read/write module-level `static`/global variables** are a data
  race on the module's own `.data`/`.bss`. This is *not* a udynlink concern — it
  is ordinary shared-state concurrency in the module author's code. Protect it
  with a module-scoped mutex, make the function reentrant, or serialize access at
  the host level. udynlink cannot help: it has no visibility into which module
  functions are reentrant.
- **Functions that call host symbols with side effects** inherit the host's
  thread-safety for those symbols.

### Two supported patterns for the same-module case

**Pattern A — One instance per thread (recommended, zero shared state).**

Load a *separate* instance of the module for each thread. udynlink allows
multiple instances of the same module with no deduplication. Each instance gets
its own `ram_base`, so there is no shared `.data`/`.bss` and therefore no
module-level data race at all. Each thread uses `UDYNLINK_CALL` against its own
`udynlink_module_t`.

```c
udynlink_module_t mod_for_thread_a, mod_for_thread_b;  /* zero-init each */
/* load mod_for_thread_a and mod_for_thread_b from the same image ... */
/* Thread A: */ UDYNLINK_CALL(&fn_a, int, (x));   /* fn_a resolved from mod_for_thread_a */
/* Thread B: */ UDYNLINK_CALL(&fn_b, int, (y));   /* fn_b resolved from mod_for_thread_b */
```

Remember: load/unload of *each* instance must still be serialized (see Lifecycle
below).

**Pattern B — Shared instance + serialized access.**

If you must share one instance, treat the module's public API as a shared
resource and guard concurrent calls with a host-side mutex around the call:

```c
xSemaphoreTake(mod_m_mutex, portMAX_DELAY);
int r = UDYNLINK_CALL(&fn_m, int, (arg));
xSemaphoreGive(mod_m_mutex);
```

This serializes execution and removes the data race, at the cost of
throughput. Only the subset of the module API that touches mutable module state
needs the guard; read-only functions can run concurrently.

## Nested / Reentrant Calls

If module A invokes a host callback, and that callback calls into module B
(same thread or not), use `UDYNLINK_CALL` — never raw `UDYNLINK_PREPARE_CALL`.
`UDYNLINK_CALL` saves the caller's `r9` on the stack before setting B's `r9`
and restores it on return, so A resumes with the correct LOT base regardless of
preemption during the nested call. A bare `UDYNLINK_PREPARE_CALL(&mod_B)`
followed by a raw call would clobber `r9` and leave A with the wrong LOT base.

The `udynlink_thunk` optional layer (`udynlink_thunk_make_call()`) creates
callable function pointers that manage `r9` internally and are safe to pass as
callbacks to ISRs, RTOS timers, or third-party libraries that are unaware of
the `r9`/LOT convention.

## Lifecycle Operations: Always Serialize

Load, unload, and any post-load symbol/link operation are **not** thread-safe
and must be serialized with a mutex regardless of how modules are partitioned
across threads. The relevant APIs:

- `udynlink_load_module()` / `udynlink_load_module_image()` — write `p_mod` fields.
- `udynlink_unload_module()` — frees RAM and mutates `p_mod`.
- `udynlink_lookup_symbol()` — re-enters the core to read the symbol table and
  may call `udynlink_external_resolve_symbol` for weak symbols; races a
  load/unload on the same handle.
- `udynlink_link_symbol()`, `udynlink_link_incremental()`, `udynlink_relink_all()`
  — write relocation slots in module RAM.

Concurrent execution of these on the same `udynlink_module_t` (or one thread
loading a module while another looks up a symbol in it) will observe a
half-initialized handle and hard-fault.

> The host guide has ready-to-use FreeRTOS mutex wrappers for load/unload in its
> [RTOS (Mutex or Critical Section)][rtosmutex] section and a full statement of
> the [When You Need Protection][needprot] rules — apply them unchanged here.
> A mutex cannot be taken from ISR context; use a counting semaphore or
> `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` if you ever load from an ISR.

The cleanest design: confine **all** load/unload/link activity to a single
loader task. Threads that need to load modules post messages to that task
instead of calling the loader directly.

## Summary Table

| Scenario | Execution safe? | Conditions |
|---|---|---|
| One thread per module, different modules | **Yes** | Use `UDYNLINK_CALL`. Synchronize host-side shared state yourself. Serialize lifecycle ops. |
| Multiple threads, one instance per thread (same module image) | **Yes** | Each thread uses its own `udynlink_module_t` / `ram_base`. No shared module data. Serialize each load/unload. |
| Multiple threads, *shared* single instance | **Only if** the called functions are read-only for module state | Otherwise guard concurrent calls with a host mutex, or serialize at the host level. |
| Nested call (module A → host cb → module B) | **Yes** | Use `UDYNLINK_CALL` (or thunks). Never bare `UDYNLINK_PREPARE_CALL` for the nested call. |
| Concurrent load / unload / link / lookup on one handle | **No** | Serialize with a mutex; confine to one loader task where possible. |

## Thread-Safe Function-Local Statics (`__cxa_guard_*`)

C++ function-local `static` variables with non-trivial constructors are
guarded by the Itanium ABI [`__cxa_guard_acquire` / `__cxa_guard_release` /
`__cxa_guard_abort`][cxa-guard] trio so the constructor runs exactly once
across threads. `mkmodule` compiles modules with `-fno-threadsafe-statics`
by default, which lowers a local `static` to a plain byte flag with no
guard call — consistent with udynlink's "no thread safety inside the
library" stance. The same flag is applied to the host-side C++ helper in
`tests/platforms/stm32f429_discovery/platform.cmake`.

A host that *does* want interlocked first-time initialization for a
specific module (because it will call that module concurrently from
multiple threads on a shared instance) can opt back in **per module**:

```bash
python3 mkmodule --build-flags=-fthreadsafe-statics  mod_foo.cpp
```

`mkmodule` appends `--build-flags` **after** its built-in `-fno-*`
defaults, and GCC honors the *last* occurrence of a toggle-style flag,
so this re-enables the `__cxa_guard_*` calls in that module only.
(Verified: the same probe object built with `-fthreadsafe-statics`
appended last emits `U __cxa_guard_acquire` / `U __cxa_guard_release`;
the default build does not.) The same last-flag-wins trick overrides
other defaults too — for instance `--build-flags="-fthreadsafe-statics
-fexceptions"` would re-enable both, but re-enabling exceptions without
an `__cxa_*` runtime to back them is almost never what you want on
bare metal.

When you opt in, the module's LOT now contains `external` slots for
`__cxa_guard_acquire` and `__cxa_guard_release` (`__cxa_guard_abort` is
only referenced if the constructor throws, which under `-fno-exceptions`
does not happen). The host must resolve them. `udynlink_cpp_abi.h` does
**not** ship guard stubs — they are a synchronization primitive, and a
correct implementation is host- and RTOS-specific. Provide your own in
`udynlink_external_resolve_symbol`:

```cpp
extern "C" int  __cxa_guard_acquire(volatile int *g);   /* return 1 → run ctor */
extern "C" void __cxa_guard_release(volatile int *g);   /* marker: ctor done */
extern "C" void __cxa_guard_abort(volatile int *g);     /* ctor threw: re-arm */

uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *m,
                                           const char *name) {
    if (!strcmp(name, "__cxa_guard_acquire")) return (uintptr_t)&__cxa_guard_acquire;
    if (!strcmp(name, "__cxa_guard_release")) return (uintptr_t)&__cxa_guard_release;
    if (!strcmp(name, "__cxa_guard_abort"))   return (uintptr_t)&__cxa_guard_abort;
    /* …host symbols, udynlink_cpp_resolve_abi_symbol(name), … */
    return 0;
}
```

A minimal interlocked implementation on Cortex-M uses a bit-test-and-set
on the guard byte under a critical section:

```cpp
extern "C" int __cxa_guard_acquire(volatile int *g) {
    int took;
    taskENTER_CRITICAL();
    took = (*g & 1) ? 0 : (*g |= 1, 1);   /* win the race → 1 */
    taskEXIT_CRITICAL();
    return took;                            /* 0 → already done, skip ctor */
}
extern "C" void __cxa_guard_release(volatile int *g) { (void)g; }
extern "C" void __cxa_guard_abort(volatile int *g)   { *g = 0; }
```

The 32-bit guard word has GCC-private semantics beyond bit 0, so leave
the other bits alone. The Cortex-M `ldrb`/`strb` bit-0 dance above is
what GCC's `-fthreadsafe-statics` lowering expects.

**Two cautions that flow from the rest of this guide:**

1. The guard only makes the *first-time* initialization race-free. The
   constructor body itself, and every subsequent call into the now-initialized
   static, still races on the module's `.data`/`.bss` exactly as described in
   [Scenario 2 — Same Module, Multiple Threads](#scenario-2--same-module-multiple-threads-conditionally-safe).
   Pattern A (one instance per thread) remains the zero-race option.
2. The guard stubs are host functions. If they call into a module, the
   [nested-call](#nested--reentrant-calls) rules apply: route through
   `UDYNLINK_CALL`, never a bare `UDYNLINK_PREPARE_CALL`.

### Interaction with `--strip-mangled-syms`

The ABI externals `_ZdlPv`, `_Znwj`, `__cxa_pure_virtual`,
`__cxa_guard_acquire`, `__cxa_guard_release` are SHN_UNDEF references —
`mkmodule` classifies them as `external`, and `--strip-mangled-syms`
only demotes *defined* mangled symbols (vtables `_ZTV…`, typeinfo
`_ZTI…`, fully-built user functions). Pairing `--strip-mangled-syms`
with `--public-symbols` keeps the intended exports alive while the
operator-delete / guard / pure-virtual slots stay `external` and are
still resolved by the host at load time — verified on the standard
C++ new/delete and vtable/destructor probes. So the strip flag does
not interact with the guard opt-in.

[cxa-guard]: https://itanium-cxx-abi.github.io/cxx-abi/abi.html#guards

## Further Reading

- [Integrating as a Host — Thread Safety and Concurrency][ts] — bare-metal and
  FreeRTOS mutex wrappers for lifecycle operations; the `r9`/ISR analysis.
- [How It Works — `r9` and the LOT][lot] — the position-independent code model
  that underpins these guarantees.
- [`udynlink_call.h`](../udynlink/udynlink_call.h) — `UDYNLINK_CALL`,
  `UDYNLINK_PREPARE_CALL`, and the `udynlink_func_t` handle.
- [`udynlink_thunk.h`](../udynlink/udynlink_thunk.h) — callable thunks that
  remove `r9` management from callback sites.

[ts]: integrating-as-host.md#thread-safety-and-concurrency
[rtosmutex]: integrating-as-host.md#rtos-mutex-or-critical-section
[needprot]: integrating-as-host.md#when-you-need-protection
[lot]: how-it-works.md#the-linker-offset-table-lot
