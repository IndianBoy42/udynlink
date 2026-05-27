/* ARM Cortex-M micro dynamic linker (udynlink) C++ convenience layer.
 *
 * Copyright (c) 2026 Bogdan Marinescu
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __UDYNLINK_HPP__
#define __UDYNLINK_HPP__

#include "udynlink.h"
#include "udynlink_call.h"

#include <cstring>

namespace udynlink {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

template<typename Sig>
class Func;

class Context;
class Module;

// ---------------------------------------------------------------------------
// Internal helper: typed function invocation with null-check
// ---------------------------------------------------------------------------

namespace detail {

template<typename R, typename... Args>
struct FuncInvoker {
    static R invoke(uintptr_t addr, const udynlink_module_t *p_mod, Args... args) {
        if (addr == 0) {
            return R();
        }
        UDYNLINK_PREPARE_CALL(p_mod);
        typedef R (*Fptr)(Args...);
        return reinterpret_cast<Fptr>(addr)(args...);
    }
};

template<typename... Args>
struct FuncInvoker<void, Args...> {
    static void invoke(uintptr_t addr, const udynlink_module_t *p_mod, Args... args) {
        if (addr == 0) {
            return;
        }
        UDYNLINK_PREPARE_CALL(p_mod);
        typedef void (*Fptr)(Args...);
        reinterpret_cast<Fptr>(addr)(args...);
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Func<R(Args...)> — typed, resolvable function handle
// ---------------------------------------------------------------------------

/**
 * @brief Typed function handle for calling module functions from C++.
 *
 * Resolve once with Module::resolve() or the Func constructor, then
 * call many times.  This avoids the O(N) string search on every
 * invocation and provides compile-time type safety.
 *
 * @tparam R    Return type.
 * @tparam Args Argument types.
 *
 * Example:
 * @code
 *   auto add = mod.resolve<int(int, int)>("add");
 *   if (!add) { // symbol not found
 *       return;
 *   }
 *   int r = add(2, 3);
 * @endcode
 */
template<typename R, typename... Args>
class Func<R(Args...)> {
    const udynlink_module_t *p_mod_;
    uintptr_t addr_;

public:
    /**
     * @brief Resolve a symbol by name into a typed handle.
     *
     * If the symbol is not found, addr_ remains 0 and operator bool()
     * returns false.
     *
     * @param[in] p_mod Pointer to the loaded module.
     * @param[in] name  Null-terminated symbol name.
     */
    Func(const udynlink_module_t *p_mod, const char *name)
        : p_mod_(p_mod), addr_(0) {
        if (p_mod_ != NULL && name != NULL) {
            udynlink_func_t h;
            if (udynlink_resolve_func(p_mod_, name, &h) == UDYNLINK_OK) {
                addr_ = h.addr;
            }
        }
    }

    /**
     * @brief Construct from a pre-resolved C handle.
     *
     * @param[in] p_mod Pointer to the loaded module.
     * @param[in] addr  Resolved function address.
     */
    Func(const udynlink_module_t *p_mod, uintptr_t addr)
        : p_mod_(p_mod), addr_(addr) {}

    /**
     * @brief Invoke the module function.
     *
     * Automatically sets the LOT base (via UDYNLINK_PREPARE_CALL) and
     * calls the function with the supplied arguments.
     *
     * @return The value returned by the module function, or R() if the
     *         symbol was not resolved.
     *
     * @warning Not interrupt-safe. If an ISR calls into a different module
     *          while this call is active, the LOT base will be wrong.
     */
    R operator()(Args... args) const {
        return detail::FuncInvoker<R, Args...>::invoke(addr_, p_mod_, args...);
    }

    /** @return true if the symbol was resolved successfully. */
    explicit operator bool() const {
        return addr_ != 0;
    }

    /** @return Raw resolved address. */
    uintptr_t address() const {
        return addr_;
    }
};

// ---------------------------------------------------------------------------
// Context — RAII LOT base manager for efficient repeated calls
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper that binds LOT base to a single module.
 *
 * When calling multiple functions from the same module in a tight loop,
 * per-call LOT base writes are redundant.  Context sets the LOT base
 * once on construction, and restores the previous value on destruction.
 *
 * @warning NOT interrupt-safe.  If an ISR calls into a different module
 *          while a Context is active, LOT base will be wrong.  Use
 *          Context only in non-preemptive code paths, or disable
 *          interrupts around the block.
 */
class Context {
    const udynlink_module_t *p_mod_;
    uint32_t prev_lot_base_;

public:
    /**
     * @brief Bind to a module: save previous LOT base, write new one.
     *
     * @param[in] p_mod Pointer to the loaded module.
     */
    explicit Context(const udynlink_module_t *p_mod)
        : p_mod_(p_mod), prev_lot_base_(0) {
        if (p_mod_ != NULL) {
            prev_lot_base_ = *(uint32_t *)UDYNLINK_LOT_BASE_ADDR;
            *(uint32_t *)UDYNLINK_LOT_BASE_ADDR = (uint32_t)p_mod_->ram_base;
        }
    }

    /**
     * @brief Restore the previous LOT base.
     */
    ~Context() {
        if (p_mod_ != NULL) {
            *(uint32_t *)UDYNLINK_LOT_BASE_ADDR = prev_lot_base_;
        }
    }

    // Non-copyable, non-movable
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    /**
     * @brief Re-bind to a different module (mid-loop switch).
     *
     * Writes the new module's RAM base to the LOT base address.
     * The previous value (saved at construction) is NOT touched;
     * it will be restored when this Context is destroyed.
     *
     * @param[in] p_mod Pointer to the new loaded module.
     */
    void rebind(const udynlink_module_t *p_mod) {
        p_mod_ = p_mod;
        if (p_mod_ != NULL) {
            *(uint32_t *)UDYNLINK_LOT_BASE_ADDR = (uint32_t)p_mod_->ram_base;
        }
    }

    /** @return Pointer to the currently bound module. */
    const udynlink_module_t *module() const {
        return p_mod_;
    }
};

// ---------------------------------------------------------------------------
// Module — RAII module lifecycle manager
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper around a loaded udynlink module.
 *
 * Handles load, automatic C++ init, and unload in a single class.
 * On successful load(), udynlink_cpp_init() is called unconditionally
 * (safe no-op for C modules).
 */
class Module {
    udynlink_module_t mod_;
    bool loaded_;

public:
    /** @brief Construct an empty (not loaded) module handle. */
    Module() : loaded_(false) {
        std::memset(&mod_, 0, sizeof(mod_));
    }

    /**
     * @brief Unload the module if it is still loaded.
     */
    ~Module() {
        if (loaded_) {
            unload();
        }
    }

    // Non-copyable
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    // Movable
    Module(Module &&other) noexcept
        : mod_(other.mod_), loaded_(other.loaded_) {
        other.loaded_ = false;
        std::memset(&other.mod_, 0, sizeof(other.mod_));
    }

    Module &operator=(Module &&other) noexcept {
        if (this != &other) {
            if (loaded_) {
                unload();
            }
            mod_ = other.mod_;
            loaded_ = other.loaded_;
            other.loaded_ = false;
            std::memset(&other.mod_, 0, sizeof(other.mod_));
        }
        return *this;
    }

    /**
     * @brief Load a module from a memory-mapped image.
     *
     * On success, unconditionally calls udynlink_cpp_init() (safe no-op
     * for C modules) and marks the module as loaded.
     *
     * @param[in] base_addr  Address of the module image.
     * @param[in] load_addr  RAM address, or NULL to auto-allocate.
     * @param[in] load_size  Size of @p load_addr region (ignored if NULL).
     * @param[in] mode       Load mode.
     *
     * @return ::UDYNLINK_OK on success, or an error code on failure.
     */
    udynlink_error_t load(const void *base_addr,
                          void *load_addr,
                          size_t load_size,
                          udynlink_load_mode_t mode) {
        if (loaded_) {
            udynlink_error_t err = unload();
            if (err != UDYNLINK_OK) {
                return err;
            }
        }
        std::memset(&mod_, 0, sizeof(mod_));
        udynlink_error_t err = udynlink_load_module(&mod_, base_addr, load_addr, load_size, mode);
        if (err == UDYNLINK_OK) {
            loaded_ = true;
            udynlink_cpp_init(&mod_);
        }
        return err;
    }

    /**
     * @brief Convenience overload: auto-allocate, COPY_ALL mode.
     *
     * @param[in] base_addr Address of the module image.
     * @return ::UDYNLINK_OK on success, or an error code on failure.
     */
    udynlink_error_t load(const void *base_addr) {
        return load(base_addr, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
    }

    /**
     * @brief Unload the module.
     *
     * @return ::UDYNLINK_OK on success, or an error code on failure.
     */
    udynlink_error_t unload() {
        if (!loaded_) {
            return UDYNLINK_ERR_INVALID_MODULE;
        }
        udynlink_error_t err = udynlink_unload_module(&mod_);
        loaded_ = false;
        std::memset(&mod_, 0, sizeof(mod_));
        return err;
    }

    /** @return true if the module is currently loaded. */
    bool is_loaded() const {
        return loaded_;
    }

    /**
     * @brief Resolve a typed function handle by name.
     *
     * @tparam Sig Function signature, e.g. int(int, int).
     * @param[in] name Null-terminated symbol name.
     * @return A Func handle.  Check operator bool() for success.
     */
    template<typename Sig>
    Func<Sig> resolve(const char *name) const {
        return Func<Sig>(&mod_, name);
    }

    /**
     * @brief Explicitly run C++ global constructors.
     *
     * Automatically called by load(); rarely needed manually.
     */
    void cpp_init() {
        if (loaded_) {
            udynlink_cpp_init(&mod_);
        }
    }

    /** @return Const pointer to the raw C module handle. */
    const udynlink_module_t *handle() const {
        return &mod_;
    }

    /** @return Pointer to the raw C module handle. */
    udynlink_module_t *handle() {
        return &mod_;
    }
};

} // namespace udynlink

#endif // __UDYNLINK_HPP__
