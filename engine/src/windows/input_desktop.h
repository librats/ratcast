// The calling thread on whichever desktop takes input now — the user's, or
// the secure one of the lock and login screens and UAC — while this process
// may open it (SYSTEM may; a user's process only the user's). A thread that
// owns no window or hook can move between desktops: the capture's and the
// pointer's do.

#pragma once

#include <windows.h>

namespace ratcast::win {

class InputDesktop {
public:
    InputDesktop() : home_(GetThreadDesktop(GetCurrentThreadId())) {}
    ~InputDesktop() {
        if (!current_) return;
        SetThreadDesktop(home_);
        CloseDesktop(current_);
    }
    InputDesktop(const InputDesktop&) = delete;
    InputDesktop& operator=(const InputDesktop&) = delete;

    /// Onto the input desktop; false where this process may not open it, or
    /// this thread may not move.
    bool follow() {
        HDESK desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!desktop) return false;
        if (!SetThreadDesktop(desktop)) {
            CloseDesktop(desktop);
            return false;
        }
        if (current_) CloseDesktop(current_);
        current_ = desktop;
        return true;
    }

private:
    HDESK home_;
    HDESK current_ = nullptr;
};

}  // namespace ratcast::win
