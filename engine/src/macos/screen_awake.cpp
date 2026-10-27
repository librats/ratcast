// The screen kept on by a power assertion that the display must not sleep for
// the user's idleness: granted at once, so no thread of its own is needed.

#include "ratcast/engine/screen_awake.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <string>

#include "librats/util/logger.h"

namespace ratcast {
namespace {

class AssertionAwake final : public ScreenAwake {
public:
    explicit AssertionAwake(const std::string& reason) {
        CFStringRef name =
            CFStringCreateWithCString(kCFAllocatorDefault, reason.c_str(), kCFStringEncodingUTF8);
        const IOReturn result = IOPMAssertionCreateWithName(
            kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
            name ? name : CFSTR("ratcast"), &assertion_);
        if (name) CFRelease(name);
        held_ = result == kIOReturnSuccess;
        if (!held_) LOG_WARN("engine", "the screen may blank: refused (" << result << ")");
    }
    AssertionAwake(const AssertionAwake&) = delete;
    AssertionAwake& operator=(const AssertionAwake&) = delete;
    ~AssertionAwake() override {
        if (held_) IOPMAssertionRelease(assertion_);
    }

    bool held() const noexcept override { return held_; }

private:
    IOPMAssertionID assertion_ = kIOPMNullAssertionID;
    bool held_ = false;
};

}  // namespace

std::unique_ptr<ScreenAwake> keep_screen_awake(std::string reason) {
    return std::make_unique<AssertionAwake>(reason);
}

}  // namespace ratcast
