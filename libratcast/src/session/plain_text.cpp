#include "session/plain_text.h"

#include <cstddef>
#include <cstdint>

namespace ratcast {
namespace {

/// What a byte that begins a multi-byte UTF-8 sequence says: the sequence's
/// length, the code point's bits it carries, and the least code point a
/// sequence that long may carry (less is an overlong form). Length 0: the
/// byte begins none.
struct Lead {
    size_t length = 0;
    uint32_t bits = 0;
    uint32_t least = 0;
};

Lead lead_of(uint8_t byte) noexcept {
    if ((byte & 0xE0) == 0xC0) return {2, byte & 0x1Fu, 0x80};
    if ((byte & 0xF0) == 0xE0) return {3, byte & 0x0Fu, 0x800};
    if ((byte & 0xF8) == 0xF0) return {4, byte & 0x07u, 0x10000};
    return {};
}

bool is_control(uint32_t code) noexcept { return code < 0x20 || (code >= 0x7F && code < 0xA0); }

}  // namespace

bool is_plain_text(std::string_view text, bool lines) noexcept {
    for (size_t i = 0; i < text.size();) {
        const auto byte = static_cast<uint8_t>(text[i]);
        if (byte < 0x80) {
            if (is_control(byte) && !(lines && (byte == '\n' || byte == '\t'))) return false;
            ++i;
            continue;
        }
        const Lead lead = lead_of(byte);
        if (lead.length == 0 || text.size() - i < lead.length) return false;
        uint32_t code = lead.bits;
        for (size_t k = 1; k < lead.length; ++k) {
            const auto next = static_cast<uint8_t>(text[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            code = code << 6 | (next & 0x3Fu);
        }
        if (code < lead.least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) ||
            is_control(code)) {
            return false;
        }
        i += lead.length;
    }
    return true;
}

}  // namespace ratcast
