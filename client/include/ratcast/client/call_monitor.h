#pragma once

/**
 * @file call_monitor.h
 * @brief What a call does, as it does it: each stream sent and played and
 * each path, a report an interval, with how well each goes.
 *
 * Every count a call keeps grows from its start. A CallSnapshot takes them
 * all at once; compare() turns two snapshots into a CallReport of the
 * interval between them — per second, the frame rates, bitrates, sizes,
 * codecs and times of what is sent and played, the pictures' frame time and
 * its jitter, what was lost, late or made up, each path's round trip and
 * loss. A publisher or player that started over in between reads from zero.
 *
 * Each part of a report has a Quality and the reason for it:
 *   - pictures sent: poor when frames were dropped for a viewer's full queue,
 *     the encoder failed or is slower than the pictures come, or skipped
 *     more than a tenth of them; fair when it skipped any, takes more than
 *     half a picture's time, or rate control cut the bitrate;
 *   - sound sent: poor when blocks were dropped or the encoder failed;
 *   - a feed sent: poor when the session refused frames, fair when bytes
 *     were no packet;
 *   - pictures played: poor when frames were lost, dropped or failed to
 *     decode, or decoding is slower than the frames come; fair when frames
 *     came late or decoding takes more than half a frame's time;
 *   - sound played: poor when more than 2% of it was made up for frames that
 *     never came (or failed to decode), fair when any was;
 *   - a feed played: poor when frames were lost, fair when late;
 *   - sound and pictures: fair when they played more than 100 ms apart;
 *   - a path: poor when live messages expired on it, fair when it lost more
 *     than 2% or a message waits more than 100 ms to go.
 * The call's is the worst of them, with its part named.
 *
 * CallMonitor keeps the last snapshot and reports what changed since;
 * report_lines() writes a report for a console, a line a part.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ratcast/client/call.h"
#include "ratcast/client/playback_meter.h"
#include "ratcast/core/time.h"

#include "librats/core/types.h"

namespace ratcast {

enum class Quality : uint8_t { Good, Fair, Poor };

/// "good", "fair", "poor".
const char* to_string(Quality quality) noexcept;

/// A quality and the reason it is not good: the worst of what was seen.
struct Verdict {
    Quality quality = Quality::Good;
    std::string reason;  ///< empty while good

    /// At least as bad as `quality`, for `reason` if that makes it worse.
    void at_least(Quality quality, std::string_view reason);
    /// At least as bad as `other`, its reason prefixed with `part`.
    void at_least(const Verdict& other, std::string_view part);
};

/// Everything a call counts, at one moment.
struct CallSnapshot {
    Timestamp at{};
    Duration cpu{};  ///< the process's CPU time
    std::vector<SentStreamStats> sent = {};
    std::optional<ReceivedVideoStats> video = {};
    std::optional<ReceivedVideoStats> camera = {};
    std::optional<AudioPlayerStats> sound = {};
    std::optional<AudioPlayerStats> voice = {};
    std::optional<DataPlayerStats> feed = {};
    PlayoutClockStats sync = {};
    std::vector<std::pair<PeerId, librats::PathStats>> paths = {};
};

/// What `call` counts, at `now`.
CallSnapshot snapshot(const Call& call, Timestamp now);

// Rates are per second over the interval; times are means over it.

struct SentStreamReport {
    StreamId stream = 0;
    std::string label;
    MediaKind kind = MediaKind::Video;
    std::string encoder;  ///< pictures and sound
    bool gpu_input = false;
    uint32_t width = 0;  ///< pictures
    uint32_t height = 0;
    double source_fps = 0;  ///< pictures, or blocks of sound, from the source
    double fps = 0;         ///< frames sent
    double bitrate = 0;     ///< of those frames, bits per second
    uint32_t target = 0;    ///< what rate control aims at; 0 without it
    Duration encode_time{};
    /// Pictures or blocks of sound the encoder never took; a feed's bytes
    /// that were no packet.
    uint64_t skipped = 0;
    uint64_t keyframes = 0;
    uint64_t dropped = 0;  ///< frames not sent to a viewer whose queue was full
    size_t viewers = 0;
    Verdict verdict;
};

struct ReceivedVideoReport {
    std::string decoder;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0;      ///< pictures handed on
    double bitrate = 0;  ///< of the stream received
    Duration decode_time{};
    FramePace pace;        ///< frame time and its jitter
    uint64_t lost = 0;     ///< frames that never came
    uint64_t late = 0;     ///< came after their time
    uint64_t dropped = 0;  ///< came and were never shown: undecodable, or dropped to start again
    Duration buffer{};     ///< pictures are held back for, past the fastest frames
    Verdict verdict;
};

struct ReceivedSoundReport {
    std::string decoder;
    double fps = 0;     ///< frames played
    Duration delay{};   ///< the jitter buffer's
    Duration target{};  ///< what it aims at
    uint64_t lost = 0;
    uint64_t late = 0;
    Duration recovered{};  ///< brought back by FEC
    Duration concealed{};  ///< made up
    Verdict verdict;
};

struct ReceivedFeedReport {
    double bitrate = 0;  ///< written out
    uint64_t lost = 0;
    uint64_t late = 0;
    Duration buffer{};  ///< held past the fastest
    Verdict verdict;
};

struct PathReport {
    PeerId peer;
    Duration rtt{};
    uint64_t bandwidth = 0;  ///< bits per second, as librats estimates the path
    /// Estimated with the path kept busy: what it carries, not a floor.
    bool busy = false;
    Duration queue{};      ///< what a message waits behind to go
    double loss = 0;       ///< share of the bytes resolved, 0..1
    uint64_t expired = 0;  ///< live messages whose lifetime ran out
    double repair = 0;     ///< FEC sent, bits per second
    Verdict verdict;
};

struct CallReport {
    Duration interval{};
    double cpu = 0;  ///< the process's, a share of one core in percent
    std::vector<SentStreamReport> sent;
    std::optional<ReceivedVideoReport> video;
    std::optional<ReceivedVideoReport> camera;
    std::optional<ReceivedSoundReport> sound;
    std::optional<ReceivedSoundReport> voice;
    std::optional<ReceivedFeedReport> feed;
    LatencyHistogram apart;  ///< sound and pictures, either way
    std::vector<PathReport> paths;
    Verdict verdict;  ///< the worst of the parts
};

/// What happened between two snapshots of a call, `earlier` taken first.
CallReport compare(const CallSnapshot& earlier, const CallSnapshot& now);

/// A report for a console: a line a part, the call's quality last.
std::vector<std::string> report_lines(const CallReport& report);

/// Reports on a call: each what changed since the one before.
class CallMonitor {
public:
    /// `call` must outlive the monitor; the first report counts from now.
    explicit CallMonitor(const Call& call, Clock& clock = SteadyClock::instance());

    CallReport report();

private:
    const Call& call_;
    Clock& clock_;
    CallSnapshot last_;
};

}  // namespace ratcast
