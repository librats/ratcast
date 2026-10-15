#pragma once

/**
 * @file function_ref.h
 * @brief A non-owning reference to something callable.
 *
 * For callbacks that are only called during the call they are passed to —
 * "hand each output to this" — where std::function would copy the callable
 * and may allocate for it. The callable must outlive the FunctionRef, which
 * a temporary lambda passed as an argument does. C++26's std::function_ref,
 * reduced to what libratcast needs.
 */

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ratcast {

template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
                 std::is_invocable_r_v<R, F&, Args...>)
    FunctionRef(F&& f) noexcept  // NOLINT(google-explicit-constructor): passed like a lambda
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, Args... args) -> R {
              return std::invoke(*static_cast<std::add_pointer_t<F>>(object),
                                 std::forward<Args>(args)...);
          }) {}

    R operator()(Args... args) const { return call_(object_, std::forward<Args>(args)...); }

private:
    void* object_;
    R (*call_)(void*, Args...);
};

}  // namespace ratcast
