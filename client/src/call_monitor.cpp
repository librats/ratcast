#include "ratcast/client/call_monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <type_traits>
#include <variant>

#include "ratcast/client/cpu_meter.h"
#include "ratcast/engine/ts_io.h"

namespace ratcast {
namespace {

using namespace std::chrono_literals;

/// Sound made up beyond this share of the time is heard as broken.
constexpr double kConcealedPoor = 0.02;
/// Pictures skipped beyond this share of the source's are a stutter.
constexpr double kSkippedPoor = 0.1;
/// Sound and pictures further apart than this are noticed.
constexpr Duration kApartFair = 100ms;
/// A path losing more than this share of its bytes is repaired at a cost.
constexpr double kLossFair = 0.02;
/// A message waiting longer than this to go is a queue building up.
constexpr Duration kQueueFair = 100ms;

/// The earlier counts to subtract: none if the counts started over since.
template <typename Stats>
Stats baseline(const Stats& earlier, uint64_t earlier_count, uint64_t now_count) {
    return now_count >= earlier_count ? earlier : Stats{};
}

template <typename Stats>
const Stats* optional_ptr(const std::optional<Stats>& stats) {
    return stats ? &*stats : nullptr;
}

template <typename Stats>
const Stats* find(const std::map<PeerId, Stats>& by_peer, const PeerId& peer) {
    const auto it = by_peer.find(peer);
    return it != by_peer.end() ? &it->second : nullptr;
}

/// "voice 1a2b3c4d": a part of one peer's.
std::string of(std::string_view part, const PeerId& peer) {
    return std::string(part) + " " + peer.short_hex();
}

class Interval {
public:
    explicit Interval(Duration length) : seconds_(std::chrono::duration<double>(length).count()) {}

    double per_second(uint64_t count) const { return seconds_ > 0 ? double(count) / seconds_ : 0; }
    double bits_per_second(uint64_t bytes) const { return per_second(bytes) * 8; }
    /// Time per count, on average: zero without any.
    static Duration mean(Duration total, uint64_t count) {
        return count > 0 ? total / static_cast<Duration::rep>(count) : Duration{};
    }
    /// The share of the interval `time` takes.
    double share(Duration time) const {
        return seconds_ > 0 ? std::chrono::duration<double>(time).count() / seconds_ : 0;
    }

private:
    double seconds_;
};

/// Holds the encoder or decoder to the time one of `count` frames has.
void judge_codec_time(Verdict& verdict, Duration mean, uint64_t count, const Interval& interval,
                      std::string_view codec) {
    if (count == 0 || mean == Duration{}) return;
    const double budget = 1 / interval.per_second(count);  // seconds per frame
    const double took = std::chrono::duration<double>(mean).count();
    if (took > budget) {
        verdict.at_least(Quality::Poor, std::string(codec) + " slower than the frames come");
    } else if (took > budget / 2) {
        verdict.at_least(Quality::Fair, std::string(codec) + " near its limit");
    }
}

SentStreamReport sent_video(const VideoPublisherStats& now, const VideoPublisherStats& before,
                            const OutgoingStreamStats& session,
                            const OutgoingStreamStats& session_before, const Interval& interval) {
    SentStreamReport sent;
    sent.kind = MediaKind::Video;
    sent.encoder = now.encoder;
    sent.gpu_input = now.gpu_input;
    sent.width = now.width;
    sent.height = now.height;
    const uint64_t pictures = now.pictures - before.pictures;
    const uint64_t frames = now.frames - before.frames;
    sent.source_fps = interval.per_second(pictures);
    sent.fps = interval.per_second(frames);
    sent.bitrate = interval.bits_per_second(now.bytes - before.bytes);
    sent.target = session.bitrate;
    sent.encode_time = Interval::mean(now.encode_time - before.encode_time, frames);
    sent.skipped = now.skipped - before.skipped;
    sent.keyframes = now.keyframes - before.keyframes;
    sent.dropped = session.backpressure_drops - session_before.backpressure_drops;

    Verdict& verdict = sent.verdict;
    if (sent.dropped > 0) verdict.at_least(Quality::Poor, "frames dropped: the path is full");
    if (now.errors > before.errors) verdict.at_least(Quality::Poor, "the encoder failed");
    if (pictures > 0 && double(sent.skipped) > kSkippedPoor * double(pictures)) {
        verdict.at_least(Quality::Poor, "pictures skipped: the encoder falls behind");
    } else if (sent.skipped > 0) {
        verdict.at_least(Quality::Fair, "pictures skipped");
    }
    judge_codec_time(verdict, sent.encode_time, std::max(pictures, frames), interval,
                     "the encoder");
    if (session.bitrate_cuts > session_before.bitrate_cuts) {
        verdict.at_least(Quality::Fair, "bitrate cut to the path");
    }
    return sent;
}

SentStreamReport sent_sound(const AudioPublisherStats& now, const AudioPublisherStats& before,
                            const Interval& interval) {
    SentStreamReport sent;
    sent.kind = MediaKind::Audio;
    sent.encoder = now.encoder;
    const uint64_t frames = now.frames - before.frames;
    sent.source_fps = interval.per_second(now.blocks - before.blocks);
    sent.fps = interval.per_second(frames);
    sent.bitrate = interval.bits_per_second(now.bytes - before.bytes);
    sent.encode_time = Interval::mean(now.encode_time - before.encode_time, frames);
    sent.skipped = now.dropped - before.dropped;
    if (sent.skipped > 0) {
        sent.verdict.at_least(Quality::Poor, "sound dropped: the encoder falls behind");
    }
    if (now.errors > before.errors) sent.verdict.at_least(Quality::Poor, "the encoder failed");
    return sent;
}

SentStreamReport sent_feed(const DataPublisherStats& now, const DataPublisherStats& before,
                           const Interval& interval) {
    SentStreamReport sent;
    sent.kind = MediaKind::Data;
    sent.source_fps = interval.per_second(now.packets - before.packets);
    sent.fps = interval.per_second(now.frames - before.frames);
    sent.bitrate = interval.bits_per_second((now.packets - before.packets) * kTsPacketSize);
    sent.skipped = now.skipped - before.skipped;
    if (now.errors > before.errors) {
        sent.verdict.at_least(Quality::Poor, "frames refused by the session");
    }
    if (sent.skipped > 0) sent.verdict.at_least(Quality::Fair, "bytes that were no packet");
    return sent;
}

/// The frames counted by a publisher of any kind: whether it started over.
uint64_t frames_of(const PublisherStats& stats) {
    return std::visit([](const auto& s) { return s.frames; }, stats);
}

SentStreamReport sent_stream(const SentStreamStats& now, const SentStreamStats* earlier,
                             const Interval& interval) {
    const bool fresh = !earlier || earlier->publisher.index() != now.publisher.index() ||
                       frames_of(now.publisher) < frames_of(earlier->publisher);
    SentStreamReport sent = std::visit(
        [&](const auto& stats) {
            using Stats = std::decay_t<decltype(stats)>;
            const Stats before = fresh ? Stats{} : std::get<Stats>(earlier->publisher);
            if constexpr (std::is_same_v<Stats, VideoPublisherStats>) {
                const OutgoingStreamStats session_before =
                    fresh ? OutgoingStreamStats{} : earlier->session;
                return sent_video(stats, before, now.session, session_before, interval);
            } else if constexpr (std::is_same_v<Stats, AudioPublisherStats>) {
                return sent_sound(stats, before, interval);
            } else {
                return sent_feed(stats, before, interval);
            }
        },
        now.publisher);
    sent.stream = now.stream;
    sent.label = now.label;
    sent.viewers = now.session.subscribers;
    sent.relayed = now.session.relayed;
    return sent;
}

ReceivedVideoReport received_video(const ReceivedVideoStats& now, const ReceivedVideoStats* earlier,
                                   const Interval& interval) {
    // Another peer's pictures since are a player started over.
    const ReceivedVideoStats before =
        earlier && earlier->peer == now.peer
            ? baseline(*earlier, earlier->player.frames, now.player.frames)
            : ReceivedVideoStats{};
    const VideoPlayerStats& player = now.player;
    const FrameTracker::Stats& frames = now.stream.frames;
    const FrameTracker::Stats& frames_before = before.stream.frames;
    ReceivedVideoReport video;
    video.peer = now.peer;
    video.decoder = player.decoder;
    video.width = player.width;
    video.height = player.height;
    const uint64_t decoded = player.frames - before.player.frames;
    video.fps = interval.per_second(player.pictures - before.player.pictures);
    video.bitrate = interval.bits_per_second(now.stream.bytes - before.stream.bytes);
    video.decode_time = Interval::mean(player.decode_time - before.player.decode_time, decoded);
    video.pace = player.pace.since(before.player.pace);
    video.lost = frames.lost - frames_before.lost;
    video.late = frames.late - frames_before.late;
    video.dropped =
        (frames.undecodable - frames_before.undecodable) + (player.dropped - before.player.dropped);
    video.buffer = player.delay;
    video.passed_on_to = now.stream.relaying_to;
    video.passed_on_bitrate =
        interval.bits_per_second(now.stream.relayed_bytes - before.stream.relayed_bytes);

    Verdict& verdict = video.verdict;
    if (video.lost > 0) verdict.at_least(Quality::Poor, "frames lost");
    if (video.dropped > 0) verdict.at_least(Quality::Poor, "frames dropped");
    if (player.decode_errors > before.player.decode_errors) {
        verdict.at_least(Quality::Poor, "frames failed to decode");
    }
    judge_codec_time(verdict, video.decode_time, decoded, interval, "the decoder");
    if (video.late > 0) verdict.at_least(Quality::Fair, "frames came late");
    return video;
}

ReceivedSoundReport received_sound(const AudioPlayerStats& now, const AudioPlayerStats* earlier,
                                   const Interval& interval) {
    const AudioPlayerStats before =
        earlier ? baseline(*earlier, earlier->frames, now.frames) : AudioPlayerStats{};
    const JitterBuffer::Stats& jitter = now.jitter;
    ReceivedSoundReport sound;
    sound.decoder = now.decoder;
    sound.fps = interval.per_second(jitter.played - before.jitter.played);
    sound.delay = now.delay;
    sound.target = now.target;
    sound.lost = jitter.lost - before.jitter.lost;
    sound.late = jitter.late - before.jitter.late;
    sound.recovered = now.decoded.recovered - before.decoded.recovered;
    // The jitter buffer's: the decoder's own counts DTX's silence as made up.
    sound.concealed = jitter.concealed - before.jitter.concealed;

    if (now.decode_errors > before.decode_errors) {
        sound.verdict.at_least(Quality::Poor, "frames failed to decode");
    }
    if (interval.share(sound.concealed) > kConcealedPoor) {
        sound.verdict.at_least(Quality::Poor, "sound made up for frames that never came");
    } else if (sound.concealed > Duration{}) {
        sound.verdict.at_least(Quality::Fair, "some sound made up");
    }
    return sound;
}

ReceivedFeedReport received_feed(const DataPlayerStats& now,
                                 const std::optional<DataPlayerStats>& earlier,
                                 const Interval& interval) {
    const DataPlayerStats before =
        earlier ? baseline(*earlier, earlier->delivery.received, now.delivery.received)
                : DataPlayerStats{};
    ReceivedFeedReport feed;
    feed.bitrate = interval.bits_per_second(now.bytes - before.bytes);
    feed.lost = now.delivery.lost - before.delivery.lost;
    feed.late = now.delivery.late - before.delivery.late;
    feed.buffer = now.latency;
    if (feed.lost > 0) feed.verdict.at_least(Quality::Poor, "frames lost");
    if (now.write_errors > before.write_errors) {
        feed.verdict.at_least(Quality::Poor, "the output failed");
    }
    if (feed.late > 0) feed.verdict.at_least(Quality::Fair, "frames came late");
    return feed;
}

PathReport path_report(const PeerId& peer, const librats::PathStats& now,
                       const librats::PathStats* earlier, const Interval& interval) {
    const librats::PathStats before =
        earlier ? baseline(*earlier, earlier->bytes_delivered, now.bytes_delivered)
                : librats::PathStats{};
    PathReport path;
    path.peer = peer;
    path.rtt = Duration{now.srtt_us};
    path.bandwidth = now.bandwidth_bps;
    path.busy = !now.app_limited;
    if (now.bandwidth_bps > 0) {
        path.queue = Duration{
            static_cast<Duration::rep>(double(now.queued_bytes) * 8e6 / double(now.bandwidth_bps))};
    }
    const uint64_t lost = now.bytes_lost - before.bytes_lost;
    const uint64_t resolved = lost + now.bytes_delivered - before.bytes_delivered;
    path.loss = resolved > 0 ? double(lost) / double(resolved) : 0;
    path.expired = now.live_expired - before.live_expired;
    path.repair = interval.bits_per_second(now.live_repair_bytes - before.live_repair_bytes);

    if (path.expired > 0) {
        path.verdict.at_least(Quality::Poor, "frames expired: the path carries less than is sent");
    }
    if (path.loss > kLossFair) path.verdict.at_least(Quality::Fair, "the path loses packets");
    if (path.queue > kQueueFair) path.verdict.at_least(Quality::Fair, "a queue builds up");
    return path;
}

// ── Text ────────────────────────────────────────────────────────────────────

double ms(Duration d) { return double(d.count()) / 1000; }

/// "4.12 Mbit/s", "24.0 kbit/s".
std::string rate(double bits_per_second) {
    char text[32];
    if (bits_per_second >= 1e6) {
        std::snprintf(text, sizeof text, "%.2f Mbit/s", bits_per_second / 1e6);
    } else {
        std::snprintf(text, sizeof text, "%.1f kbit/s", bits_per_second / 1e3);
    }
    return text;
}

std::string size(uint32_t width, uint32_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

/// "good", or "fair: frames came late".
std::string quality(const Verdict& verdict) {
    std::string text = to_string(verdict.quality);
    if (!verdict.reason.empty()) text += ": " + verdict.reason;
    return text;
}

template <typename... Args>
std::string format(const char* pattern, Args... args) {
    char line[512];
    std::snprintf(line, sizeof line, pattern, args...);
    return line;
}

/// "3", or "3 (2 relayed)".
std::string viewers(const SentStreamReport& s) {
    return s.relayed == 0 ? std::to_string(s.viewers)
                          : format("%zu (%zu relayed)", s.viewers, s.relayed);
}

/// "  passing on to 2 at 4.2 Mbit/s", or nothing.
std::string passing_on(const ReceivedVideoReport& v) {
    if (v.passed_on_to == 0) return {};
    return format("  passing on to %zu at %s", v.passed_on_to, rate(v.passed_on_bitrate).c_str());
}

std::string sent_line(const SentStreamReport& s) {
    const auto count = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    switch (s.kind) {
        case MediaKind::Video:
            return format(
                "sent %-10s %s %s%s  %3.0f fps (source %3.0f)  %s (aim %s)  encode %5.2f ms"
                "  viewers %s  skipped %llu  keyframes %llu  dropped %llu  %s",
                s.label.c_str(), size(s.width, s.height).c_str(), s.encoder.c_str(),
                s.gpu_input ? " (GPU)" : "", s.fps, s.source_fps, rate(s.bitrate).c_str(),
                rate(s.target).c_str(), ms(s.encode_time), viewers(s).c_str(), count(s.skipped),
                count(s.keyframes), count(s.dropped), quality(s.verdict).c_str());
        case MediaKind::Audio:
            return format(
                "sent %-10s %s  %3.0f fps  %s  encode %5.2f ms  viewers %zu  dropped %llu"
                "  %s",
                s.label.c_str(), s.encoder.c_str(), s.fps, rate(s.bitrate).c_str(),
                ms(s.encode_time), s.viewers, count(s.skipped), quality(s.verdict).c_str());
        case MediaKind::Data:
            return format(
                "sent %-10s feed  %4.0f frames/s  %s  viewers %zu  skipped %llu bytes  %s",
                s.label.c_str(), s.fps, rate(s.bitrate).c_str(), s.viewers, count(s.skipped),
                quality(s.verdict).c_str());
    }
    return {};
}

std::string video_line(const char* name, const ReceivedVideoReport& v) {
    const LatencyHistogram& frame = v.pace.intervals();
    return format(
        "%-15s %s %s  %3.0f fps  %s  decode %5.2f ms  frame %5.1f ms (p95 %5.1f, max %5.1f,"
        " jitter %4.1f)  buffer %3.0f ms  lost %llu  late %llu  dropped %llu%s  %s",
        name, size(v.width, v.height).c_str(), v.decoder.c_str(), v.fps, rate(v.bitrate).c_str(),
        ms(v.decode_time), ms(frame.percentile(0.5)), ms(frame.percentile(0.95)), ms(frame.max()),
        ms(v.pace.jitter()), ms(v.buffer), static_cast<unsigned long long>(v.lost),
        static_cast<unsigned long long>(v.late), static_cast<unsigned long long>(v.dropped),
        passing_on(v).c_str(), quality(v.verdict).c_str());
}

std::string sound_line(const char* name, const ReceivedSoundReport& s) {
    return format(
        "%-15s %s  %3.0f fps  delay %3.0f ms (target %3.0f)  lost %llu  late %llu  fec %4.0f ms"
        "  concealed %4.0f ms  %s",
        name, s.decoder.c_str(), s.fps, ms(s.delay), ms(s.target),
        static_cast<unsigned long long>(s.lost), static_cast<unsigned long long>(s.late),
        ms(s.recovered), ms(s.concealed), quality(s.verdict).c_str());
}

// ── Sections ────────────────────────────────────────────────────────────────

/// "16.9 ms (p95 18.9, max 22.0)".
std::string spread(const LatencyHistogram& h) {
    return format("%.1f ms (p95 %.1f, max %.1f)", ms(h.percentile(0.5)), ms(h.percentile(0.95)),
                  ms(h.max()));
}

std::string milliseconds(Duration d) { return format("%.1f ms", ms(d)); }

std::string counts(uint64_t a, uint64_t b, uint64_t c) {
    return format("%llu, %llu, %llu", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
}

ReportSection sent_section(const SentStreamReport& s) {
    ReportSection section{"Sent: " + s.label, s.verdict, {}};
    auto& rows = section.rows;
    switch (s.kind) {
        case MediaKind::Video:
            rows = {{"Size", size(s.width, s.height)},
                    {"Encoder", s.encoder + (s.gpu_input ? " (GPU)" : "")},
                    {"Frame rate", format("%.0f fps (source %.0f)", s.fps, s.source_fps)},
                    {"Bitrate", rate(s.bitrate) + (s.target > 0 ? " (aim " + rate(s.target) + ")"
                                                                : std::string())},
                    {"Encode", milliseconds(s.encode_time)},
                    {"Skipped, keyframes, dropped", counts(s.skipped, s.keyframes, s.dropped)}};
            break;
        case MediaKind::Audio:
            rows = {{"Encoder", s.encoder},
                    {"Frame rate", format("%.0f fps", s.fps)},
                    {"Bitrate", rate(s.bitrate)},
                    {"Encode", milliseconds(s.encode_time)},
                    {"Dropped", std::to_string(s.skipped)}};
            break;
        case MediaKind::Data:
            rows = {{"Frames", format("%.0f a second", s.fps)},
                    {"Bitrate", rate(s.bitrate)},
                    {"Skipped", std::to_string(s.skipped) + " bytes"}};
            break;
    }
    rows.emplace_back("Viewers", viewers(s));
    return section;
}

ReportSection video_section(std::string title, const ReceivedVideoReport& v) {
    ReportSection section{std::move(title),
                          v.verdict,
                          {{"Size", size(v.width, v.height)},
                           {"Decoder", v.decoder},
                           {"Frame rate", format("%.0f fps", v.fps)},
                           {"Bitrate", rate(v.bitrate)},
                           {"Decode", milliseconds(v.decode_time)},
                           {"Frame time", spread(v.pace.intervals())},
                           {"Frame jitter", milliseconds(v.pace.jitter())},
                           {"Buffer", milliseconds(v.buffer)},
                           {"Lost, late, dropped", counts(v.lost, v.late, v.dropped)}}};
    if (v.passed_on_to > 0) {
        section.rows.emplace_back("Passing on to", format("%zu viewers at %s", v.passed_on_to,
                                                          rate(v.passed_on_bitrate).c_str()));
    }
    return section;
}

ReportSection sound_section(std::string title, const ReceivedSoundReport& s) {
    return {std::move(title),
            s.verdict,
            {{"Decoder", s.decoder},
             {"Frame rate", format("%.0f fps", s.fps)},
             {"Delay", format("%.0f ms (target %.0f)", ms(s.delay), ms(s.target))},
             {"Lost, late", format("%llu, %llu", static_cast<unsigned long long>(s.lost),
                                   static_cast<unsigned long long>(s.late))},
             {"Recovered by FEC", milliseconds(s.recovered)},
             {"Made up", milliseconds(s.concealed)}}};
}

ReportSection path_section(const PathReport& p) {
    return {"Path to " + p.peer.short_hex(),
            p.verdict,
            {{"Round trip", milliseconds(p.rtt)},
             {"Bandwidth", (p.busy ? "" : ">= ") + rate(double(p.bandwidth))},
             {"Queue", milliseconds(p.queue)},
             {"Loss", format("%.1f%%", p.loss * 100)},
             {"Expired", std::to_string(p.expired)},
             {"Repair", rate(p.repair)}}};
}

}  // namespace

const char* to_string(Quality quality) noexcept {
    switch (quality) {
        case Quality::Good: return "good";
        case Quality::Fair: return "fair";
        case Quality::Poor: return "poor";
    }
    return "good";
}

void Verdict::at_least(Quality worse, std::string_view why) {
    if (worse <= quality) return;
    quality = worse;
    reason = why;
}

void Verdict::at_least(const Verdict& other, std::string_view part) {
    if (other.quality <= quality) return;
    quality = other.quality;
    reason = std::string(part) + ": " + other.reason;
}

CallSnapshot snapshot(const Call& call, Timestamp now) {
    CallSnapshot snapshot;
    snapshot.at = now;
    snapshot.cpu = process_cpu_time();
    snapshot.sent = call.sent_stats();
    snapshot.video = call.video_stats();
    snapshot.sound = call.sound_stats();
    snapshot.cameras = call.camera_stats();
    snapshot.voices = call.voice_stats();
    snapshot.feed = call.feed_stats();
    snapshot.sync = call.sync_stats();
    for (const PeerId& peer : call.peers()) {
        const std::optional<librats::PathStats> path = call.path(peer);
        if (path && path->valid) snapshot.paths.emplace_back(peer, *path);
    }
    return snapshot;
}

CallReport compare(const CallSnapshot& earlier, const CallSnapshot& now) {
    CallReport report;
    report.interval = now.at - earlier.at;
    const Interval interval(report.interval);
    report.cpu = CpuMeter::percent(now.cpu - earlier.cpu, report.interval);

    for (const SentStreamStats& stream : now.sent) {
        const auto before =
            std::ranges::find(earlier.sent, stream.stream, &SentStreamStats::stream);
        report.sent.push_back(
            sent_stream(stream, before != earlier.sent.end() ? &*before : nullptr, interval));
        report.verdict.at_least(report.sent.back().verdict, "sent " + stream.label);
    }
    if (now.video) {
        report.video = received_video(*now.video, optional_ptr(earlier.video), interval);
        report.verdict.at_least(report.video->verdict, "pictures");
    }
    if (now.sound) {
        report.sound = received_sound(*now.sound, optional_ptr(earlier.sound), interval);
        report.verdict.at_least(report.sound->verdict, "sound");
    }
    for (const auto& [peer, camera] : now.cameras) {
        report.cameras.push_back(received_video(camera, find(earlier.cameras, peer), interval));
        report.verdict.at_least(report.cameras.back().verdict, of("camera", peer));
    }
    for (const auto& [peer, voice] : now.voices) {
        report.voices.push_back(received_sound(voice, find(earlier.voices, peer), interval));
        report.voices.back().peer = peer;
        report.verdict.at_least(report.voices.back().verdict, of("voice", peer));
    }
    if (now.feed) {
        report.feed = received_feed(*now.feed, earlier.feed, interval);
        report.verdict.at_least(report.feed->verdict, "feed");
    }
    report.apart = now.sync.apart.since(earlier.sync.apart);
    report.together = now.sync.together;
    if (report.apart.percentile(0.95) > kApartFair) {
        report.in_step.at_least(Quality::Fair, "sound and pictures apart");
        report.verdict.at_least(report.in_step, "in step");
    }
    for (const auto& [peer, path] : now.paths) {
        const auto before =
            std::ranges::find(earlier.paths, peer, &std::pair<PeerId, librats::PathStats>::first);
        report.paths.push_back(path_report(
            peer, path, before != earlier.paths.end() ? &before->second : nullptr, interval));
        report.verdict.at_least(report.paths.back().verdict, "path to " + peer.short_hex());
    }
    return report;
}

std::vector<std::string> report_lines(const CallReport& report) {
    std::vector<std::string> lines;
    for (const SentStreamReport& sent : report.sent) lines.push_back(sent_line(sent));
    if (report.video) lines.push_back(video_line("pictures", *report.video));
    for (const ReceivedVideoReport& camera : report.cameras) {
        lines.push_back(video_line(of("camera", camera.peer).c_str(), camera));
    }
    if (report.sound) lines.push_back(sound_line("sound", *report.sound));
    for (const ReceivedSoundReport& voice : report.voices) {
        lines.push_back(sound_line(of("voice", voice.peer).c_str(), voice));
    }
    if (const auto& feed = report.feed) {
        lines.push_back(format("%-15s %s  lost %llu  late %llu  buffer %3.0f ms  %s", "feed",
                               rate(feed->bitrate).c_str(),
                               static_cast<unsigned long long>(feed->lost),
                               static_cast<unsigned long long>(feed->late), ms(feed->buffer),
                               quality(feed->verdict).c_str()));
    }
    if (report.apart.count() > 0) {
        lines.push_back(format("%-15s %4.1f ms apart (p95 %4.1f, max %4.1f)", "in step",
                               ms(report.apart.percentile(0.5)), ms(report.apart.percentile(0.95)),
                               ms(report.apart.max())));
    }
    if (report.together) {
        lines.push_back(format("%-15s every viewer at %3.0f ms", "together", ms(*report.together)));
    }
    for (const PathReport& p : report.paths) {
        lines.push_back(format(
            "path %-10s rtt %5.1f ms  bandwidth %s%s  queue %4.0f ms  loss %4.1f%%  expired %llu"
            "  repair %s  %s",
            // Not kept busy, the estimate is what was used: a floor.
            p.peer.short_hex().c_str(), ms(p.rtt),
            p.busy ? "" : ">= ", rate(double(p.bandwidth)).c_str(), ms(p.queue), p.loss * 100,
            static_cast<unsigned long long>(p.expired), rate(p.repair).c_str(),
            quality(p.verdict).c_str()));
    }
    lines.push_back(
        format("%-15s cpu %3.0f%%  %s", "call", report.cpu, quality(report.verdict).c_str()));
    return lines;
}

std::string report_text(const CallReport& report, unsigned second) {
    std::string text;
    for (const std::string& line : report_lines(report)) {
        text += text.empty() ? format("[%4us] ", second) : std::string(8, ' ');
        text += line + "\n";
    }
    return text;
}

std::vector<ReportSection> report_sections(const CallReport& report) {
    std::vector<ReportSection> sections;
    sections.push_back({"Call", report.verdict, {{"CPU", format("%.0f%%", report.cpu)}}});
    for (const SentStreamReport& sent : report.sent) sections.push_back(sent_section(sent));
    if (report.video) sections.push_back(video_section("Pictures", *report.video));
    for (const ReceivedVideoReport& camera : report.cameras) {
        sections.push_back(video_section(of("Camera of", camera.peer), camera));
    }
    if (report.sound) sections.push_back(sound_section("Sound", *report.sound));
    for (const ReceivedSoundReport& voice : report.voices) {
        sections.push_back(sound_section(of("Voice of", voice.peer), voice));
    }
    if (const auto& feed = report.feed) {
        sections.push_back(
            {"Feed",
             feed->verdict,
             {{"Bitrate", rate(feed->bitrate)},
              {"Lost, late", format("%llu, %llu", static_cast<unsigned long long>(feed->lost),
                                    static_cast<unsigned long long>(feed->late))},
              {"Buffer", milliseconds(feed->buffer)}}});
    }
    if (report.apart.count() > 0 || report.together) {
        ReportSection in_step{"In step", report.in_step, {}};
        if (report.apart.count() > 0) in_step.rows.emplace_back("Apart", spread(report.apart));
        if (report.together)
            in_step.rows.emplace_back("Every viewer at", milliseconds(*report.together));
        sections.push_back(std::move(in_step));
    }
    for (const PathReport& path : report.paths) sections.push_back(path_section(path));
    return sections;
}

CallMonitor::CallMonitor(const Call& call, Clock& clock)
    : call_(call), clock_(clock), last_(snapshot(call, clock.now())) {}

CallReport CallMonitor::report() {
    CallSnapshot now = snapshot(call_, clock_.now());
    CallReport report = compare(last_, now);
    last_ = std::move(now);
    return report;
}

}  // namespace ratcast
