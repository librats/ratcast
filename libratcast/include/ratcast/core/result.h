#pragma once

/**
 * @file result.h
 * @brief A value or the reason there is none: how libratcast reports failure.
 *
 * libratcast throws nothing across its boundary — it sits under a C API and is
 * called from real-time threads — so an operation that can fail returns a
 * Result<T>. The interface is a subset of C++23's std::expected<T, Errc>, so
 * code written against it reads the same and can switch over unchanged once
 * every supported toolchain has <expected>.
 *
 *     Result<EncodedFrame> frame = wire::decode_frame(message);
 *     if (!frame) return fail(frame.error());
 *     use(*frame);
 */

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

#include "ratcast/export.h"

namespace ratcast {

/// Why an operation failed. Codes a caller can act on; the detail a human
/// needs goes to the log at the point of failure, which keeps Result small.
enum class Errc : uint8_t {
    Truncated,           ///< input ended inside the structure it was to hold
    Malformed,           ///< bytes no valid encoder produces: reserved bits, overlong varints, …
    UnsupportedVersion,  ///< a wire version this build does not speak
    UnknownKind,         ///< a message kind this build does not know
    Unsupported,         ///< well-formed, but uses something this build does not implement
    InvalidArgument,     ///< a caller passed a value outside its documented range
    Unavailable,         ///< what is needed is not there: a codec, a device, a driver
    Cancelled,           ///< called off: the user declined, or the caller stopped waiting
    Internal,            ///< an underlying library failed; the log has the details
};

/// A stable, human-readable name for `code`, e.g. "truncated".
RATCAST_EXPORT const char* to_string(Errc code) noexcept;

/// The failure side of a Result, built with fail(). A distinct type, so that
/// constructing a Result from it can never be mistaken for a value.
struct Failure {
    Errc code;
};

constexpr Failure fail(Errc code) noexcept { return Failure{code}; }

template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result holds values");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Failure>, "Result<Failure> is ambiguous");

public:
    // Implicit both ways, as std::expected: `return value;` and `return fail(code);`.
    Result(const T& value) : state_(std::in_place_index<0>, value) {}
    Result(T&& value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(Failure failure) noexcept : state_(std::in_place_index<1>, failure.code) {}

    bool has_value() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & noexcept { return checked(); }
    const T& value() const& noexcept { return checked(); }
    T&& value() && noexcept { return std::move(checked()); }

    T& operator*() & noexcept { return checked(); }
    const T& operator*() const& noexcept { return checked(); }
    T&& operator*() && noexcept { return std::move(checked()); }
    T* operator->() noexcept { return &checked(); }
    const T* operator->() const noexcept { return &checked(); }

    /// The failure. Only meaningful when there is no value.
    Errc error() const noexcept {
        assert(!has_value());
        return *std::get_if<1>(&state_);
    }

private:
    T& checked() noexcept {
        assert(has_value());
        return *std::get_if<0>(&state_);
    }
    const T& checked() const noexcept {
        assert(has_value());
        return *std::get_if<0>(&state_);
    }

    std::variant<T, Errc> state_;
};

/// An operation that produces nothing but can fail.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() noexcept = default;
    Result(Failure failure) noexcept : failed_(true), code_(failure.code) {}

    bool has_value() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return has_value(); }

    Errc error() const noexcept {
        assert(failed_);
        return code_;
    }

private:
    bool failed_ = false;
    Errc code_{};
};

}  // namespace ratcast
