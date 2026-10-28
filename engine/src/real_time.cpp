// Linux keeps to the time a wait asks for, and Windows' real-time waits are on
// Sleeper's high-resolution timer: nothing to change.

#include "ratcast/engine/real_time.h"

namespace ratcast {

void keep_time_on_this_thread() {}

}  // namespace ratcast
