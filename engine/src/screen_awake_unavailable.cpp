#include "ratcast/engine/screen_awake.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

class NotKept final : public ScreenAwake {
public:
    bool held() const noexcept override { return false; }
};

}  // namespace

std::unique_ptr<ScreenAwake> keep_screen_awake(std::string) {
    LOG_WARN("engine", "the screen may blank: this build cannot keep it on");
    return std::make_unique<NotKept>();
}

}  // namespace ratcast
