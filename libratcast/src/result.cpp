#include "ratcast/result.h"

namespace ratcast {

const char* to_string(Errc code) noexcept {
    switch (code) {
        case Errc::Truncated: return "truncated";
        case Errc::Malformed: return "malformed";
        case Errc::UnsupportedVersion: return "unsupported version";
        case Errc::UnknownKind: return "unknown kind";
        case Errc::InvalidArgument: return "invalid argument";
    }
    return "unknown error";
}

}  // namespace ratcast
