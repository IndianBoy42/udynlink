# Rust Host Support for udynlink — Refined Plan

**Status:** Refined — Ready for review
**Last updated:** 2026-06-09

---

## 1. Goal

Provide a Rust crate that wraps the C udynlink library, enabling Rust firmware to:
- Load udynlink binary modules (produced from C/C++ — and later Rust)
- Look up exported symbols by name
- Call exported functions safely with automatic r9/LOT management
- Manage module lifetime (load/unload with RAII)
- Optionally use the thunk, deps, hash, and host-utils layers

**Scope:** Host-side integration only. Does NOT create Rust modules (see `rust-module.md`).

**Focus:** Rust host loading C modules. The Rust host is a `no_std` ARM Cortex-M firmware.

---

## 2. What the Draft Got Wrong — Corrections

| Draft item | Actual C API | Impact on Rust design |
|------------|--------------|----------------------|
| `udynlink_module_t` has `num_deps`, `dep_refcount`, `deps[4]` | `udynlink_module_t` has: `p_header`, `union { p_ram; ram_base }`, `info` (u8), `reserved` (u8), `reserved2` (u16), `num_named_syms` (u16), `user_ctx` (*mut c_void) | Must bind the real struct — 24 bytes with a union |
| `UDYNLINK_LOT_BASE_ADDR` global that gets written | **No such thing.** The C API uses `r9` (a CPU register) set via inline asm `UDYNLINK_PREPARE_CALL(p_mod)` | LOT-base call guard works via `mov r9`, not memory-mapped I/O |
| `resolve_critical_symbol` and `get_module_handle` callbacks | Only 5 externals: `is_pointer_in_ram`, `malloc`, `free`, `vprintf`, `resolve_symbol` (takes `p_mod` + `name`) | Simpler trait — 5 methods, not 7. `resolve_symbol` receives the module handle |
| `LotGuard` reads/writes a memory address | Must save/restore `r9` via inline asm, same as C++ `Context` class | Guard uses `asm!` not volatile reads |
| `load_from_memory` uses `MaybeUninit` | C API requires zero-initialized `udynlink_module_t` before first call | Must `memset(0)` / `zeroed()` the struct first |
| `udynlink_hash.c` compiled in `build.rs` | `udynlink_hash.h` is header-only (inline functions) | No `.c` file to compile for hash; it's feature-gated headers only |
| Plan ignores `udynlink_call.h` | Provides `udynlink_func_t`, `udynlink_resolve_func()`, `UDYNLINK_CALL` | Rust `Func<Sig>` should mirror this — amortized lookup + r9 save/restore |
| Plan ignores `udynlink.hpp` | Provides `Module`, `Func<Sig>`, `Context` (RAII r9) | The C++ API is the direct design precedent for the Rust API |
| `udynlink_load_module_image` not mentioned | Exists for non-contiguous images | Must bind the full loading API |
| Incremental linking / `udynlink_link_symbol` not mentioned | Core API: `udynlink_link_incremental`, `udynlink_relink_all`, `udynlink_link_symbol`, `udynlink_is_symbol_resolved` | Must expose for deferred-symbol workflows |
| `VAddr` / `VaList` in core | `core::ffi::VaList` is unstable; C `va_list` is tricky from Rust | Use a `*mut c_void` or a custom VaList wrapper; or just ignore vprintf (weak no-op default) |

---

## 3. Architecture

```
┌──────────────────────────────────────────────────────┐
│  udynlink (safe crate, no_std)                       │
│                                                      │
│  Module          — RAII load/unload, symbol lookup   │
│  Func<Sig>       — typed function handle, r9 guards  │
│  Context         — RAII r9 scope for repeated calls  │
│  LoadMode        — enum wrapping udynlink_load_mode_t│
│  Error           — enum from udynlink_error_t        │
│  Symbol          — untyped symbol descriptor          │
│  Image           — builder for non-contiguous images  │
│                                                      │
│  Externals trait — host callbacks (5 methods)        │
│                                                      │
│  [feature: thunk]  ThunkPool, make_call()            │
│  [feature: deps]    DepManager, dep_load/unload      │
│  [feature: hash]    HashTable (inline, from header)  │
│  [feature: alloc]   String returns, Vec-backed deps  │
│                                                      │
├──────────────────────────────────────────────────────┤
│  udynlink-sys (raw FFI, no_std)                      │
│  - extern "C" bindings for ALL public functions      │
│  - #[repr(C)] structs matching C layout              │
│  - #[no_mangle] extern "C" callback trampolines       │
│  - build.rs compiles udynlink.c (+ thunk.c, deps.c) │
├──────────────────────────────────────────────────────┤
│  C library (udynlink.h + .c)                         │
│  Core: udynlink.c                                    │
│  Optional: udynlink_thunk.c, udynlink_deps.c         │
│  Header-only: udynlink_call.h, udynlink_hash.h,     │
│               udynlink_host_utils.h                  │
└──────────────────────────────────────────────────────┘
```

**Key insight:** The existing C++ API (`udynlink.hpp`) is the direct design precedent. The Rust API should mirror it closely:
- `udynlink::Module` ≈ C++ `udynlink::Module` (RAII, movable, non-copyable)
- `Func<Sig>` ≈ C++ `Func<R(Args...)>` (typed, resolved once, callable many times with r9 save/restore)
- `Context` ≈ C++ `Context` (RAII r9 scope for tight loops)

---

## 4. Phase 1: `udynlink-sys` — Raw FFI Crate

### 4.1 Crate Layout

```
crates/udynlink-sys/
├── Cargo.toml
├── build.rs
└── src/
    ├── lib.rs           # Re-exports + conditional compilation
    ├── types.rs         # #[repr(C)] structs, enums, constants
    ├── functions.rs     # extern "C" fn declarations
    └── externals.rs     # #[no_mangle] callback trampolines
```

### 4.2 `Cargo.toml`

```toml
[package]
name = "udynlink-sys"
version = "0.1.0"
edition = "2021"
links = "udynlink"

[build-dependencies]
cc = "1"

[features]
default = []
thunk = []
deps = ["thunk"]
hash = []
host-utils = []
```

### 4.3 `build.rs`

```rust
fn main() {
    let target = std::env::var("TARGET").unwrap();

    // Only meaningful for ARM Cortex-M; allow doc generation on host
    if !target.starts_with("thumb") && !target.starts_with("arm") {
        println!("cargo:warning=udynlink-sys: skipping C compilation for non-ARM target {target}");
        return;
    }

    let udynlink_dir = std::path::Path::new("../../udynlink");
    let mut build = cc::Build::new();
    build
        .file(udynlink_dir.join("udynlink.c"))
        .include(udynlink_dir)
        .compiler("arm-none-eabi-gcc")  // explicit; cc auto-detection is unreliable for bare-metal
        .flag("-mcpu=cortex-m4")        // default; overridden via env vars below
        .flag("-mthumb")
        .flag("-nostdlib")
        .flag("-ffreestanding");

    // Let users override the CPU via env var
    if let Ok(mcpu) = std::env::var("UDYNLINK_MCPU") {
        build.flag(&format!("-mcpu={mcpu}"));
    }

    // Host arch tag
    if let Ok(arch_tag) = std::env::var("UDYNLINK_HOST_ARCH_TAG") {
        build.define("UDYNLINK_HOST_ARCH_TAG", &arch_tag);
    }

    // Debug level
    if let Ok(level) = std::env::var("UDYNLINK_DEBUG_LEVEL") {
        build.define("UDYNLINK_DEBUG_LEVEL", &level);
    }

    // Optional features: compile additional C sources
    // NOTE: Cargo features are cfg() on the *sys* crate being built,
    // so we use the DEP_* env vars that Cargo sets or check the feature
    // flags that were passed. Since this is build.rs, we can check
    // env vars or use a different mechanism.

    // Feature-gated C compilation: the cc crate doesn't support
    // cfg!(feature) in build.rs for the *current* crate, so we
    // use Cargo:env var UDYNLINK_SYS_FEATURES or just always compile
    // (linker will strip unused). For size-sensitive builds, the
    // user can set env vars to exclude C files.

    if std::env::var("CARGO_FEATURE_THUNK").is_ok() || std::env::var("CARGO_FEATURE_DEPS").is_ok() {
        build.file(udynlink_dir.join("udynlink_thunk.c"));
    }
    if std::env::var("CARGO_FEATURE_DEPS").is_ok() {
        build.file(udynlink_dir.join("udynlink_deps.c"));
    }

    build.compile("udynlink");
}
```

### 4.4 Type Bindings (`types.rs`)

All structs must match the C layout exactly. Key points:

```rust
use core::ffi::{c_void, c_char, c_int};

// ── Header ──

/// Module image header (32 bytes).
/// Must match `udynlink_module_header_t` exactly.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct UdynlinkModuleHeader {
    pub sign: u32,
    pub mod_version: u16,
    pub udynlink_version: u16,
    pub arch_tag: u16,
    pub num_lot: u16,
    pub num_rels: u16,
    pub reserved: u16,
    pub symt_size: u32,
    pub code_size: u32,
    pub data_size: u32,
    pub bss_size: u32,
}

// ── Module handle ──

/// Runtime module handle.
///
/// The C struct uses an anonymous union for p_ram/ram_base.
/// In Rust we expose this as two accessor methods on a #[repr(C)]
/// wrapper that holds the union.
#[repr(C)]
pub struct UdynlinkModule {
    pub p_header: *const UdynlinkModuleHeader,
    /// Anonymous union: p_ram (pointer) and ram_base (uintptr_t).
    pub ram: UdynlinkModuleRam,
    pub info: u8,
    pub reserved: u8,
    pub reserved2: u16,
    pub num_named_syms: u16,
    pub user_ctx: *mut c_void,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union UdynlinkModuleRam {
    pub p_ram: *mut c_void,
    pub ram_base: usize,
}

// ── Symbol ──

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct UdynlinkSym {
    pub name: *const c_char,
    pub val: usize,
    pub type_: u8,
    pub location: u8,
}

// ── Image descriptor ──

#[repr(C)]
pub struct UdynlinkModuleImage {
    pub p_header: *const UdynlinkModuleHeader,
    pub p_relocations: *const u32,
    pub p_symtab: *const u32,
    pub p_code: *const u8,
    pub p_data: *const u8,
}

// ── Enums ──

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UdynlinkLoadMode {
    CopyAll = 0,
    CopyTextData = 1,
    Xip = 2,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UdynlinkError {
    Ok = 0,
    LoadInvalidSign,
    LoadRamLenLow,
    LoadOutOfMemory,
    LoadXipUnsupported,
    LoadInvalidMode,
    LoadBadRelocationTable,
    LoadUnknownSymbol,
    LoadDuplicateName,
    LoadVersionMismatch,
    LoadArchMismatch,
    LoadIoError,
    InvalidModule,
    LoadHookAborted,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UdynlinkDebugLevel {
    None = 0,
    Error,
    Warning,
    Info,
}

// ── Constants ──

pub const UDYNLINK_SYM_DEFERRED: usize = 1;

pub const UDYNLINK_SYM_TYPE_INTERNAL: u8  = 0;
pub const UDYNLINK_SYM_TYPE_EXPORTED: u8 = 1;
pub const UDYNLINK_SYM_TYPE_EXTERN: u8   = 2;
pub const UDYNLINK_SYM_TYPE_MODULE_NAME: u8 = 3;
pub const UDYNLINK_SYM_TYPE_WEAK: u8     = 4;

pub const UDYNLINK_SYM_LOCATION_CODE: u8 = 0;
pub const UDYNLINK_SYM_LOCATION_DATA: u8 = 1;

// Arch tags
pub const UDYNLINK_ARCH_TAG_CORTEX_M0: u16     = 0x01;
pub const UDYNLINK_ARCH_TAG_CORTEX_M0PLUS: u16 = 0x02;
pub const UDYNLINK_ARCH_TAG_CORTEX_M3: u16     = 0x03;
pub const UDYNLINK_ARCH_TAG_CORTEX_M4: u16     = 0x04;
pub const UDYNLINK_ARCH_TAG_CORTEX_M4F: u16    = 0x54;
pub const UDYNLINK_ARCH_TAG_CORTEX_M7: u16     = 0x57;
pub const UDYNLINK_ARCH_TAG_CORTEX_M33: u16    = 0x08;
pub const UDYNLINK_ARCH_TAG_CORTEX_M55: u16    = 0x59;
pub const UDYNLINK_ARCH_TAG_CORTEX_M85: u16    = 0x5A;

// Arch helpers (mirrors C macros)
pub const UDYNLINK_ARCH_FAMILY_MASK: u16     = 0x0F;
pub const UDYNLINK_ARCH_FPU_MASK: u16        = 0x10;
pub const UDYNLINK_ARCH_FLOAT_ABI_MASK: u16  = 0x60;
pub const UDYNLINK_ARCH_FLOAT_ABI_SHIFT: u16 = 5;
pub const UDYNLINK_ARCH_FLAG_NO_PROLOGUE: u16 = 0x80;

// ── udynlink_call.h (inline in C, re-implemented in safe layer) ──

/// Reusable function handle (mirrors udynlink_func_t).
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct UdynlinkFunc {
    pub p_mod: *const UdynlinkModule,
    pub addr: usize,
    pub name: *const c_char,
}

// ── udynlink_thunk.h types (feature = "thunk") ──

pub const UDYNLINK_GATEWAY_SIZE: usize = 18;
pub const UDYNLINK_STUB_SIZE: usize    = 10;

/// Thunk pool (mirrors udynlink_thunk_pool_t).
#[repr(C)]
pub struct UdynlinkThunkPool {
    pub base: *mut u8,
    pub size: usize,
    pub used: usize,
    pub gateway_top: usize,
}

// ── udynlink_deps.h types (feature = "deps") ──

pub const UDYNLINK_DEP_PREFIX: &[u8; 23] = b".udynlink.mod.requires.";
pub const UDYNLINK_DEP_PREFIX_LEN: usize = 23;
pub const UDYNLINK_DEP_MAX_DEPTH: usize   = 8;

/// Per-module entry in the dependency manager.
#[repr(C)]
pub struct UdynlinkDepEntry {
    pub p_mod: *mut UdynlinkModule,
    pub gateway: *mut u8,
}

/// Dependency manager state.
#[repr(C)]
pub struct UdynlinkDepMgr {
    pub entries: *mut UdynlinkDepEntry,
    pub count: usize,
    pub capacity: usize,
    pub loading_stack: [*const c_char; UDYNLINK_DEP_MAX_DEPTH],
    pub loading_depth: usize,
}

// ── udynlink_hash.h types (feature = "hash") ──

/// GNU hash table (mirrors udynlink_hash_table_t).
#[repr(C)]
pub struct UdynlinkHashTable {
    pub nbuckets: usize,
    pub symoffset: usize,
    pub bloom_size: usize,
    pub bloom_shift: usize,
    pub bloom: *const u32,
    pub buckets: *const u32,
    pub hash_values: *const u32,
    pub sym_addrs: *const usize,
    pub strtab: *const c_char,
    pub strtab_offsets: *const usize,
}

// ── udynlink_host_utils.h types (feature = "host-utils") ──

pub const UDYNLINK_HOST_SYM_CACHE_SIZE: usize = 16;

/// Host symbol cache entry.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UdynlinkHostSymCacheEntry {
    pub name: *const c_char,
    pub addr: usize,
}
```

### 4.5 Function Bindings (`functions.rs`)

All public C functions from `udynlink.h`, `udynlink_call.h` (the inline ones are re-implemented in the safe layer):

```rust
extern "C" {
    // Image builders
    pub fn udynlink_image_from_memory(base_addr: *const c_void, out_image: *mut UdynlinkModuleImage);
    pub fn udynlink_image_from_module(p_mod: *const UdynlinkModule, out_image: *mut UdynlinkModuleImage);

    // Planning / validation
    pub fn udynlink_check_arch_tag(mod_arch: u16, host_arch: u16) -> UdynlinkError;
    pub fn udynlink_validate_header(header: *const UdynlinkModuleHeader) -> UdynlinkError;
    pub fn udynlink_compute_ram_size(header: *const UdynlinkModuleHeader, mode: UdynlinkLoadMode) -> usize;
    pub fn udynlink_get_image_metadata_size(header: *const UdynlinkModuleHeader) -> usize;
    pub fn udynlink_image_get_module_name(p_symtab: *const u32) -> *const c_char;

    // Module loading
    pub fn udynlink_load_module(
        p_mod: *mut UdynlinkModule,
        base_addr: *const c_void,
        load_addr: *mut c_void,
        load_size: usize,
        load_mode: UdynlinkLoadMode,
    ) -> UdynlinkError;

    pub fn udynlink_load_module_image(
        p_mod: *mut UdynlinkModule,
        image: *const UdynlinkModuleImage,
        load_addr: *mut c_void,
        load_size: usize,
        load_mode: UdynlinkLoadMode,
    ) -> UdynlinkError;

    pub fn udynlink_load_apply_relocations(
        p_mod: *mut UdynlinkModule,
        p_header: *const UdynlinkModuleHeader,
        p_relocations: *const u32,
        p_symtab: *const u32,
    ) -> UdynlinkError;

    pub fn udynlink_unload_module(p_mod: *mut UdynlinkModule) -> UdynlinkError;
    pub fn udynlink_cpp_init(p_mod: *mut UdynlinkModule);

    // Query
    pub fn udynlink_get_ram_size(p_mod: *const UdynlinkModule) -> usize;
    pub fn udynlink_get_module_name(p_mod: *const UdynlinkModule) -> *const c_char;
    pub fn udynlink_get_module_name_from_image(base_addr: *const c_void) -> *const c_char;
    pub fn udynlink_lookup_symbol(
        p_mod: *const UdynlinkModule,
        name: *const c_char,
        p_sym: *mut UdynlinkSym,
    ) -> *mut UdynlinkSym;
    pub fn udynlink_get_symbol_value(p_mod: *const UdynlinkModule, name: *const c_char) -> usize;

    // Incremental / deferred linking
    pub fn udynlink_link_incremental(p_mod: *mut UdynlinkModule) -> UdynlinkError;
    pub fn udynlink_relink_all(p_mod: *mut UdynlinkModule) -> UdynlinkError;
    pub fn udynlink_link_symbol(p_mod: *mut UdynlinkModule, sym_name: *const c_char, sym_addr: usize) -> UdynlinkError;
    pub fn udynlink_is_symbol_resolved(p_mod: *const UdynlinkModule, sym_name: *const c_char) -> c_int;

    // Miscellaneous
    pub fn udynlink_error_msg(err: *mut UdynlinkError) -> *const c_char;
    pub fn udynlink_set_debug_level(level: UdynlinkDebugLevel);
    pub fn udynlink_get_image_size(base_addr: *const c_void) -> usize;
    pub fn udynlink_get_text_pointer(p_mod: *const UdynlinkModule) -> *mut u8;
    pub fn udynlink_get_ram_requirements(base_addr: *const c_void, mode: UdynlinkLoadMode) -> usize;
}

// ── udynlink_thunk.h functions (feature = "thunk") ──

#[cfg(feature = "thunk")]
extern "C" {
    pub fn udynlink_thunk_pool_init(pool: *mut UdynlinkThunkPool, buf: *mut u8, sz: usize);
    pub fn udynlink_thunk_alloc(pool: *mut UdynlinkThunkPool, n: usize) -> *mut c_void;
    pub fn udynlink_thunk_find_gateway(pool: *const UdynlinkThunkPool, ram_base: u32) -> *mut u8;
    pub fn udynlink_thunk_alloc_gateway(pool: *mut UdynlinkThunkPool, ram_base: u32) -> *mut u8;
    pub fn udynlink_thunk_alloc_stub(
        pool: *mut UdynlinkThunkPool,
        func_addr: u32,
        gateway: *const u8,
    ) -> usize;
    pub fn udynlink_thunk_write_gateway(dst: *mut u8, ram_base: u32);
    pub fn udynlink_thunk_write_stub(
        dst: *mut u8,
        func_addr: u32,
        gateway: *const u8,
    ) -> usize;
    pub fn udynlink_thunk_make_call(
        pool: *mut UdynlinkThunkPool,
        p_mod: *const UdynlinkModule,
        sym_name: *const c_char,
    ) -> usize;
}

// ── udynlink_deps.h functions (feature = "deps") ──

#[cfg(feature = "deps")]
extern "C" {
    pub fn udynlink_dep_mgr_init(mgr: *mut UdynlinkDepMgr, buf: *mut UdynlinkDepEntry, cap: usize);
    pub fn udynlink_dep_is_dependency(name: *const c_char) -> c_int;
    pub fn udynlink_dep_get_name(name: *const c_char) -> *const c_char;
    pub fn udynlink_dep_find(mgr: *mut UdynlinkDepMgr, name: *const c_char) -> *mut UdynlinkModule;
    pub fn udynlink_dep_resolve_dependency(mgr: *mut UdynlinkDepMgr, name: *const c_char) -> usize;
    pub fn udynlink_dep_resolve_func(
        mgr: *mut UdynlinkDepMgr,
        pool: *mut UdynlinkThunkPool,
        name: *const c_char,
    ) -> usize;
    pub fn udynlink_dep_resolve_data(mgr: *mut UdynlinkDepMgr, name: *const c_char) -> usize;
    pub fn udynlink_dep_register(mgr: *mut UdynlinkDepMgr, p_mod: *mut UdynlinkModule);
    pub fn udynlink_dep_generate_thunks(mgr: *mut UdynlinkDepMgr, p_mod: *mut UdynlinkModule);
    pub fn udynlink_dep_load(
        mgr: *mut UdynlinkDepMgr,
        p_mod: *mut UdynlinkModule,
        base_addr: *const c_void,
        load_addr: *mut c_void,
        load_size: usize,
        load_mode: UdynlinkLoadMode,
        pool: *mut UdynlinkThunkPool,
    ) -> UdynlinkError;
    pub fn udynlink_dep_unload(mgr: *mut UdynlinkDepMgr, p_mod: *mut UdynlinkModule) -> UdynlinkError;
}
```

### 4.6 Externals Callback Infrastructure (`externals.rs`)

The C library has **5 required** `extern` callbacks (with weak defaults for `is_pointer_in_ram`, `vprintf`, and `resolve_symbol`), plus **2 optional** callbacks gated on features. The Rust crate must provide `#[no_mangle] extern "C"` shims that dispatch through a static vtable.

**Full list of C callbacks:**

| Callback | Source header | Required? | Has weak default? |
|----------|--------------|-----------|-------------------|
| `udynlink_external_is_pointer_in_ram` | `udynlink_externals.h` | Yes | Yes (returns 0) |
| `udynlink_external_malloc` | `udynlink_externals.h` | Yes | No |
| `udynlink_external_free` | `udynlink_externals.h` | Yes | No |
| `udynlink_external_vprintf` | `udynlink_externals.h` | Yes | Yes (no-op) |
| `udynlink_external_resolve_symbol` | `udynlink_externals.h` | Yes | Yes (returns 0) |
| `udynlink_external_dep_load` | `udynlink_deps.h` | No (deps feature) | Yes (returns NULL) |
| `udynlink_external_find_stub` | `udynlink_thunk.h` | No (thunk feature) | Yes (linear scan) |

**Key constraint:** On `no_std` bare-metal, we cannot use `dyn Trait` or closures. Use a `static mut` vtable of function pointers.

**Design:** The `Externals` struct contains the 5 required callbacks. Optional callbacks live in separate feature-gated structs and are registered independently. This avoids pulling in deps/thunk types in the core externals.

```rust
use core::ffi::{c_void, c_char, c_int};
use core::sync::atomic::{AtomicBool, Ordering};

/// Function-pointer table for the 5 required host callbacks.
///
/// All pointers must be valid for the entire lifetime of the program
/// (typically `static` functions).
#[repr(C)]
pub struct Externals {
    pub is_pointer_in_ram: unsafe extern "C" fn(p: *const c_void) -> c_int,
    pub malloc: unsafe extern "C" fn(size: usize) -> *mut c_void,
    pub free: unsafe extern "C" fn(p: *mut c_void),
    pub vprintf: unsafe extern "C" fn(s: *const c_char, va_list_ptr: *mut c_void),
    pub resolve_symbol: unsafe extern "C" fn(p_mod: *const UdynlinkModule, name: *const c_char) -> usize,
}

static mut EXTERNALS: Option<&'static Externals> = None;
static EXTERNALS_SET: AtomicBool = AtomicBool::new(false);

/// Register the host's externals callbacks.
///
/// # Safety
/// Must be called exactly once, before any udynlink operation.
/// The provided `Externals` must live for the entire program duration.
pub unsafe fn set_externals(ext: &'static Externals) {
    EXTERNALS = Some(ext);
    EXTERNALS_SET.store(true, Ordering::SeqCst);
}

// ── C callback shims (required) ──

const UNINITIALIZED_PANIC: &str = "udynlink externals not set";

#[no_mangle]
pub unsafe extern "C" fn udynlink_external_is_pointer_in_ram(p: *const c_void) -> c_int {
    let ext = EXTERNALS.expect(UNINITIALIZED_PANIC);
    (ext.is_pointer_in_ram)(p)
}

#[no_mangle]
pub unsafe extern "C" fn udynlink_external_malloc(size: usize) -> *mut c_void {
    let ext = EXTERNALS.expect(UNINITIALIZED_PANIC);
    (ext.malloc)(size)
}

#[no_mangle]
pub unsafe extern "C" fn udynlink_external_free(p: *mut c_void) {
    let ext = EXTERNALS.expect(UNINITIALIZED_PANIC);
    (ext.free)(p)
}

#[no_mangle]
pub unsafe extern "C" fn udynlink_external_vprintf(s: *const c_char, va_list_ptr: *mut c_void) {
    let ext = EXTERNALS.expect(UNINITIALIZED_PANIC);
    (ext.vprintf)(s, va_list_ptr);
}

#[no_mangle]
pub unsafe extern "C" fn udynlink_external_resolve_symbol(
    p_mod: *const UdynlinkModule,
    name: *const c_char,
) -> usize {
    let ext = EXTERNALS.expect(UNINITIALIZED_PANIC);
    (ext.resolve_symbol)(p_mod, name)
}
```

#### Optional Callbacks (feature-gated)

The 2 optional callbacks are registered separately to keep the core `Externals` free of deps/thunk types:

```rust
// ── deps feature: udynlink_external_dep_load ──

#[cfg(feature = "deps")]
static mut DEP_LOAD_CALLBACK: Option<
    unsafe extern "C" fn(name: *const c_char) -> *mut UdynlinkModule
> = None;

#[cfg(feature = "deps")]
pub unsafe fn set_dep_load_callback(
    f: unsafe extern "C" fn(name: *const c_char) -> *mut UdynlinkModule,
) {
    DEP_LOAD_CALLBACK = Some(f);
}

#[cfg(feature = "deps")]
#[no_mangle]
pub unsafe extern "C" fn udynlink_external_dep_load(name: *const c_char) -> *mut UdynlinkModule {
    match DEP_LOAD_CALLBACK {
        Some(f) => f(name),
        None => core::ptr::null_mut(), // matches C weak default
    }
}

// ── thunk feature: udynlink_external_find_stub ──

#[cfg(feature = "thunk")]
static mut FIND_STUB_CALLBACK: Option<
    unsafe extern "C" fn(pool: *const UdynlinkThunkPool, func_addr: u32) -> usize
> = None;

#[cfg(feature = "thunk")]
pub unsafe fn set_find_stub_callback(
    f: unsafe extern "C" fn(pool: *const UdynlinkThunkPool, func_addr: u32) -> usize,
) {
    FIND_STUB_CALLBACK = Some(f);
}

#[cfg(feature = "thunk")]
#[no_mangle]
pub unsafe extern "C" fn udynlink_external_find_stub(
    pool: *const UdynlinkThunkPool,
    func_addr: u32,
) -> usize {
    match FIND_STUB_CALLBACK {
        Some(f) => f(pool, func_addr),
        None => 0, // C weak default returns 0 (triggers linear scan fallback)
    }
}
```

**Note on `va_list`:** The C `va_list` type has no stable Rust ABI representation. The vprintf shim receives it as an opaque `*mut c_void` and passes it through to the user's callback, which can forward it to a C `vprintf` implementation. This avoids needing `core::ffi::VaList` (which is unstable).

---

## 5. Phase 2: `udynlink` — Safe Wrapper Crate

### 5.1 Crate Layout

```
crates/udynlink/
├── Cargo.toml
└── src/
    ├── lib.rs           # Re-exports, crate-level docs
    ├── module.rs        # Module (RAII lifecycle)
    ├── func.rs          # Func<Sig> (typed function handle)
    ├── context.rs       # Context (RAII r9 scope)
    ├── symbol.rs        # Symbol (untyped descriptor)
    ├── image.rs         # Image builder
    ├── error.rs         # Error enum + conversions
    ├── load_mode.rs     # LoadMode enum
    ├── externals.rs     # ExternalsBuilder + set_externals()
    ├── arch.rs          # Arch tag constants + helpers
    ├── thunk.rs         # [feature = "thunk"] ThunkPool
    ├── deps.rs          # [feature = "deps"] DepManager
    ├── hash.rs          # [feature = "hash"] HashTable
    ├── host_utils.rs    # [feature = "host-utils"] SymbolCache
    └── alloc_impls.rs   # [feature = "alloc"] String/Vec conveniences
```

### 5.2 `Module` — RAII Lifecycle

Design mirrors the C++ `udynlink::Module` but uses `Result<Module, Error>` instead of two-phase init:

```rust
/// A loaded udynlink module. Unloads on drop.
///
/// Non-copyable, movable.
pub struct Module {
    inner: UdynlinkModule,
    loaded: bool,
}

impl Module {
    /// Load a module from a contiguous memory-mapped image.
    ///
    /// The module handle is zero-initialized internally as required by the C API.
    /// On success, C++ constructors are run automatically (safe no-op for C modules).
    ///
    /// # Safety
    /// - `base_addr` must point to a valid UDLM binary image
    /// - The image must remain valid for the lifetime of the module (for XIP mode)
    /// - Externals must have been registered via `set_externals()` before calling
    pub unsafe fn load(
        base_addr: *const u8,
        load_addr: *mut u8,
        load_size: usize,
        mode: LoadMode,
    ) -> Result<Self, Error> {
        let mut inner = core::mem::zeroed::<UdynlinkModule>();
        let err = udynlink_load_module(
            &mut inner,
            base_addr.cast(),
            load_addr.cast(),
            load_size,
            mode.into_raw(),
        );
        if err == UdynlinkError::Ok {
            udynlink_cpp_init(&mut inner);
            Ok(Self { inner, loaded: true })
        } else {
            Err(Error::from_raw(err))
        }
    }

    /// Convenience: load with auto-allocation and COPY_ALL mode.
    pub unsafe fn load_simple(base_addr: *const u8) -> Result<Self, Error> {
        Self::load(base_addr, core::ptr::null_mut(), 0, LoadMode::CopyAll)
    }

    /// Load from a non-contiguous image descriptor.
    pub unsafe fn load_image(
        image: &Image,
        load_addr: *mut u8,
        load_size: usize,
        mode: LoadMode,
    ) -> Result<Self, Error> { /* ... */ }

    /// Look up a symbol by name.
    ///
    /// Returns `None` if the symbol is not found in the module.
    /// The returned `Symbol` borrows this `Module`.
    pub fn lookup(&self, name: &CStr) -> Option<Symbol<'_>> { /* ... */ }

    /// Resolve a typed function handle by name.
    ///
    /// Looked-up once, callable many times. The `Func` internally
    /// saves/restores r9 on every call.
    pub fn resolve_func<Sig>(&self, name: &CStr) -> Option<Func<Sig>> { /* ... */ }

    /// Module name (from symbol table).
    pub fn name(&self) -> Option<&CStr> { /* ... */ }

    /// RAM size consumed by this loaded module.
    pub fn ram_size(&self) -> usize { /* ... */ }

    /// The module's ram_base value (useful for r9 management).
    pub fn ram_base(&self) -> usize {
        unsafe { self.inner.ram.ram_base }
    }

    /// Whether this module was built with --no-prologue.
    pub fn has_no_prologue(&self) -> bool {
        unsafe { udynlink_module_has_no_prologue(self.inner.p_header) != 0 }
    }

    /// Raw C handle access (for advanced use).
    pub fn as_raw(&self) -> &UdynlinkModule { &self.inner }
    pub fn as_raw_mut(&mut self) -> &mut UdynlinkModule { &mut self.inner }

    // Incremental / deferred linking
    pub fn link_incremental(&mut self) -> Result<(), Error> { /* ... */ }
    pub fn relink_all(&mut self) -> Result<(), Error> { /* ... */ }
    pub fn link_symbol(&mut self, name: &CStr, addr: usize) -> Result<(), Error> { /* ... */ }
    pub fn is_symbol_resolved(&self, name: &CStr) -> bool { /* ... */ }

    /// Create a Context for repeated calls (avoids per-call r9 overhead).
    pub fn context(&self) -> Context<'_> { /* ... */ }

    /// Explicitly unload. Returns `Ok(())` on success.
    /// Safe to call on an already-unloaded module (no-op).
    pub fn unload(&mut self) -> Result<(), Error> {
        if !self.loaded { return Ok(()); }
        let err = unsafe { udynlink_unload_module(&mut self.inner) };
        self.loaded = false;
        self.inner = unsafe { core::mem::zeroed() };
        if err == UdynlinkError::Ok { Ok(()) } else { Err(Error::from_raw(err)) }
    }
}

impl Drop for Module {
    fn drop(&mut self) {
        let _ = self.unload();
    }
}
```

### 5.3 `Func<Sig>` — Typed Function Handle

Mirrors C++ `Func<R(Args...)>` with inline r9 save/restore:

```rust
/// Typed function handle resolved from a module.
///
/// Resolved once via `Module::resolve_func()`, then called many times.
/// Each call automatically saves the current r9, sets r9 to the module's
/// ram_base, invokes the function, and restores the original r9.
///
/// # Safety
/// `Func` holds an unowned reference to the `Module`. If the `Module`
/// is unloaded while a `Func` still references it, calling the `Func`
/// is undefined behavior.
pub struct Func<Sig> {
    module: *const UdynlinkModule,
    addr: usize,
    _marker: PhantomData<Sig>,
}

impl<Sig> Func<Sig> {
    /// Raw resolved address.
    pub fn address(&self) -> usize { self.addr }

    /// Whether the symbol was successfully resolved.
    pub fn is_resolved(&self) -> bool { self.addr != 0 }
}

impl<R, Args...> Func<extern "C" fn(Args...) -> R> {
    /// Call the module function.
    ///
    /// # Safety
    /// The caller must ensure the `Module` this `Func` was resolved from
    /// is still loaded.  Not interrupt-safe if an ISR calls into a
    /// different module (r9 corruption).
    pub unsafe fn call(&self, args: Args...) -> R {
        // Inline asm: save r9, set r9 to module ram_base, call, restore r9
        // Identical to the C++ FuncInvoker pattern
    }
}
```

**Implementation of `call()`:**

The critical asm sequence (matching the C++ `FuncInvoker`):

```rust
#[cfg(target_arch = "arm")]
impl<R, A1> Func<extern "C" fn(A1) -> R> {
    pub unsafe fn call(&self, a1: A1) -> R {
        let ram_base = (*self.module).ram.ram_base;
        let addr = self.addr;
        let mut prev_r9: u32;
        core::arch::asm!(
            "mov {prev}, r9",
            "mov r9, {base}",
            "blx {func}",
            "mov r9, {prev}",
            prev = out(reg) prev_r9,
            base = in(reg) ram_base,
            func = in(reg) addr,
            // clobbers...
        );
        // result is in r0 already
        // ...
    }
}
```

**Alternative approach (arch-portable, less performant):**

If inline asm for every arity is too much boilerplate, we can use the `FuncInvoker` pattern:
a single `unsafe fn call_raw(module, addr, args...)` that uses `asm!` for r9 save/restore
around a variadic-equivalent function pointer call. This requires per-arity impls anyway,
but the asm block is the same for all — only the cast differs.

**Recommended approach:** Generate per-arity impls via a macro, matching the C++ template pattern. Support arities 0..=6 (Cortex-M calling convention: r0-r3 for first 4 args, stack for 5+).

### 5.4 `Context` — RAII r9 Scope

For tight loops calling the same module repeatedly, per-call r9 save/restore is wasteful:

```rust
/// RAII r9 scope. Saves the current r9, writes the module's ram_base,
/// and restores on drop.
///
/// # Safety
/// NOT interrupt-safe. If an ISR calls into a different module while
/// a Context is active, r9 will be corrupted. Use only in non-preemptive
/// code paths or with interrupts disabled.
pub struct Context<'m> {
    module: &'m Module,
    prev_r9: u32,
}

impl<'m> Context<'m> {
    /// Bind r9 to a module.
    pub unsafe fn new(module: &'m Module) -> Self {
        let prev_r9: u32;
        core::arch::asm!("mov {}, r9", out(reg) prev_r9);
        core::arch::asm!("mov r9, {}", in(reg) module.ram_base());
        Self { module, prev_r9 }
    }

    /// Re-bind to a different module without leaving the scope.
    pub unsafe fn rebind(&mut self, module: &'m Module) {
        self.module = module;
        core::arch::asm!("mov r9, {}", in(reg) module.ram_base());
    }
}

impl<'m> Drop for Context<'m> {
    fn drop(&mut self) {
        unsafe {
            core::arch::asm!("mov r9, {}", in(reg) self.prev_r9);
        }
    }
}
```

Within a `Context`, you can call module functions by raw address without r9 management:

```rust
let ctx = unsafe { Context::new(&module) };
let f: extern "C" fn(i32) -> i32 = unsafe { core::mem::transmute(addr) };
let result = f(42);
drop(ctx);
```

### 5.5 `Symbol` — Untyped Symbol Descriptor

```rust
/// A symbol resolved from a module.
pub struct Symbol<'m> {
    module: &'m Module,
    inner: UdynlinkSym,
}

impl<'m> Symbol<'m> {
    pub fn name(&self) -> &CStr { /* ... */ }
    pub fn value(&self) -> usize { self.inner.val }
    pub fn sym_type(&self) -> SymType { SymType::from_raw(self.inner.type_) }
    pub fn location(&self) -> SymLocation { SymLocation::from_raw(self.inner.location) }

    /// Cast to a typed function handle.
    ///
    /// # Safety
    /// `Sig` must match the actual function signature.
    pub unsafe fn as_func<Sig>(&self) -> Func<Sig> { /* ... */ }
}
```

### 5.6 `Image` — Non-Contiguous Image Builder

```rust
/// Builder for non-contiguous module images.
pub struct Image {
    inner: UdynlinkModuleImage,
}

impl Image {
    /// Build from a contiguous memory buffer.
    pub unsafe fn from_memory(base: *const u8) -> Self { /* ... */ }

    /// Build from an already-loaded module.
    pub unsafe fn from_module(module: &Module) -> Self { /* ... */ }

    // Direct field setters for truly non-contiguous images
    pub unsafe fn set_code(&mut self, ptr: *const u8) { /* ... */ }
    pub unsafe fn set_data(&mut self, ptr: *const u8) { /* ... */ }
}
```

### 5.7 Error Type

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    InvalidSign,
    RamLenLow,
    OutOfMemory,
    XipUnsupported,
    InvalidMode,
    BadRelocationTable,
    UnknownSymbol,
    DuplicateName,
    VersionMismatch,
    ArchMismatch,
    IoError,
    InvalidModule,
    HookAborted,
}

impl Error {
    fn from_raw(e: UdynlinkError) -> Self { /* match on all variants */ }
    pub fn msg(&self) -> &CStr { /* wrapper around udynlink_error_msg */ }
}

impl core::fmt::Display for Error { /* ... */ }
```

### 5.8 `Externals` — Host Callback Registration

Rather than a trait (which requires `dyn` or monomorphization), we provide a builder pattern:

```rust
/// Builder for registering host callbacks.
pub struct ExternalsBuilder {
    inner: udynlink_sys::Externals,
}

impl ExternalsBuilder {
    pub const fn new() -> Self { /* ... */ }

    pub const fn is_pointer_in_ram(mut self, f: unsafe extern "C" fn(*const c_void) -> c_int) -> Self { /* ... */ }
    pub const fn malloc(mut self, f: unsafe extern "C" fn(usize) -> *mut c_void) -> Self { /* ... */ }
    pub const fn free(mut self, f: unsafe extern "C" fn(*mut c_void)) -> Self { /* ... */ }
    pub const fn vprintf(mut self, f: unsafe extern "C" fn(*const c_char, *mut c_void)) -> Self { /* ... */ }
    pub const fn resolve_symbol(mut self, f: unsafe extern "C" fn(*const UdynlinkModule, *const c_char) -> usize) -> Self { /* ... */ }

    /// Build the externals struct (does not install it).
    pub const fn build(self) -> udynlink_sys::Externals { self.inner }

    /// Install all callbacks. Must be called before any udynlink operation.
    ///
    /// # Safety
    /// All registered functions must be valid for the lifetime of the program.
    #[cfg(feature = "alloc")]
    pub unsafe fn install(self) {
        let leaked = Box::leak(Box::new(self.inner));
        udynlink_sys::set_externals(leaked);
    }
}
```

For `no_std` without `alloc`, provide `install_static`:

```rust
impl ExternalsBuilder {
    pub unsafe fn install_static(&'static self) {
        udynlink_sys::set_externals(&self.inner);
    }
}
```

**Optional callback registration** (feature-gated, separate from the builder):

```rust
#[cfg(feature = "deps")]
/// Register the dependency auto-load callback.
///
/// # Safety
/// Must be called before loading any module with `DepManager` if
/// automatic dependency resolution is desired.
pub unsafe fn set_dep_load_callback(
    f: unsafe extern "C" fn(name: *const c_char) -> *mut UdynlinkModule,
) {
    udynlink_sys::set_dep_load_callback(f);
}

#[cfg(feature = "thunk")]
/// Override the default stub finder with a faster implementation.
///
/// # Safety
/// Must be called before any thunk pool usage if overriding.
pub unsafe fn set_find_stub_callback(
    f: unsafe extern "C" fn(pool: *const UdynlinkThunkPool, func_addr: u32) -> usize,
) {
    udynlink_sys::set_find_stub_callback(f);
}
```

### 5.9 Optional Features

#### `thunk` feature

Binds `udynlink_thunk.h` + compiles `udynlink_thunk.c`. Provides callable function pointers that handle r9 switching automatically — useful for callbacks, ISRs, or any consumer unaware of the r9/LOT convention.

```rust
#[cfg(feature = "thunk")]
pub struct ThunkPool {
    inner: UdynlinkThunkPool,
}

#[cfg(feature = "thunk")]
impl ThunkPool {
    /// Create a thunk pool from a RAM buffer.
    /// The buffer must be in executable RAM (typical for Cortex-M TCM).
    pub fn new(buf: &mut [u8]) -> Self {
        let mut pool = ThunkPool { inner: core::mem::zeroed() };
        unsafe { udynlink_thunk_pool_init(&mut pool.inner, buf.as_mut_ptr(), buf.len()) };
        pool
    }

    /// Create a callable thunk for a module symbol.
    ///
    /// Returns a function pointer that can be called directly without r9
    /// management.  The thunk switches r9 to the module's RAM base, calls
    /// the target, and restores r9.
    ///
    /// # Safety
    /// - The module must remain loaded for as long as the thunk is used.
    /// - The thunk pool buffer must be in executable RAM.
    pub unsafe fn make_call(&mut self, module: &Module, sym_name: &CStr) -> Option<usize> {
        let addr = udynlink_thunk_make_call(
            &mut self.inner,
            module.as_raw(),
            sym_name.as_ptr(),
        );
        if addr != 0 { Some(addr) } else { None }
    }

    /// Raw pool access.
    pub fn as_raw(&self) -> &UdynlinkThunkPool { &self.inner }
    pub fn as_raw_mut(&mut self) -> &mut UdynlinkThunkPool { &mut self.inner }
}
```

**Additional sys-level thunk functions** (already bound in `functions.rs`, feature-gated):
- `udynlink_thunk_pool_init`
- `udynlink_thunk_alloc` — low-level stub allocation
- `udynlink_thunk_find_gateway` — find gateway by ram_base
- `udynlink_thunk_alloc_gateway` — allocate gateway for a module
- `udynlink_thunk_alloc_stub` — allocate stub and link to gateway
- `udynlink_thunk_make_call` — convenience: lookup + allocate gateway + stub

**Optional callback:** `udynlink_external_find_stub` — override stub lookup with a faster method (e.g. hash table). Registered via `set_find_stub_callback()`. If not registered, the C weak default performs a linear scan.

#### `deps` feature (implies `thunk`)

Binds `udynlink_deps.h` + compiles `udynlink_deps.c`. Provides cross-module function calls via runtime-generated RAM thunks, with automatic dependency tracking and circular-dependency detection.

```rust
#[cfg(feature = "deps")]
pub struct DepEntry {
    module: Option<Module>,
    gateway: Option<usize>,
}

#[cfg(feature = "deps")]
pub struct DepManager<'a> {
    inner: UdynlinkDepMgr,
    entries: &'a mut [UdynlinkDepEntry],
    pool: &'a mut ThunkPool,
}

#[cfg(feature = "deps")]
impl<'a> DepManager<'a> {
    /// Create a dependency manager with the given entries buffer.
    pub fn new(entries: &'a mut [UdynlinkDepEntry], pool: &'a mut ThunkPool) -> Self { /* ... */ }

    /// Load a module with automatic dependency detection.
    ///
    /// Pushes onto loading stack (circular detection), calls
    /// udynlink_dep_load (which calls udynlink_load_module),
    /// registers the module, pops the stack.
    pub fn load(
        &mut self,
        base: *const u8,
        load_addr: *mut u8,
        load_size: usize,
        mode: LoadMode,
    ) -> Result<Module, Error> { /* ... */ }

    /// Unload a module and deregister it from the dependency manager.
    pub fn unload(&mut self, module: &mut Module) -> Result<(), Error> { /* ... */ }

    /// Find a loaded module by name.
    pub fn find(&self, name: &CStr) -> Option<&Module> { /* ... */ }

    /// Check if a symbol name is a dependency declaration.
    pub fn is_dependency(name: &CStr) -> bool { /* ... */ }

    /// Extract the module name from a dependency symbol.
    pub fn get_dep_name(name: &CStr) -> Option<&CStr> { /* ... */ }

    /// Resolve a cross-module function via thunk stub.
    pub fn resolve_func(&mut self, name: &CStr) -> Option<usize> { /* ... */ }

    /// Resolve a cross-module data symbol (no thunk needed).
    pub fn resolve_data(&self, name: &CStr) -> Option<usize> { /* ... */ }

    /// Register an already-loaded module (if loaded outside the dep system).
    pub fn register(&mut self, module: &Module) { /* ... */ }
}
```

**Helper module writer API (mirrors C `UDYNLINK_REQUIRES`):**
The `UDYNLINK_REQUIRES(mod_name)` C macro is for **module source code**, not the host. The Rust crate does NOT provide an equivalent — Rust modules are out of scope. Document that C modules using `UDYNLINK_REQUIRES` automatically work with the Rust host's `DepManager`.

**Optional callback:** `udynlink_external_dep_load` — called when a dependency is not yet loaded. Registered via `set_dep_load_callback()`. If not registered, the C weak default returns NULL (dependency loading fails).

#### `hash` feature

The hash table is header-only in C. In Rust we re-implement it as a `const`-friendly struct for zero-cost O(1) host symbol resolution:

```rust
#[cfg(feature = "hash")]
pub struct HashTable {
    inner: UdynlinkHashTable,
}

#[cfg(feature = "hash")]
impl HashTable {
    /// Construct from raw pointers (typically from mkhostsyms output).
    pub const unsafe fn from_raw(inner: UdynlinkHashTable) -> Self {
        Self { inner }
    }

    /// GNU hash function (re-implemented from C header).
    pub const fn gnu_hash(name: &[u8]) -> u32 {
        let mut h: u32 = 5381;
        let mut i = 0;
        while i < name.len() {
            h = h.wrapping_mul(33).wrapping_add(name[i] as u32);
            i += 1;
        }
        h
    }

    /// Resolve a symbol by name. O(1) average via bloom filter + bucket lookup.
    pub fn resolve(&self, name: &CStr) -> Option<usize> { /* mirrors udynlink_resolve_hashed_symbol */ }

    /// Raw table access.
    pub fn as_raw(&self) -> &UdynlinkHashTable { &self.inner }
}
```

**Usage in `resolve_symbol` callback:**

```rust
static HOST_SYMS: HashTable = unsafe { HashTable::from_raw(UdynlinkHashTable { /* ... mkhostsyms output ... */ }) };

unsafe extern "C" fn host_resolve_symbol(
    _p_mod: *const UdynlinkModule,
    name: *const c_char,
) -> usize {
    let name = CStr::from_ptr(name);
    HOST_SYMS.resolve(name).unwrap_or(0)
}
```

#### `host-utils` feature

Binds `udynlink_host_utils.h` (header-only). Provides a tiny LRU symbol cache with hash-based lookup to speed up repeated `resolve_symbol` calls:

```rust
#[cfg(feature = "host-utils")]
pub struct SymbolCache<const N: usize = UDYNLINK_HOST_SYM_CACHE_SIZE> {
    entries: [Option<(/* name ptr */, usize)>; N],
}

#[cfg(feature = "host-utils")]
impl<const N: usize> SymbolCache<N> {
    pub const fn new() -> Self { /* all entries None */ }

    /// Look up a symbol, falling back to `resolver` on cache miss.
    /// Mirrors `udynlink_host_sym_cache_lookup`.
    pub fn lookup(
        &mut self,
        p_mod: *const UdynlinkModule,
        name: &CStr,
        resolver: unsafe extern "C" fn(*const UdynlinkModule, *const c_char) -> usize,
    ) -> usize { /* ... */ }

    /// Invalidate all cache entries.
    pub fn invalidate(&mut self) { /* ... */ }
}
```

**Usage pattern:** The host declares a `static mut SYMBOL_CACHE: SymbolCache<16> = SymbolCache::new();` and calls `cache.lookup()` from their `resolve_symbol` callback.

#### `alloc` feature

Enables `String` and `Vec` return types for convenience:

```rust
#[cfg(feature = "alloc")]
impl Module {
    /// Get module name as an owned String.
    pub fn name_string(&self) -> Option<alloc::string::String> { /* ... */ }
}
```

---

### 5.10 Feature Summary

| Cargo feature | Compiles C code? | Binds headers? | Zero-cost when unused? |
|---------------|------------------|-----------------|------------------------|
| (default) | `udynlink.c` | `udynlink.h`, `udynlink_externals.h`, `udynlink_call.h` | Yes |
| `thunk` | `udynlink_thunk.c` | `udynlink_thunk.h` | Yes |
| `deps` | `udynlink_deps.c` | `udynlink_deps.h`, `udynlink_thunk.h` | Yes |
| `hash` | No (header-only) | `udynlink_hash.h` | Yes |
| `host-utils` | No (header-only) | `udynlink_host_utils.h` | Yes |
| `alloc` | No | — | Yes (only adds method impls) |

---

## 6. Phase 3: Test Firmware

### 6.1 Structure

```
tests/rust_qemu_host/
├── Cargo.toml
├── memory.x
├── build.rs
└── src/
    └── main.rs
```

### 6.2 Test Firmware

```rust
#![no_std]
#![no_main]

use udynlink::{Module, ExternalsBuilder, LoadMode};
use cortex_m_rt::entry;
use cortex_m_semihosting::debug;

static MODULE_BIN: &[u8] = include_bytes!("../test-helloworld/mod_hello.bin");

// Simple bump allocator
static mut HEAP: [u8; 8192] = [0; 8192];
static mut HEAP_OFFSET: usize = 0;

unsafe extern "C" fn host_malloc(size: usize) -> *mut core::ffi::c_void {
    let offset = HEAP_OFFSET;
    if offset + size > HEAP.len() { return core::ptr::null_mut(); }
    HEAP_OFFSET = offset + size;
    HEAP[offset..].as_mut_ptr() as *mut _
}

unsafe extern "C" fn host_free(_p: *mut core::ffi::c_void) { /* bump: no free */ }

unsafe extern "C" fn host_is_pointer_in_ram(p: *const core::ffi::c_void) -> i32 {
    let addr = p as usize;
    if addr >= 0x2000_0000 && addr < 0x2001_0000 { 1 } else { 0 }
}

unsafe extern "C" fn host_vprintf(_s: *const i8, _va: *mut core::ffi::c_void) { /* no-op */ }

unsafe extern "C" fn host_resolve_symbol(
    _p_mod: *const udynlink::UdynlinkModule,
    name: *const i8,
) -> usize {
    // No external symbols needed for simple hello world
    0
}

#[entry]
fn main() -> ! {
    static EXTERNALS: udynlink::Externals = unsafe {
        ExternalsBuilder::new()
            .is_pointer_in_ram(host_is_pointer_in_ram)
            .malloc(host_malloc)
            .free(host_free)
            .vprintf(host_vprintf)
            .resolve_symbol(host_resolve_symbol)
            .build()
    };
    unsafe { udynlink::set_externals(&EXTERNALS); }

    let module = unsafe {
        Module::load_simple(MODULE_BIN.as_ptr())
    }.expect("module load failed");

    let test_fn = module.resolve_func::<extern "C" fn() -> i32>(c"test")
        .expect("symbol 'test' not found");

    let result = unsafe { test_fn.call() };

    debug::exit(if result != 0 { 0 } else { 1 });
}
```

### 6.3 Integration with existing test harness

The Python test driver (`test_driver.py`) runs C test firmware via QEMU. For Rust, we add a new platform entry or a `just test-rust-host` command that:
1. Builds the Rust firmware with `cargo build --target thumbv7em-none-eabihf`
2. Converts the ELF to a format QEMU can load
3. Runs QEMU with the same machine flags as the MPS2-AN386 test

---

## 7. Difficulty & Tradeoff Analysis

### 7.1 FFI Binding — LOW
Straightforward C89 API. Hand-writing is preferred over bindgen (more control, no build dependency).

### 7.2 Callback Architecture — LOW-MEDIUM
Static vtable of `extern "C"` function pointers matches the C design perfectly.
`va_list` is the only tricky bit; we paper over it with `*mut c_void` passthrough.

### 7.3 r9 / LOT Management — MEDIUM
The core safety challenge. The approach mirrors the C++ API exactly:
- `Func::call()` does inline r9 save/restore around each call (safe but overhead per call)
- `Context` RAII scope for tight loops (less safe but efficient)
- NOT interrupt-safe if ISR calls different module — document as constraint

**No `critical-section` dependency by default.** The C library doesn't disable interrupts around r9 writes; the Rust wrapper shouldn't either. If a user needs interrupt safety, they wrap calls themselves. This follows the "unopinionated" design principle.

### 7.4 Per-Arity Func Impl — LOW-MEDIUM
Requires macro-generated impls for arities 0..=6. Boilerplate but straightforward.
The C++ code does the same with template specialization.

### 7.5 Target Compilation — LOW
Explicit `arm-none-eabi-gcc` in `build.rs` avoids `cc` crate auto-detection issues.
Documented in `.cargo/config.toml`.

### 7.6 `alloc` vs `no_alloc` — LOW
Core crate is fully `no_std` / `no_alloc`. `alloc` feature is optional, only for
`String`/`Vec` convenience types. The `ExternalsBuilder::install()` method leaks a
`Box` — for `no_alloc`, use `install_static` with a `&'static Externals`.

---

## 8. Task Breakdown

| # | Phase | Task | Deliverable |
|---|-------|------|-------------|
| 1.1 | FFI | Create `udynlink-sys` crate with `Cargo.toml`, `build.rs` | Compiles `udynlink.c` for thumbv7em |
| 1.2 | FFI | Write `#[repr(C)]` type bindings for ALL headers (core, call, thunk, deps, hash, host-utils) | Complete `types.rs` |
| 1.3 | FFI | Write `extern "C"` function declarations for all public APIs (core + feature-gated) | Complete `functions.rs` |
| 1.4 | FFI | Implement externals callback trampolines (5 required + 2 optional) | Complete `externals.rs` with all shims |
| 2.1 | Safe | Create `udynlink` crate skeleton | `Cargo.toml`, `lib.rs` with re-exports and features |
| 2.2 | Safe | Implement `Module` (load/unload/lookup/resolve_func) | RAII `Module` with `Drop` |
| 2.3 | Safe | Implement `Func<Sig>` with per-arity r9 save/restore | Typed callable handle |
| 2.4 | Safe | Implement `Context` (RAII r9 scope) | `Context::new`, `rebind`, `Drop` |
| 2.5 | Safe | Implement `Error`, `LoadMode`, `Symbol`, `Image`, arch tag helpers | All supporting types |
| 2.6 | Safe | Implement `ExternalsBuilder` + `set_externals()` + optional callback registration | Builder + static registration |
| 2.7 | Safe | Implement feature `thunk`: `ThunkPool` with all methods | Full thunk wrapping |
| 2.8 | Safe | Implement feature `deps`: `DepManager` with `load/unload/find/resolve_func/resolve_data` | Full deps wrapping |
| 2.9 | Safe | Implement feature `hash`: `HashTable` with `resolve` + const `gnu_hash` | Full hash wrapping |
| 2.10 | Safe | Implement feature `host-utils`: `SymbolCache<N>` with `lookup/invalidate` | Full host-utils wrapping |
| 2.11 | Safe | Implement feature `alloc`: `String` returns on `Module::name` | Alloc convenience impls |
| 3.1 | Test | Create `tests/rust_qemu_host/` firmware | Cargo project + memory.x |
| 3.2 | Test | Implement bump allocator + host callbacks + dep_load for cross-module test | Working test main |
| 3.3 | Test | Add `just test-rust-host` to Justfile | QEMU integration |
| 3.4 | Test | Run hello-world test in QEMU (MPS2-AN386) | Test passes |
| 3.5 | Test | Run cross-module deps test in QEMU (deps feature) | Test passes |
| 4.1 | Infra | Workspace `Cargo.toml` at repo root | All crates in workspace |
| 4.2 | Infra | Update `.github/workflows/ci.yml` with Rust toolchain | CI passes |

---

## 9. Risk Register

| Risk | Likelihood | Impact | Strategy |
|------|-----------|--------|----------|
| `cc` invokes wrong compiler for ARM | Low | High | Explicit `.compiler("arm-none-eabi-gcc")` in build.rs |
| `asm!` r9 save/restore incorrect for some optimization levels | Medium | High | Mirror exact C++ asm sequence that's tested; validate with QEMU |
| `va_list` passthrough doesn't work on all targets | Low | Medium | Use opaque `*mut c_void`; vprintf is weak no-op by default |
| Per-arity macro misses edge case (i64 args, etc.) | Low | Medium | Start with extern "C" fn arities 0-4; add more as needed |
| Cargo workspace conflicts with existing CMake build | Low | Low | Separate `crates/` directory; does not touch existing build |

---

## 10. Dependencies

| Crate | Purpose | Required? |
|-------|---------|-----------|
| `cc` (build-dep) | Compile C code in `build.rs` | Yes (udynlink-sys) |
| `cortex-m-rt` | Entry point + linker script for tests | Dev-only |
| `cortex-m-semihosting` | QEMU test exit | Dev-only |
| `critical-section` | Optional interrupt-safe LOT guard | No (not used by default) |
| `ufmt` | Lightweight `no_std` formatting | No (considered, not needed) |

No runtime dependencies for the core crate.

---

## 11. Design Principles Alignment

| Project principle | Rust API consequence |
|-------------------|---------------------|
| **Simplicity** | No proc macros, no codegen, no DSLs. Hand-written FFI. Builder pattern for externals. |
| **Unopinionated** | No hidden thread model, no secret allocator, no required event loop. All callbacks are explicit `extern "C"` fn pointers. |
| **Usage-agnostic** | No privileged use case. RAII `Module` for convenience; raw FFI access for control. |
| **Flexible** | All three load modes, non-contiguous images, incremental linking, deferred symbols — all exposed. |
| **Minimal overhead** | `Module` is ~24 bytes + 1 bool. `Func<Sig>` is 2 usizes + PhantomData. No heap allocation in core path. |
| **Zero-cost optional features** | `thunk`, `deps`, `hash`, `host-utils`, `alloc` are Cargo features that compile to zero when unused. Each maps 1:1 to a C header/compilation unit. |
| **Library, not framework** | Rust host calls udynlink; udynlink never calls back except through the 5 required + 2 optional explicit callbacks. |

---

## 12. Independence From `rust-module` Plan

This workstream is **fully independent**:
- A Rust host can load **existing C modules** immediately
- No changes to module format or build process
- Only depends on the existing stable C API (`udynlink.h`)

**Integration point:** After `rust-module` produces a Rust module binary, the Rust host can load it transparently (same `Module::load_simple()` API).

---

*End of Rust Host plan.*
