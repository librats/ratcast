#include "ratcast/version.h"

#include "librats/util/version.h"

namespace ratcast {

const char* version() noexcept { return RATCAST_VERSION_STRING; }

const char* librats_version() noexcept { return LIBRATS_VERSION_STRING; }

}  // namespace ratcast
