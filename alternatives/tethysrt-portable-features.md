# TethysRT — Portable Features for udynlink

Source: https://github.com/HotelSierraWhiskey/TethysRT  
Analyzed: /tmp/TethysRT

## Feature 1: Streaming I/O Interface for Storage-Agnostic Loading

TethysRT abstracts the module image source using an I/O callback struct
(`tethys_io_t` in `tethysrt.h:122`) with `read` and `get_size` callbacks
plus a generic `void *pv_ctx`. It reads the module in chunks into a small
internal buffer (`tethysrt.c:196`, `TETHYS_IO_BUFFER_SIZE`) rather than
requiring the entire file to exist in a contiguous memory block.

**udynlink currently**: `udynlink_load_module()` takes `const void *p_image_data`,
requiring the entire binary to be memory-mapped. Loading from SD card / SPI flash /
network requires pre-copying the entire file into RAM first.

**Integration plan**:
1. Define `udynlink_io_t` in `udynlink.h` with read/get_size callbacks
2. Add `udynlink_load_module_from_stream()` API
3. Internally, use the `read` callback to fetch header + symbol table + sections
4. XIP mode should return error (requires memory-mapped pointer)
5. Requires a small static buffer (64-256 bytes) for chunked reads

**Complexity**: Medium

## Feature 2: Pre-Load RAM Requirement Query

`tethys_get_module_size()` (`tethysrt.c:331`) reads just enough of the header
to compute exact RAM requirements without loading or allocating.

**udynlink currently**: No helper exists. Users must manually cast to
`udynlink_module_header_t` and calculate based on load mode.

**Integration plan**:
1. Add `uint32_t udynlink_get_ram_requirements(const void *p_image_data, udynlink_load_mode_t mode)`
2. If streaming I/O is adopted, add `udynlink_get_ram_requirements_stream()`
3. Implementation: parse header, return byte count for given load mode

**Complexity**: Low

## Feature 3: Ergonomic ABI Symbol Registration Macros

`#define TETHYS_SYMBOL(sym) { .kpc_name = #sym, .addr = (uintptr_t)(sym) }`
(`tethysrt.h:29`) — C99 compound literal that stringifies the function name
and casts the pointer.

**udynlink currently**: Users write tedious manual `{"printf", (void*)printf}` pairs.

**Integration plan**:
Add to `udynlink_externals.h`:
```c
#define UDYNLINK_SYMBOL(sym) { #sym, (void *)(sym) }
```

**Complexity**: Trivial

## Feature 4: 64-bit Clean Pointer Casting for Test Hosts

TethysRT includes GCC/Clang diagnostic suppression macros (`tethysrt.c:70`)
for pointer-to-int casts when compiled on 64-bit test hosts.

**udynlink currently**: Raw `(uint32_t)ptr` arithmetic triggers warnings on 64-bit CI.

**Integration plan**: Adopt `uintptr_t` throughout the codebase, cast to `uint32_t`
only at the exact point of patching ARM instructions. Add diagnostic suppression
macros for remaining warnings.

**Complexity**: Low
