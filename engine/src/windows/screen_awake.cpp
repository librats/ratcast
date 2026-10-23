// The screen kept on by a power request: a handle, unlike
// SetThreadExecutionState's state, which belongs to the thread that set it.
// Granted at once, so no thread of its own is needed.

#include "ratcast/engine/screen_awake.h"

#include <windows.h>

#include <string>

#include "librats/util/logger.h"

namespace ratcast {
namespace {

class PowerRequestAwake final : public ScreenAwake {
public:
    explicit PowerRequestAwake(const std::string& reason) {
        std::wstring text(reason.begin(), reason.end());  // the reasons given are ASCII
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = text.data();
        request_ = PowerCreateRequest(&context);
        if (request_ == INVALID_HANDLE_VALUE) {
            LOG_WARN("engine", "the screen may blank: no power request (" << GetLastError() << ")");
            return;
        }
        held_ = PowerSetRequest(request_, PowerRequestDisplayRequired) != 0;
        if (!held_) LOG_WARN("engine", "the screen may blank: refused (" << GetLastError() << ")");
    }
    PowerRequestAwake(const PowerRequestAwake&) = delete;
    PowerRequestAwake& operator=(const PowerRequestAwake&) = delete;
    ~PowerRequestAwake() override {
        if (request_ == INVALID_HANDLE_VALUE) return;
        if (held_) PowerClearRequest(request_, PowerRequestDisplayRequired);
        CloseHandle(request_);
    }

    bool held() const noexcept override { return held_; }

private:
    HANDLE request_ = INVALID_HANDLE_VALUE;
    bool held_ = false;
};

}  // namespace

std::unique_ptr<ScreenAwake> keep_screen_awake(std::string reason) {
    return std::make_unique<PowerRequestAwake>(reason);
}

}  // namespace ratcast
