#include "ratcast/session/chat.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "ratcast/session/presence.h"
#include "session/plain_text.h"

namespace ratcast {

bool is_valid(const ChatMessage& message) noexcept {
    return !message.text.empty() && message.text.size() <= ChatMessage::kMaxTextSize &&
           is_plain_text(message.text, /*lines=*/true) &&
           message.name.size() <= Presence::kMaxNameSize && is_plain_text(message.name) &&
           message.sent.time_since_epoch().count() >= 0;
}

ChatLog::ChatLog(size_t kept) : kept_(std::max<size_t>(kept, 1)) {}

bool ChatLog::add(ChatMessage message) {
    const auto key = [](const ChatMessage& m) { return std::tie(m.sent, m.author); };
    const auto at = std::lower_bound(
        messages_.begin(), messages_.end(), message,
        [&](const ChatMessage& a, const ChatMessage& b) { return key(a) < key(b); });
    if (at != messages_.end() && key(*at) == key(message)) return false;
    if (messages_.size() == kept_ && at == messages_.begin()) return false;
    messages_.insert(at, std::move(message));
    if (messages_.size() > kept_) messages_.pop_front();
    return true;
}

}  // namespace ratcast
