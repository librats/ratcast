#pragma once

/**
 * @file com.h
 * @brief What every Windows backend here needs of COM and Win32: an apartment
 * for the thread it runs on, kernel handles closed when they go, and how a
 * failed HRESULT reads as an Errc.
 */

#include <objbase.h>
#include <windows.h>
#include <wrl/client.h>

#include <utility>

#include "ratcast/core/result.h"

namespace ratcast::win {

using Microsoft::WRL::ComPtr;

/// COM on this thread, multithreaded, for as long as this lives. A thread
/// that already has an apartment of another kind keeps it.
class ComApartment {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    /// Whether COM can be used on this thread.
    bool ok() const noexcept { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_;
};

/// COM's multithreaded apartment, kept for the rest of the process: every
/// thread that has not chosen an apartment of its own is in it. For objects
/// that outlive the call that made them — a thread that initialised COM only
/// for the call would let the system unload their code when it uninitialised.
inline void keep_multithreaded_apartment() noexcept {
    static const bool kept = [] {
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        return SUCCEEDED(CoIncrementMTAUsage(&cookie));  // never given back
    }();
    (void)kept;
}

/// A kernel handle, closed when it goes.
class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~Handle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_;
};

/// An event, manual-reset or not, unsignalled.
inline Handle make_event(bool manual_reset = false) {
    return Handle(CreateEventW(nullptr, manual_reset ? TRUE : FALSE, FALSE, nullptr));
}

/// A failed call's outcome: what is asked for is not there or not allowed
/// (a device, a driver, the user's consent) — Unavailable — or something
/// went wrong inside — Internal.
inline Errc errc_of(HRESULT result) noexcept {
    switch (result) {
        case E_OUTOFMEMORY:
        case E_UNEXPECTED:
        case E_POINTER: return Errc::Internal;
        case E_INVALIDARG: return Errc::InvalidArgument;
        default: return Errc::Unavailable;
    }
}

}  // namespace ratcast::win
