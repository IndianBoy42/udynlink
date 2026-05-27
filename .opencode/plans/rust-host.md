# Rust Host Support for udynlink — Detailed Sub-Plan

**Status:** Draft — Awaiting user review  
**Last updated:** 2026-05-26

---

## 1. Goal

Provide a Rust crate that wraps the C udynlink library, enabling Rust programs to:
- Load udynlink binary modules (whether produced from C/C++ or Rust)
- Look up exported symbols by name
- Call exported functions safely
- Manage module lifetime (load/unload with RAII)

**Scope:** This is a host-side integration only. It does NOT create Rust modules — that is covered in `rust-module.md`.

---

## 2. Architecture: Two Layers

```
┌─────────────────────────────────────────┐
│  udynlink-rs (safe API)                 │
│  - Module (RAII)                        │
│  - Symbol<T> (typed wrapper)            │
│  - UdynlinkExternals trait              │
│  - automatic LOT-base call guard        │
├─────────────────────────────────────────┤
│  udynlink-sys (raw FFI)                 │
│  - extern "C" bindings                  │
│  - link to libudynlink.a                │
│  - bindgen-generated enums/structs      │
├─────────────────────────────────────────┤
│  C library (udynlink.h / udynlink.c)    │
│  - udynlink_load_module()               │
│  - udynlink_lookup_symbol()             │
│  - udynlink_get_symbol_value()          │
└─────────────────────────────────────────┘
```

**Design principle:** The Rust host must implement `udynlink_externals.h` (malloc, free, vprintf, resolve_symbol, is_pointer_in_ram, get_module_handle). The Rust crate provides a trait for this.

---

## 3. Phase 1: `udynlink-sys` FFI Crate

**Goal:** Raw bindings to C udynlink library.

**Agent:** `agent`

### 3.1 Crate Setup

```
crates/udynlink-sys/
├── Cargo.toml
├── build.rs              # Compile udynlink/udynlink.c via cc crate
├── src/
│   └── lib.rs            # Generated or hand-written bindings
```

### 3.2 `build.rs`

```rust
use std::env;

fn main() {
    let target = env::var("TARGET").unwrap();
    // Only build for ARM targets
    if !target.starts_with("thumb") && !target.starts_with("arm") {
        panic!("udynlink-sys only supports ARM Cortex-M targets");
    }
    
    cc::Build::new()
        .file("../../udynlink/udynlink.c")
        .file("../../udynlink/udynlink_hash.c")
        .include("../../udynlink")
        .define("UDYNLINK_LOT_BASE_ADDR", "0x20000000")
        // User can override via env var
        .compile("udynlink");
}
```

**Note:** `UDYNLINK_LOT_BASE_ADDR` must be configurable by the user at build time (via environment variables or Cargo features).

### 3.3 Bindings

Hand-written or `bindgen`-generated `extern "C"` blocks:

```rust
pub mod raw {
    use core::ffi::{c_void, c_char, c_int, VaList};
    
    #[repr(C)]
    pub struct udynlink_module_header_t {
        pub sign: u32,
        pub mod_version: u16,
        pub udynlink_version: u16,
        pub arch_tag: u16,
        pub num_lot: u16,
        pub num_rels: u16,
        pub num_deps: u16,
        pub symt_size: u32,
        pub code_size: u32,
        pub data_size: u32,
        pub bss_size: u32,
        pub deps_strtab_size: u32,
    }
    
    #[repr(C)]
    pub struct udynlink_module_t {
        pub p_header: *const udynlink_module_header_t,
        pub p_ram: *mut c_void,
        pub info: u8,
        pub num_deps: u8,
        pub dep_refcount: u8,
        pub deps: [*const udynlink_module_t; 4], // UDYNLINK_MAX_DEPS
    }
    
    // ... enums, other structs ...
    
    extern "C" {
        pub fn udynlink_load_module(
            p_mod: *mut udynlink_module_t,
            base_addr: *const c_void,
            load_addr: *mut c_void,
            load_size: u32,
            load_mode: udynlink_load_mode_t,
        ) -> udynlink_error_t;
        
        pub fn udynlink_unload_module(p_mod: *mut udynlink_module_t) -> udynlink_error_t;
        pub fn udynlink_lookup_symbol(
            p_mod: *const udynlink_module_t,
            name: *const c_char,
            p_sym: *mut udynlink_sym_t,
        ) -> *mut udynlink_sym_t;
        pub fn udynlink_get_symbol_value(
            p_mod: *const udynlink_module_t,
            name: *const c_char,
        ) -> u32;
        pub fn udynlink_get_ram_size(p_mod: *const udynlink_module_t) -> u32;
        pub fn udynlink_get_module_name(p_mod: *const udynlink_module_t) -> *const c_char;
        pub fn udynlink_set_debug_level(level: udynlink_debug_level_t);
        pub fn udynlink_get_module_size(base_addr: *const c_void) -> u32;
        pub fn udynlink_get_code_pointer(p_mod: *const udynlink_module_t) -> *mut u8;
        pub fn udynlink_error_msg(err: *mut udynlink_error_t) -> *const c_char;
        
        // C++ init
        pub fn udynlink_cpp_init(p_mod: *mut udynlink_module_t);
    }
}
```

### 3.4 Externals Callbacks

The C library requires these callbacks to be defined by the host:
```c
int udynlink_external_is_pointer_in_ram(const void *p);
void *udynlink_external_malloc(size_t size);
void udynlink_external_free(void *p);
void udynlink_external_vprintf(const char *s, va_list va);
uint32_t udynlink_external_resolve_critical_symbol(const char *name);
uint32_t udynlink_external_resolve_symbol(const char *name);
udynlink_module_t *udynlink_external_get_module_handle(const char *name);
```

In Rust, the user implements these by providing a type that implements `UdynlinkExternals`.
`udynlink-sys` declares `extern "C"` function pointers that the user sets:

```rust
// In udynlink-sys/src/externals.rs
#[no_mangle]
pub extern "C" fn udynlink_external_malloc(size: usize) -> *mut c_void {
    EXTERNALS.with(|e| e.borrow().malloc(size))
}

// etc.
```

Or, simpler: expose a `set_externals()` function that registers function pointers.

Actually, `no_std` Rust with FFI callbacks is tricky because we can't easily pass closures. The cleanest approach on bare metal is a static trait object or function pointers:

```rust
pub struct ExternalsVTable {
    pub is_pointer_in_ram: unsafe extern "C" fn(*const c_void) -> c_int,
    pub malloc: unsafe extern "C" fn(usize) -> *mut c_void,
    pub free: unsafe extern "C" fn(*mut c_void),
    pub vprintf: unsafe extern "C" fn(*const c_char, VaList),
    pub resolve_critical_symbol: unsafe extern "C" fn(*const c_char) -> u32,
    pub resolve_symbol: unsafe extern "C" fn(*const c_char) -> u32,
    pub get_module_handle: unsafe extern "C" fn(*const c_char) -> *mut udynlink_module_t,
}

static mut EXTERNALS: Option<&'static ExternalsVTable> = None;

pub unsafe fn set_externals(vtable: &'static ExternalsVTable) {
    EXTERNALS = Some(vtable);
}
```

These are then called from `#[no_mangle] extern "C"` functions that the C library links against.

---

## 4. Phase 2: `udynlink-rs` Safe Wrapper

**Goal:** Idiomatic, safe Rust API on top of `udynlink-sys`.

**Agent:** `agent`

### 4.1 Core Types

```rust
use core::ffi::c_void;
use core::marker::PhantomData;
use udynlink_sys::raw::*;

/// A loaded udynlink module. Unloads automatically on drop.
pub struct Module {
    inner: udynlink_module_t,
}

/// A typed reference to a symbol within a module.
pub struct Symbol<'m, T> {
    module: &'m Module,
    value: u32,
    _marker: PhantomData<T>,
}

/// Error type wrapping udynlink_error_t.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    LoadInvalidSign,
    LoadRamLenLow,
    LoadOutOfMemory,
    LoadUnableToXip,
    LoadNoMoreHandles,
    LoadInvalidMode,
    LoadBadRelocationTable,
    LoadUnknownSymbol,
    LoadDuplicateName,
    LoadVersionMismatch,
    LoadArchMismatch,
    LoadMissingDep,
    LoadIoError,
    ModuleInUse,
    InvalidModule,
}
```

### 4.2 Module Lifecycle

```rust
impl Module {
    /// Load a module from a memory buffer.
    /// Safety: base_addr must point to a valid UDLM binary.
    pub unsafe fn load_from_memory(
        base_addr: *const u8,
        mode: LoadMode,
    ) -> Result<Self, Error> {
        let mut inner = core::mem::MaybeUninit::<udynlink_module_t>::uninit();
        let err = udynlink_load_module(
            inner.as_mut_ptr(),
            base_addr as *const c_void,
            core::ptr::null_mut(), // let udynlink allocate
            0,
            mode.into(),
        );
        if err == udynlink_error_t::UDYNLINK_OK {
            Ok(Self { inner: inner.assume_init() })
        } else {
            Err(err.into())
        }
    }
    
    /// Load into a pre-allocated buffer.
    pub unsafe fn load_into(
        base_addr: *const u8,
        ram: &mut [u8],
        mode: LoadMode,
    ) -> Result<Self, Error> {
        // ... similar ...
    }
    
    /// Look up a symbol by name.
    pub fn lookup(&self, name: &str) -> Option<Symbol<'_, ()>> {
        let name_c = name.as_bytes();
        // ... call udynlink_lookup_symbol ...
    }
    
    /// Get module name.
    pub fn name(&self) -> Option<&str> {
        // ... call udynlink_get_module_name ...
    }
    
    /// Get RAM size required by this module.
    pub fn ram_size(&self) -> usize {
        unsafe { udynlink_get_ram_size(&self.inner) as usize }
    }
}

impl Drop for Module {
    fn drop(&mut self) {
        unsafe {
            udynlink_unload_module(&mut self.inner);
        }
    }
}
```

### 4.3 Symbol Calling

The critical operation is calling a function from a loaded module. The C library gives us a `u32` address. We need to set `r9` (LOT base) before calling.

```rust
impl<'m, T> Symbol<'m, T> {
    /// Cast the symbol to a callable function pointer.
    /// 
    /// # Safety
    /// The symbol must actually be a function with signature `T`.
    /// `T` must be an `extern "C" fn` type.
    pub unsafe fn as_fn(&self) -> T 
    where
        T: Copy, // function pointers are Copy
    {
        // Before calling, write ram_base to UDYNLINK_LOT_BASE_ADDR
        let lot_base = self.module.ram_base();
        // ... write to *(u32*)UDYNLINK_LOT_BASE_ADDR ...
        core::mem::transmute_copy(&self.value)
    }
}
```

**Problem:** Writing to `UDYNLINK_LOT_BASE_ADDR` is unsafe and must happen atomically around the call. On bare metal with interrupts, this is a critical section.

**Solution:** A `LotGuard` that saves the old value, writes the new one, calls the function, and restores:

```rust
pub struct LotGuard {
    old_value: u32,
}

impl LotGuard {
    pub unsafe fn new(ram_base: u32) -> Self {
        let addr = UDYNLINK_LOT_BASE_ADDR as *mut u32;
        let old = core::ptr::read_volatile(addr);
        core::ptr::write_volatile(addr, ram_base);
        Self { old_value: old }
    }
}

impl Drop for LotGuard {
    fn drop(&mut self) {
        unsafe {
            let addr = UDYNLINK_LOT_BASE_ADDR as *mut u32;
            core::ptr::write_volatile(addr, self.old_value);
        }
    }
}
```

**But wait:** The C library's `udynlink_cpp_init()` already writes to `UDYNLINK_LOT_BASE_ADDR`. If we have multiple modules loaded, they might overwrite each other's LOT base!

This is an existing design limitation of udynlink: the LOT base is a **global singleton**. Only one module can be active at a time per core. On single-core Cortex-M, this is fine as long as calls don't nest across modules.

For now, document this:
- `LotGuard` is needed around every module call
- Nested calls to different modules are not supported (architecture limitation)
- The guard should be non-reentrant (panic if nested)

### 4.4 `UdynlinkExternals` Trait

```rust
/// Trait for providing the host-side callbacks that udynlink requires.
///
/// # Safety
/// Implementations must be thread-safe (or the host must ensure no concurrent
/// load/unload operations). On Cortex-M, this typically means either:
/// - Single-threaded usage, OR
/// - Critical sections around all udynlink operations.
pub unsafe trait UdynlinkExternals {
    fn is_pointer_in_ram(&self, p: *const c_void) -> bool;
    fn malloc(&self, size: usize) -> *mut c_void;
    fn free(&self, p: *mut c_void);
    fn vprintf(&self, fmt: *const c_char, args: VaList);
    fn resolve_critical_symbol(&self, name: &str) -> Option<u32>;
    fn resolve_symbol(&self, name: &str) -> Option<u32>;
    fn get_module_handle(&self, name: &str) -> Option<&Module>;
}
```

### 4.5 Build-Time Configuration

Users configure via Cargo features or env vars:

```toml
[features]
default = []
# Enable alloc support for returning owned Strings
alloc = []

[package.metadata.udynlink]
max-handles = 8
lot-base = "0x20000000"
```

Or environment variables:
```bash
UDYNLINK_LOT_BASE_ADDR=0x20010000 cargo build
```

The `build.rs` in `udynlink-sys` reads these and passes `-D` flags to the C compiler.

---

## 5. Phase 3: Host Test Firmware

**Goal:** A Rust-based test firmware that loads a C module and calls it, running in QEMU.

**Agent:** `expert`

### 5.1 Test Firmware Structure

```
tests/rust_qemu_host/
├── Cargo.toml              # thumbv7em-none-eabihf target
├── memory.x                # Linker script (same as tests/platforms/)
├── build.rs                # Link with memory.x
└── src/
    └── main.rs
```

### 5.2 Example Test Firmware

```rust
#![no_std]
#![no_main]

use core::panic::PanicInfo;
use udynlink_rs::{Module, LoadMode, UdynlinkExternals};

// Include the test module binary as a byte array
static MODULE_DATA: &[u8] = include_bytes!("../../test-helloworld/mod_hello.bin");

struct HostExternals;

unsafe impl UdynlinkExternals for HostExternals {
    fn malloc(&self, size: usize) -> *mut core::ffi::c_void {
        // Simple bump allocator
        todo!()
    }
    fn free(&self, _p: *mut core::ffi::c_void) {
        // No-op for simple test
    }
    fn is_pointer_in_ram(&self, p: *const core::ffi::c_void) -> bool {
        (p as usize) >= 0x2000_0000 && (p as usize) < 0x2001_0000
    }
    fn resolve_symbol(&self, name: &str) -> Option<u32> {
        match name {
            "printf" => Some(printf as u32),
            "_write" => Some(_write as u32),
            _ => None,
        }
    }
    // ... other callbacks ...
}

#[no_mangle]
pub extern "C" fn main() -> ! {
    let host = HostExternals;
    udynlink_rs::set_externals(&host);
    
    let module = unsafe {
        Module::load_from_memory(MODULE_DATA.as_ptr(), LoadMode::CopyAll)
    }.expect("load failed");
    
    let test_fn = module.lookup("test")
        .expect("symbol not found")
        .as_fn::<extern "C" fn() -> i32>();
    
    let result = unsafe {
        let _guard = udynlink_rs::LotGuard::new(module.ram_base());
        test_fn()
    };
    
    if result != 0 {
        semihosting::exit(0); // success
    } else {
        semihosting::exit(1); // failure
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    semihosting::exit(1);
}
```

### 5.3 Semihosting for QEMU

Use `cortex-m-semihosting` or `cortex-m-rt` for semihosting exit on QEMU.

---

## 6. Difficulty & Tradeoff Analysis

### 6.1 FFI Binding Completeness

**Difficulty: LOW**

`udynlink.h` is a C89 header with simple structs and enums. Hand-writing bindings is straightforward. `bindgen` is optional overhead.

### 6.2 Callback Architecture in `no_std`

**Difficulty: LOW-MEDIUM**

Passing Rust trait implementations to C callbacks requires a global function pointer table or a static trait object. On bare metal, the simplest approach is a `static` vtable of `extern "C"` function pointers.

Tradeoff: No closures or per-Module context. All callbacks are global. This matches the C design and is acceptable for embedded.

### 6.3 LOT-Base Call Guard

**Difficulty: MEDIUM**

The singleton LOT base is a design constraint from the C library. The Rust wrapper must:
1. Set LOT base before every call
2. Restore it after
3. Prevent nested calls to different modules (or document as unsupported)

On Cortex-M, interrupts could also call module functions. Without disabling interrupts around module calls, the LOT base could be corrupted.

**Mitigation:**
- `LotGuard` uses `critical-section` crate (disabled interrupts on single-core M)
- Document that module calls must be brief to minimize interrupt latency impact
- In v2, consider extending C loader with per-call LOT base parameter (but user said don't modify loader)

### 6.4 Target Support

**Difficulty: LOW**

`udynlink-sys` compiles C code via `cc` crate. The target (`thumbv7em-none-eabihf`, `thumbv6m-none-eabi`, etc.) is determined by Cargo. The C compiler invoked by `cc` must match the target.

**Challenge:** `cc` crate might invoke the host `gcc` instead of `arm-none-eabi-gcc`. Need to ensure `CC` env var or `.cargo/config.toml` sets the cross-compiler.

**Mitigation:** Document that users must set:
```toml
[env]
CC = "arm-none-eabi-gcc"
```

Or provide a build script that detects the target and invokes the right compiler.

### 6.5 `alloc` vs `no_alloc`

**Difficulty: LOW**

`udynlink-sys` is `no_std` / `no_alloc`. `udynlink-rs` should also be `no_alloc` by default, with an optional `alloc` feature for `String` return types.

---

## 7. Task Breakdown

| # | Phase | Task | Agent | Deliverable |
|---|-------|------|-------|-------------|
| 1.1 | FFI | Create `udynlink-sys` crate with `build.rs` | `agent` | Compiles `udynlink.c` for thumbv7em |
| 1.2 | FFI | Write `extern "C"` bindings for all public APIs | `quick` | Complete `src/lib.rs` with structs/enums/funcs |
| 1.3 | FFI | Externals callback infrastructure | `agent` | `set_externals()` + `#[no_mangle]` C callbacks |
| 2.1 | Safe | Create `udynlink-rs` crate skeleton | `quick` | `crates/udynlink-rs/` |
| 2.2 | Safe | Implement `Module` (load/unload/lookup) | `agent` | RAII `Module` with Drop |
| 2.3 | Safe | Implement `Symbol<T>` with `LotGuard` | `agent` | Typed symbol + LOT base management |
| 2.4 | Safe | Error type and conversion | `quick` | `Error` enum from `udynlink_error_t` |
| 2.5 | Safe | `UdynlinkExternals` trait + example impl | `agent` | Trait definition + simple bump-allocator example |
| 3.1 | Test | Create `tests/rust_qemu_host/` firmware | `agent` | Rust test host loading C module |
| 3.2 | Test | Run in QEMU (MPS2-AN386) | `agent` | `just test-rust-host` target |
| 3.3 | Test | Cross-test: Rust host loads Rust module | `expert` | End-to-end after rust-module is done |
| 4.1 | Cargo | Workspace `Cargo.toml` at repo root | `quick` | Top-level workspace including all crates |
| 4.2 | CI | GitHub Actions Rust toolchain step | `quick` | Install rustup target, run Rust tests |

---

## 8. Risk Register

| Risk | Likelihood | Impact | Strategy |
|------|-----------|--------|----------|
| `cc` crate invokes wrong compiler for ARM | Low | High | Explicit `CC` env var; test early |
| LOT-base singleton corrupts on nested calls | Medium | High | `LotGuard` + `critical-section`; document restriction |
| Semihosting exit not working in QEMU | Low | Medium | Use `cortex-m-semihosting` crate; validated on MPS2-AN386 |
| `VaList` not available in stable Rust `core` | Low | High | Use `c_va_list` crate or define manually |

---

## 9. Dependencies

| Crate | Purpose | Feature-gated? |
|-------|---------|---------------|
| `cc` | Compile C code in build.rs | No |
| `cstr_core` | C string handling in no_std | Yes (`alloc`) |
| `critical-section` | Interrupt-safe LOT guard | Yes |
| `cortex-m-semihosting` | QEMU test exit | Dev-only |
| `cortex-m-rt` | Runtime + linker script | Dev-only (tests) |

---

## 10. Independence From `rust-module` Plan

This workstream is **fully independent**:
- A Rust host can load **existing C modules** immediately
- No changes to module format or build process
- Only depends on the existing stable C API (`udynlink.h`)

**Integration point:** After `rust-module` produces a Rust module binary, the Rust host can load it transparently (same `Module::load_from_memory()` API).

---

*End of Rust Host sub-plan.*
