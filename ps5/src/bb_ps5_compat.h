// bbport PS5: included first in every GPU library source (as shim/bb_win_compat.h is on
// Windows): the runtime's console compatibility (src/compat_ps5.h: gettid and the rest).
#pragma once
#ifdef __PROSPERO__
#include "../../src/compat_ps5.h"
#endif

#if defined(__PROSPERO__) && defined(__cplusplus)
// libc++ 18 (the payload SDK's) has no std::atomic_ref: the members the GPU library uses, on the
// compiler's atomic builtins.
#include <atomic>
#include <version>
#if !defined(__cpp_lib_atomic_ref)
namespace std {
template <class T>
struct atomic_ref {
    explicit atomic_ref(T& object) noexcept : p(&object) {}
    T load(memory_order order = memory_order_seq_cst) const noexcept { return __atomic_load_n(p, int(order)); }
    void store(T value, memory_order order = memory_order_seq_cst) const noexcept { __atomic_store_n(p, value, int(order)); }
    T exchange(T value, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_exchange_n(p, value, int(order)); }
    T fetch_add(T value, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(p, value, int(order)); }
    T fetch_sub(T value, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(p, value, int(order)); }
    T fetch_or(T value, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_or(p, value, int(order)); }
    T fetch_and(T value, memory_order order = memory_order_seq_cst) const noexcept { return __atomic_fetch_and(p, value, int(order)); }
    bool compare_exchange_strong(T& expected, T desired, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange_n(p, &expected, desired, false, int(order), int(memory_order_relaxed));
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange_n(p, &expected, desired, true, int(order), int(memory_order_relaxed));
    }
    operator T() const noexcept { return load(); }
    T* p;
};
} // namespace std
#endif
#endif
