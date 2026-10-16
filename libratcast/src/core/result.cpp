#include "ratcast/core/result.h"

namespace ratcast {

const char* to_string(Errc code) noexcept {
    switch (code) {
        case Errc::Truncated: return "truncated";
        case Errc::Malformed: return "malformed";
        case Errc::UnsupportedVersion: return "unsupported version";
        case Errc::UnknownKind: return "unknown kind";
        case Errc::Unsupported: return "unsupported";
        case Errc::InvalidArgument: return "invalid argument";
        case Errc::Unavailable: return "unavailable";
        case Errc::Cancelled: return "cancelled";
        case Errc::Internal: return "internal error";
    }
    return "unknown error";
}

}  // namespace ratcast
