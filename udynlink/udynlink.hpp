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

#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace udynlink {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

template<typename Sig>
class Func;

class Context;
class Module;

// ---------------------------------------------------------------------------
// Internal helper: typed function invocation via C API
// ---------------------------------------------------------------------------

namespace detail {

template<typename R, typename... Args>
struct FuncInvoker {
    static inline R invoke(uintptr_t addr, const udynlink_module_t &mod, Args... args) {
        if (addr == 0) {
            return R();
        }
        typedef R (*Fptr)(Args...);
        Fptr f = reinterpret_cast<Fptr>(addr);
        return ({
            uint32_t _udynlink_prev_r9;
            __asm volatile ("mov %0, r9" : "=r"(_udynlink_prev_r9) : :);
            UDYNLINK_PREPARE_CALL(&mod);
            R _udynlink_result = f(args...);
            __asm volatile ("mov r9, %0" :: "r"(_udynlink_prev_r9) : "r9");
            _udynlink_result;
        });
    }
};

template<typename... Args>
struct FuncInvoker<void, Args...> {
    static inline void invoke(uintptr_t addr, const udynlink_module_t &mod, Args... args) {
        if (addr == 0) {
            return;
        }
        typedef void (*Fptr)(Args...);
        Fptr f = reinterpret_cast<Fptr>(addr);
        ({
            uint32_t _udynlink_prev_r9;
            __asm volatile ("mov %0, r9" : "=r"(_udynlink_prev_r9) : :);
            UDYNLINK_PREPARE_CALL(&mod);
            f(args...);
            __asm volatile ("mov r9, %0" :: "r"(_udynlink_prev_r9) : "r9");
        });
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
 * On invocation, r9 is saved, set to the module's ram_base, and
 * restored after the call returns — matching the UDYNLINK_CALL()
 * contract from the C API.
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
 *   int r = (*add)(2, 3);
 * @endcode
 *
 * @warning Func holds an unowned reference to the udynlink_module_t.
 *          If the Module is unloaded or destroyed while any Func
 *          still references it, invoking that Func causes undefined
 *          behaviour.  The lifetime of Func must not exceed the
 *          lifetime of the Module it was resolved from.
 */
template<typename R, typename... Args>
class Func<R(Args...)> {
    const udynlink_module_t *p_mod_;
    uintptr_t addr_;

public:
    /** @brief Construct a disengaged (null) handle. */
    Func() noexcept : p_mod_(nullptr), addr_(0) {}

    /**
     * @brief Resolve a symbol by name into a typed handle.
     *
     * If the symbol is not found, the handle remains disengaged
     * (operator bool() returns false).
     *
     * @param[in] mod  Reference to the loaded module.
     * @param[in] name Null-terminated symbol name.
     */
    Func(const udynlink_module_t &mod, const char *name) noexcept
        : p_mod_(&mod), addr_(0) {
        if (name != nullptr) {
            udynlink_func_t h;
            if (udynlink_resolve_func(p_mod_, name, &h) == UDYNLINK_OK) {
                addr_ = h.addr;
            }
        }
    }

    /**
     * @brief Construct from a pre-resolved address.
     *
     * @param[in] mod  Reference to the loaded module.
     * @param[in] addr Resolved function address.
     */
    Func(const udynlink_module_t &mod, uintptr_t addr) noexcept
        : p_mod_(&mod), addr_(addr) {}

    /**
     * @brief Invoke the module function.
     *
     * Saves r9, sets r9 to the module's RAM base, calls the function,
     * and restores the original r9 — equivalent to UDYNLINK_CALL().
     *
     * @return The value returned by the module function.  If the
     *         symbol was not resolved (addr == 0), returns R().
     *
     * @warning Not interrupt-safe if the called function itself is not
     *          re-entrant.  The r9 save/restore happens in the caller's
     *          stack frame, so nested module calls from the same
     *          interrupt level are safe, but an ISR calling a different
     *          module during this call would corrupt r9 unless
     *          interrupts are disabled.
     */
    inline R operator()(Args... args) const {
        return detail::FuncInvoker<R, Args...>::invoke(addr_, *p_mod_, args...);
    }

    /** @return true if the symbol was resolved successfully. */
    explicit operator bool() const noexcept {
        return addr_ != 0;
    }

    /** @return Raw resolved address. */
    uintptr_t address() const noexcept {
        return addr_;
    }
};

// ---------------------------------------------------------------------------
// Context — RAII r9 manager for efficient repeated calls
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper that binds r9 to a single module.
 *
 * When calling multiple functions from the same module in a tight loop,
 * per-call r9 writes are redundant.  Context saves the previous r9 on
 * construction, writes the module's ram_base, and restores the original
 * r9 on destruction.
 *
 * @warning NOT interrupt-safe.  If an ISR calls into a different module
 *          while a Context is active, r9 will be wrong.  Use Context
 *          only in non-preemptive code paths, or disable interrupts
 *          around the block.
 */
class Context {
    const udynlink_module_t *p_mod_;
    uint32_t prev_r9_;

public:
    /**
     * @brief Bind to a module: save previous r9, write new ram_base.
     *
     * @param[in] mod Reference to the loaded module.
     */
    explicit Context(const udynlink_module_t &mod) noexcept
        : p_mod_(&mod), prev_r9_(0) {
        __asm volatile ("mov %0, r9" : "=r"(prev_r9_) : :);
        UDYNLINK_PREPARE_CALL(&mod);
    }

    /**
     * @brief Restore the previous r9.
     */
    ~Context() noexcept {
        __asm volatile ("mov r9, %0" :: "r"(prev_r9_) : "r9");
    }

    // Non-copyable, non-movable
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

    /**
     * @brief Re-bind to a different module (mid-loop switch).
     *
     * Writes the new module's RAM base directly to r9.
     * The previous value (saved at construction) is NOT touched;
     * it will be restored when this Context is destroyed.
     *
     * @param[in] mod Reference to the new loaded module.
     */
    void rebind(const udynlink_module_t &mod) noexcept {
        p_mod_ = &mod;
        __asm volatile ("mov r9, %0" :: "r"((uint32_t)p_mod_->ram_base) : "r9");
    }

    /** @return Pointer to the currently bound module. */
    const udynlink_module_t *module() const noexcept {
        return p_mod_;
    }
};

// ---------------------------------------------------------------------------
// Symbol — read-only view of one symbol-table entry
// ---------------------------------------------------------------------------

/**
 * @brief Read-only descriptor for a single module symbol.
 *
 * A thin, copyable wrapper around ::udynlink_sym_t returned by SymbolView.
 * Cheap to copy (one pointer + one uintptr_t + two bytes); hold it by value.
 *
 * For symbols obtained from a loaded module (Module::symbols()) the @ref value
 * is the relocated absolute address for INTERNAL/EXPORTED/WEAK symbols; for
 * symbols obtained from an image before loading (udynlink::symbols(image)) it
 * is the raw section offset — see udynlink_image_get_symbol() vs.
 * udynlink_get_symbol() for the full semantics.
 */
class Symbol {
    udynlink_sym_t s_{};

public:
    Symbol() noexcept = default;
    explicit Symbol(udynlink_sym_t s) noexcept : s_(s) {}

    /** @brief Symbol name, or an empty view if the entry was not populated. */
    std::string_view name() const noexcept {
        return s_.name ? std::string_view{s_.name} : std::string_view{};
    }
    /** @brief Relocated value (loaded module) or raw section offset (image). */
    uintptr_t value() const noexcept { return s_.val; }
    /** @brief Symbol type (::UDYNLINK_SYM_TYPE_EXPORTED, ::UDYNLINK_SYM_TYPE_EXTERN, ...). */
    unsigned type() const noexcept { return s_.type; }
    /** @brief Memory location (::UDYNLINK_SYM_LOCATION_CODE or ::UDYNLINK_SYM_LOCATION_DATA). */
    unsigned location() const noexcept { return s_.location; }

    /** @return true for ::UDYNLINK_SYM_TYPE_EXPORTED. */
    bool is_exported() const noexcept { return s_.type == UDYNLINK_SYM_TYPE_EXPORTED; }
    /** @return true for ::UDYNLINK_SYM_TYPE_EXTERN (unresolved at link time). */
    bool is_extern() const noexcept { return s_.type == UDYNLINK_SYM_TYPE_EXTERN; }
    /** @return true for ::UDYNLINK_SYM_TYPE_WEAK. */
    bool is_weak() const noexcept { return s_.type == UDYNLINK_SYM_TYPE_WEAK; }
    /** @return true for ::UDYNLINK_SYM_TYPE_INTERNAL (module-local, nameless). */
    bool is_internal() const noexcept { return s_.type == UDYNLINK_SYM_TYPE_INTERNAL; }
    /** @return true for ::UDYNLINK_SYM_TYPE_MODULE_NAME (index 0). */
    bool is_module_name() const noexcept { return s_.type == UDYNLINK_SYM_TYPE_MODULE_NAME; }
    /** @return true when the symbol lives in the code (.text) section. */
    bool is_function() const noexcept { return s_.location == UDYNLINK_SYM_LOCATION_CODE; }
    /** @return true when the symbol lives in the data section. */
    bool is_data() const noexcept { return s_.location == UDYNLINK_SYM_LOCATION_DATA; }

    /** @return true if this entry was populated (e.g. from a valid index). */
    explicit operator bool() const noexcept { return s_.name != nullptr; }

    /** @brief Escape hatch: the underlying C descriptor. */
    const udynlink_sym_t &raw() const noexcept { return s_; }
};

// ---------------------------------------------------------------------------
// SymbolView — iterable, sized view over a module's symbol table
// ---------------------------------------------------------------------------

/**
 * @brief A C++23 range over a module's symbol table.
 *
 * Models std::ranges::view (default-constructible, movable, O(1) copy). It
 * does not own the underlying image/module data — the caller must keep the
 * module loaded (for Module::symbols()) or the image bytes alive (for the
 * image overloads) for the lifetime of the view and any iterators derived
 * from it.
 *
 * Two construction modes:
 *  - **relocated** — from a loaded module (Module::symbols()): Symbol::value()
 *    is the absolute address, via udynlink_get_symbol().
 *  - **raw** — from an image (udynlink::symbols(image) / symbols(base_addr)):
 *    Symbol::value() is the unrelocated section offset, via
 *    udynlink_image_get_symbol(). The view holds a copy of the
 *    ::udynlink_module_image_t descriptor (just pointers), so it stays valid
 *    as long as the backing image bytes remain mapped.
 *
 * Index 0 is the module name; the rest follow the on-disk symbol-table order.
 *
 * Example:
 * @code
 *   // Before loading — inspect exports from a flash image:
 *   for (udynlink::Symbol s : udynlink::symbols(flash_base)) {
 *       if (s.is_exported() && s.is_function()) { dispatch(s); }
 *   }
 *
 *   // After loading — enumerate relocated symbols:
 *   for (udynlink::Symbol s : mod.symbols()) {
 *       printf("%s @ %p\n", std::string{s.name()}.c_str(), (void*)s.value());
 *   }
 * @endcode
 */
class SymbolView : public std::ranges::view_interface<SymbolView> {
    udynlink_module_image_t img_{};
    const udynlink_module_t *mod_ = nullptr;
    bool relocated_ = false;

public:
    SymbolView() noexcept = default;

    /** @brief Relocated view over a loaded module. */
    explicit SymbolView(const udynlink_module_t &mod) noexcept
        : mod_(&mod), relocated_(true) {}

    /** @brief Raw (unrelocated) view over an image descriptor. */
    explicit SymbolView(const udynlink_module_image_t &img) noexcept
        : img_(img) {}

    /** @brief Raw view built from a contiguous module image in memory. */
    explicit SymbolView(const void *base_addr) noexcept {
        udynlink_image_from_memory(base_addr, &img_);
    }

    /** @brief Number of symbol entries (including the module-name entry at 0). */
    size_t size() const noexcept {
        return relocated_ ? udynlink_get_symbol_count(mod_)
                           : udynlink_image_get_symbol_count(&img_);
    }

    /**
     * @brief Random access to the symbol at @p index (no bounds check beyond
     *        the loader's own; out-of-range yields a default-constructed Symbol).
     */
    Symbol operator[](size_t index) const noexcept {
        udynlink_sym_t s{};
        if (index < size()) {
            if (relocated_) {
                udynlink_get_symbol(mod_, index, &s);
            } else {
                udynlink_image_get_symbol(&img_, index, &s);
            }
        }
        return Symbol{s};
    }

    /**
     * @brief Input iterator yielding Symbol by value.
     *
     * Dereference returns a value (the symbol table is packed, not an array
     * of structs), so reference-stability across increments is not provided
     * — copy the Symbol out if you need to retain it.
     */
    class iterator {
        const SymbolView *v_ = nullptr;
        size_t i_ = 0;

    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Symbol;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Symbol;

        iterator() noexcept = default;
        iterator(const SymbolView *v, size_t i) noexcept : v_(v), i_(i) {}

        Symbol operator*() const noexcept { return v_->operator[](i_); }
        iterator &operator++() noexcept { ++i_; return *this; }
        iterator operator++(int) noexcept { auto t = *this; ++i_; return t; }

        friend bool operator==(const iterator &a, const iterator &b) noexcept {
            return a.i_ == b.i_ && a.v_ == b.v_;
        }
    };

    /** @brief Iterator at the first symbol (index 0). */
    iterator begin() const noexcept { return {this, 0}; }
    /** @brief Past-the-end iterator. */
    iterator end() const noexcept { return {this, size()}; }
};

/**
 * @brief Build a raw (pre-load) SymbolView over an image descriptor.
 *
 * The caller must keep the image data referenced by @p img alive for the
 * lifetime of the returned view.
 */
inline SymbolView symbols(const udynlink_module_image_t &img) noexcept {
    return SymbolView{img};
}

/**
 * @brief Build a raw (pre-load) SymbolView over a contiguous module image.
 *
 * Convenience overload: builds the image descriptor internally. The caller
 * must keep the image bytes at @p base_addr mapped for the lifetime of the
 * returned view.
 */
inline SymbolView symbols(const void *base_addr) noexcept {
    return SymbolView{base_addr};
}

// ---------------------------------------------------------------------------
// Module — RAII module lifecycle manager
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper around a loaded udynlink module.
 *
 * Handles load, automatic C++ init, and unload in a single class.
 * On successful load(), udynlink_cpp_init() is called unconditionally
 * (safe no-op for C modules, since it only runs __init_array
 * constructors if the module exports that symbol).
 */
class Module {
    udynlink_module_t mod_ = {};
    bool loaded_ = false;

public:
    /** @brief Construct an empty (not loaded) module handle. */
    Module() = default;

    /**
     * @brief Unload the module if it is still loaded.
     */
    ~Module() noexcept {
        if (loaded_) {
            (void)unload();
        }
    }

    // Non-copyable
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    // Movable
    Module(Module &&other) noexcept
        : mod_(other.mod_), loaded_(other.loaded_) {
        other.loaded_ = false;
        other.mod_ = {};
    }

    Module &operator=(Module &&other) noexcept {
        if (this != &other) {
            if (loaded_) {
                (void)unload();
            }
            mod_ = other.mod_;
            loaded_ = other.loaded_;
            other.loaded_ = false;
            other.mod_ = {};
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
     * @param[in] load_addr  RAM address, or nullptr to auto-allocate.
     * @param[in] load_size  Size of @p load_addr region (ignored if nullptr).
     * @param[in] mode       Load mode.
     *
     * @return ::UDYNLINK_OK on success, or an error code on failure.
     */
    [[nodiscard]] udynlink_error_t load(const void *base_addr,
                          void *load_addr,
                          size_t load_size,
                          udynlink_load_mode_t mode) noexcept {
        if (loaded_) {
            udynlink_error_t err = unload();
            if (err != UDYNLINK_OK) {
                return err;
            }
        }
        mod_ = {};
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
    [[nodiscard]] udynlink_error_t load(const void *base_addr) noexcept {
        return load(base_addr, nullptr, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
    }

    /**
     * @brief Unload the module.
     *
     * Double-unload is a no-op (returns UDYNLINK_OK).
     *
     * @return ::UDYNLINK_OK on success, or an error code on failure.
     */
    [[nodiscard]] udynlink_error_t unload() noexcept {
        if (!loaded_) {
            return UDYNLINK_OK;
        }
        udynlink_error_t err = udynlink_unload_module(&mod_);
        loaded_ = false;
        mod_ = {};
        return err;
    }

    /** @return true if the module is currently loaded. */
    bool is_loaded() const noexcept {
        return loaded_;
    }

    /**
     * @brief Resolve a typed function handle by name.
     *
     * @tparam Sig Function signature, e.g. int(int, int).
     * @param[in] name Null-terminated symbol name.
     * @return An optional Func handle.  Check has_value() / value().
     */
    template<typename Sig>
    [[nodiscard]] std::optional<Func<Sig>> resolve(const char *name) const {
        Func<Sig> f(mod_, name);
        if (f) {
            return f;
        }
        return std::nullopt;
    }

    /**
     * @brief Explicitly run C++ global constructors.
     *
     * Automatically called by load(); rarely needed manually.
     */
    void cpp_init() noexcept {
        if (loaded_) {
            udynlink_cpp_init(&mod_);
        }
    }

    /**
     * @brief Iterate the loaded module's symbol table.
     *
     * @return A SymbolView yielding relocated symbols (Symbol::value() is the
     *         absolute address for INTERNAL/EXPORTED/WEAK symbols). The view
     *         is empty when the module is not loaded. The view references
     *         this Module; do not use it after unload().
     */
    SymbolView symbols() const noexcept {
        return SymbolView{mod_};
    }

    /** @return Const pointer to the raw C module handle. */
    const udynlink_module_t *handle() const noexcept {
        return &mod_;
    }

    /** @return Pointer to the raw C module handle. */
    udynlink_module_t *handle() noexcept {
        return &mod_;
    }

    /**
     * @brief Swap contents with another Module.
     */
    void swap(Module &other) noexcept {
        if (this != &other) {
            const udynlink_module_t tmp_mod = mod_;
            const bool tmp_loaded = loaded_;
            mod_ = other.mod_;
            loaded_ = other.loaded_;
            other.mod_ = tmp_mod;
            other.loaded_ = tmp_loaded;
        }
    }
};

/** @brief ADL-findable swap for Module. */
inline void swap(Module &a, Module &b) noexcept {
    a.swap(b);
}

} // namespace udynlink

#endif // __UDYNLINK_HPP__
