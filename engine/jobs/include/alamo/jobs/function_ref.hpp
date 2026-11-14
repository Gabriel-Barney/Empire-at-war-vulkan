// Non-owning, non-allocating reference to a callable (like C++26 std::function_ref).
// The referenced callable must outlive every call through the FunctionRef.
#pragma once
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace alamo::jobs {

template <class Sig>
class FunctionRef;

template <class R, class... Args>
class FunctionRef<R(Args...)> {
public:
    template <class F,
              class = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
                                       std::is_invocable_r_v<R, std::remove_reference_t<F>&, Args...>>>
    FunctionRef(F&& f) noexcept  // NOLINT(google-explicit-constructor): implicit by design
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_(&invoke<std::remove_reference_t<F>>) {}

    R operator()(Args... args) const { return call_(obj_, std::forward<Args>(args)...); }
    // Identifies the referenced callable's type (its call thunk), e.g. as a call-site key; never 0.
    uintptr_t target_id() const noexcept { return reinterpret_cast<uintptr_t>(call_); }

private:
    template <class T>
    static R invoke(void* obj, Args... args) {
        if constexpr (std::is_void_v<R>) {
            (*static_cast<T*>(obj))(std::forward<Args>(args)...);
        } else {
            return (*static_cast<T*>(obj))(std::forward<Args>(args)...);
        }
    }

    void* obj_;
    R (*call_)(void*, Args...);
};

}  // namespace alamo::jobs
