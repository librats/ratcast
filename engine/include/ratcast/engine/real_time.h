#pragma once

/**
 * @file real_time.h
 * @brief A thread that keeps time.
 *
 * The threads that wait for a moment — a picture due, a block of sound to
 * hand out, a file's next frame — need each wait to end when asked. macOS
 * lets a timer end late by as much as the thread's service class allows: on
 * the threads of a background process (a launchd service, a CI runner) a
 * sleep or a condition variable's wait of 17 ms ends up to 70 ms late, and no
 * QoS a process may ask for itself changes it. A thread of real time
 * (THREAD_TIME_CONSTRAINT_POLICY) ends its waits within a millisecond or two,
 * as Core Audio's own do. Elsewhere nothing needs changing: Linux keeps to
 * the time asked, and Windows' waits go through a high-resolution timer
 * (Sleeper).
 */

namespace ratcast {

/// Make the calling thread one whose waits end on time: one that waits for
/// moments and does short work in between (a picture decoded, not a file
/// read in full). Logs a warning where the system says no.
void keep_time_on_this_thread();

}  // namespace ratcast
