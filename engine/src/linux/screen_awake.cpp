// The screen kept on through the portal's Inhibit, asked and let go on a
// thread of its own: the portal may be slow to start, and the GUI thread that
// makes one must not wait for it.

#include "ratcast/engine/screen_awake.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

#include "linux/portal.h"

namespace ratcast {
namespace {

class PortalAwake final : public ScreenAwake {
public:
    explicit PortalAwake(std::string reason)
        : thread_([this, reason = std::move(reason)](std::stop_token stop) {
              Result<std::unique_ptr<portal::Inhibition>> inhibition =
                  portal::inhibit_idle(reason, stop);
              if (!inhibition) return;
              held_ = true;
              // Held until let go; ended on this thread, as it was asked.
              std::mutex mutex;
              std::unique_lock lock(mutex);
              std::condition_variable_any().wait(lock, stop, [] { return false; });
              held_ = false;
          }) {}

    bool held() const noexcept override { return held_; }

private:
    std::atomic<bool> held_{false};
    std::jthread thread_;  ///< last: it uses the rest; stopped and joined first
};

}  // namespace

std::unique_ptr<ScreenAwake> keep_screen_awake(std::string reason) {
    return std::make_unique<PortalAwake>(std::move(reason));
}

}  // namespace ratcast
