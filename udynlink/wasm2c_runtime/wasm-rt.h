/* wasm-rt.h — Minimal bare-metal wasm2c runtime header for udynlink
 *
 * This is a drop-in replacement for the upstream wabt wasm-rt.h.
 * It provides everything wasm2c-generated code needs without pulling
 * in libc (no setjmp, no stdlib, no assert).
 *
 * The companion implementation lives in wasm-rt-udynlink.c.
 */

#ifndef WASM_RT_H_
#define WASM_RT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "udynlink_externals.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Core type aliases (match wasm2c generator)                                */
/* -------------------------------------------------------------------------- */

#ifndef WASM_RT_CORE_TYPES_DEFINED
#define WASM_RT_CORE_TYPES_DEFINED
typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;
typedef float    f32;
typedef double   f64;
#endif

/* -------------------------------------------------------------------------- */
/*  Compiler helpers                                                          */
/* -------------------------------------------------------------------------- */

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_expect)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#else
#define UNLIKELY(x) (x)
#define LIKELY(x)   (x)
#endif

#if __has_builtin(__builtin_memcpy)
#define wasm_rt_memcpy __builtin_memcpy
#else
#define wasm_rt_memcpy memcpy
#endif

#if __has_builtin(__builtin_memset)
#define wasm_rt_memset __builtin_memset
#else
#define wasm_rt_memset memset
#endif

#if __has_builtin(__builtin_unreachable)
#define wasm_rt_unreachable __builtin_unreachable
#else
#define wasm_rt_unreachable() do { while (1); } while (0)
#endif

#define WASM_RT_THREAD_LOCAL

/* -------------------------------------------------------------------------- */
/*  Feature toggles (all disabled for bare-metal MVP)                           */
/* -------------------------------------------------------------------------- */

#define WASM_RT_USE_MMAP                  0
#define WASM_RT_MEMCHECK_GUARD_PAGES      0
#define WASM_RT_MEMCHECK_BOUNDS_CHECK     1
#define WASM_RT_SKIP_SIGNAL_RECOVERY      1
#define WASM_RT_INSTALL_SIGNAL_HANDLER    0
#define WASM_RT_USE_STACK_DEPTH_COUNT     0

#if defined(_MSC_VER)
#define WASM_RT_NO_RETURN __declspec(noreturn)
#else
#define WASM_RT_NO_RETURN __attribute__((noreturn))
#endif

#define WASM_RT_MERGED_OOB_AND_EXHAUSTION_TRAPS 0

/* -------------------------------------------------------------------------- */
/*  Trap codes                                                                */
/* -------------------------------------------------------------------------- */

typedef enum {
  WASM_RT_TRAP_NONE,
  WASM_RT_TRAP_OOB,
  WASM_RT_TRAP_INT_OVERFLOW,
  WASM_RT_TRAP_DIV_BY_ZERO,
  WASM_RT_TRAP_INVALID_CONVERSION,
  WASM_RT_TRAP_UNREACHABLE,
  WASM_RT_TRAP_CALL_INDIRECT,
  WASM_RT_TRAP_UNCAUGHT_EXCEPTION,
  WASM_RT_TRAP_UNALIGNED,
  WASM_RT_TRAP_EXHAUSTION,
} wasm_rt_trap_t;

/* -------------------------------------------------------------------------- */
/*  Value types (used by dynamic call-indirect paths)                         */
/* -------------------------------------------------------------------------- */

typedef enum {
  WASM_RT_I32,
  WASM_RT_I64,
  WASM_RT_F32,
  WASM_RT_F64,
  WASM_RT_V128,
  WASM_RT_FUNCREF,
  WASM_RT_EXTERNREF,
} wasm_rt_type_t;

/* -------------------------------------------------------------------------- */
/*  Function reference / table types                                         */
/* -------------------------------------------------------------------------- */

typedef void (*wasm_rt_function_ptr_t)(void);

typedef struct wasm_rt_tailcallee_t {
  void (*fn)(void** instance_ptr,
             void* tail_call_stack,
             struct wasm_rt_tailcallee_t* next);
} wasm_rt_tailcallee_t;

typedef const char* wasm_rt_func_type_t;

typedef struct {
  wasm_rt_func_type_t func_type;
  wasm_rt_function_ptr_t func;
  wasm_rt_tailcallee_t func_tailcallee;
  void* module_instance;
} wasm_rt_funcref_t;

static const wasm_rt_funcref_t wasm_rt_funcref_null_value;

typedef void* wasm_rt_externref_t;
static const wasm_rt_externref_t wasm_rt_externref_null_value = NULL;

/* -------------------------------------------------------------------------- */
/*  Memory & table descriptors                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
  uint8_t* data;
  uint64_t pages;
  uint64_t max_pages;
  uint64_t size;
  bool is64;
} wasm_rt_memory_t;

typedef struct {
  wasm_rt_funcref_t* data;
  uint32_t max_size;
  uint32_t size;
} wasm_rt_funcref_table_t;

typedef struct {
  wasm_rt_externref_t* data;
  uint32_t max_size;
  uint32_t size;
} wasm_rt_externref_table_t;

/* -------------------------------------------------------------------------- */
/*  Lifecycle functions                                                       */
/* -------------------------------------------------------------------------- */

void wasm_rt_init(void);
bool wasm_rt_is_initialized(void);
void wasm_rt_free(void);

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t);
const char* wasm_rt_strerror(wasm_rt_trap_t trap);

/* -------------------------------------------------------------------------- */
/*  Memory API                                                                */
/* -------------------------------------------------------------------------- */

void wasm_rt_allocate_memory(wasm_rt_memory_t* mem,
                             uint64_t initial_pages,
                             uint64_t max_pages,
                             bool is64);
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* mem, uint64_t pages);
void wasm_rt_free_memory(wasm_rt_memory_t* mem);

/* -------------------------------------------------------------------------- */
/*  Table API                                                                 */
/* -------------------------------------------------------------------------- */

void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* table,
                                    uint32_t elements,
                                    uint32_t max_elements);
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t* table);
void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t* table,
                                       uint32_t elements,
                                       uint32_t max_elements);
void wasm_rt_free_externref_table(wasm_rt_externref_table_t* table);
uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t* table,
                                     uint32_t delta,
                                     wasm_rt_funcref_t init);
uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t* table,
                                     uint32_t delta,
                                     wasm_rt_externref_t init);

/* -------------------------------------------------------------------------- */
/*  Func-type macros (must match wasm2c generator)                              */
/* -------------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#define FUNC_TYPE_DECL_EXTERN_T(x) extern const char* const x
#define FUNC_TYPE_EXTERN_T(x)      const char* const x
#define FUNC_TYPE_T(x)             static const char* const x
#else
#define FUNC_TYPE_DECL_EXTERN_T(x) extern const char x[]
#define FUNC_TYPE_EXTERN_T(x)      const char x[]
#define FUNC_TYPE_T(x)             static const char* const x
#endif

/* -------------------------------------------------------------------------- */
/*  Weak-func declaration macro (matches wasm2c generator)                    */
/* -------------------------------------------------------------------------- */

#if defined(_MSC_VER)
#define WEAK_FUNC_DECL(func, fallback)                             \
  __pragma(comment(linker, "/alternatename:" #func "=" #fallback)) \
      void fallback(void** instance_ptr, void* tail_call_stack,      \
                    wasm_rt_tailcallee_t* next)
#else
#define WEAK_FUNC_DECL(func, fallback)                                        \
  __attribute__((weak)) void func(void** instance_ptr, void* tail_call_stack, \
                                  wasm_rt_tailcallee_t* next)
#endif

/* -------------------------------------------------------------------------- */
/*  static_assert fallback for pre-C11                                          */
/* -------------------------------------------------------------------------- */

#if (__STDC_VERSION__ < 201112L) && !defined(static_assert)
#define static_assert(X) \
  extern int(*assertion(void))[!!sizeof(struct { int x : (X) ? 2 : -1; })];
#endif

/* -------------------------------------------------------------------------- */
/*  Hook-based integration with udynlink host                                 */
/* -------------------------------------------------------------------------- */

/*
 * The following symbols are declared weak so the host firmware can override
 * them without modifying this runtime.  Defaults delegate to the udynlink
 * external callbacks (udynlink_external_malloc, udynlink_external_free, ...).
 */

__attribute__((weak)) void* wasm_rt_malloc(size_t size);
__attribute__((weak)) void  wasm_rt_mem_free(void* p);
__attribute__((weak)) void* wasm_rt_mem_realloc(void* p, size_t size);
__attribute__((weak)) void  wasm_rt_trap_handler(wasm_rt_trap_t code);
__attribute__((weak)) void* wasm_rt_resolve_import(const udynlink_module_t *p_mod,
                                                    const char* module,
                                                    const char* name);

/* Default trap page size (64 KiB).  Override with -DWASM_RT_PAGE_SIZE=... */
#ifndef WASM_RT_PAGE_SIZE
#define WASM_RT_PAGE_SIZE 65536
#endif

#ifdef __cplusplus
}
#endif

#endif /* WASM_RT_H_ */
