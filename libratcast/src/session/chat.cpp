#include "ratcast/session/chat.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "ratcast/session/presence.h"
#include "session/plain_text.h"

namespace ratcast {

bool is_valid(const ChatAttachment& attachment) noexcept {
    const std::string& name = attachment.name;
    return !name.empty() && name.size() <= ChatAttachment::kMaxNameSize && name != "." &&
           name != ".." && name.find_first_of("/\\") == std::string::npos && is_plain_text(name) &&
           (attachment.width == 0) == (attachment.height == 0) &&
           attachment.width <= ChatAttachment::kMaxImageSide &&
           attachment.height <= ChatAttachment::kMaxImageSide &&
           attachment.preview.size() <= ChatAttachment::kMaxPreviewSize;
}

bool is_valid(const ChatMessage& message) noexcept {
    const std::vector<ChatAttachment>& files = message.attachments;
    if (files.size() > ChatMessage::kMaxAttachments) return false;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!is_valid(files[i])) return false;
        for (size_t j = 0; j < i; ++j) {
            if (files[j].id == files[i].id) return false;
        }
    }
    return (!message.text.empty() || !files.empty()) &&
           message.text.size() <= ChatMessage::kMaxTextSize &&
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
