/* Minimal wasm-rt.h for bare-metal udynlink (no setjmp, no stdlib) */
#ifndef WASM_RT_H_
#define WASM_RT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_expect)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define UNLIKELY(x) (x)
#define LIKELY(x) (x)
#endif

#if __has_builtin(__builtin_memcpy)
#define wasm_rt_memcpy __builtin_memcpy
#else
/* Fallback: compiler may not have builtin, but arm-none-eabi-gcc does */
#define wasm_rt_memcpy udynlink_memcpy
#endif

#if __has_builtin(__builtin_unreachable)
#define wasm_rt_unreachable __builtin_unreachable
#else
#define wasm_rt_unreachable() do { while (1); } while (0)
#endif

#define WASM_RT_THREAD_LOCAL

#define WASM_RT_USE_MMAP 0
#define WASM_RT_MEMCHECK_GUARD_PAGES 0
#define WASM_RT_MEMCHECK_BOUNDS_CHECK 1
#define WASM_RT_SKIP_SIGNAL_RECOVERY 1
#define WASM_RT_INSTALL_SIGNAL_HANDLER 0
#define WASM_RT_USE_STACK_DEPTH_COUNT 0

#if defined(_MSC_VER)
#define WASM_RT_NO_RETURN __declspec(noreturn)
#else
#define WASM_RT_NO_RETURN __attribute__((noreturn))
#endif

#define WASM_RT_MERGED_OOB_AND_EXHAUSTION_TRAPS 0

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

typedef enum {
  WASM_RT_I32,
  WASM_RT_I64,
  WASM_RT_F32,
  WASM_RT_F64,
  WASM_RT_V128,
  WASM_RT_FUNCREF,
  WASM_RT_EXTERNREF,
} wasm_rt_type_t;

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

typedef struct {
  uint8_t* data;
  uint64_t pages, max_pages;
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

void wasm_rt_init(void);
bool wasm_rt_is_initialized(void);
void wasm_rt_free(void);

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t);
const char* wasm_rt_strerror(wasm_rt_trap_t trap);

void wasm_rt_allocate_memory(wasm_rt_memory_t*,
                             uint64_t initial_pages,
                             uint64_t max_pages,
                             bool is64);
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t*, uint64_t pages);
void wasm_rt_free_memory(wasm_rt_memory_t*);

void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t*,
                                    uint32_t elements,
                                    uint32_t max_elements);
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t*);
void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t*,
                                       uint32_t elements,
                                       uint32_t max_elements);
void wasm_rt_free_externref_table(wasm_rt_externref_table_t*);
uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t*,
                                     uint32_t delta,
                                     wasm_rt_funcref_t init);
uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t*,
                                       uint32_t delta,
                                       wasm_rt_externref_t init);

#ifdef __cplusplus
}
#endif

#endif
