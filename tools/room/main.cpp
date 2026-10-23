// ratcast-room: a member of a room without a window — what the app is, with a
// stamped tone for a voice and the stamped test pattern for a screen, if asked.
// Makes a room or joins one by any member's link, plays every voice and the
// share it watches to nobody, and prints a report a second with each voice's
// latency and, on exit, a summary line of key=value pairs.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include "common/arguments.h"
#include "common/tool.h"
#include "ratcast/client/call.h"
#include "ratcast/client/call_monitor.h"
#include "ratcast/client/cpu_meter.h"
#include "ratcast/client/playback_meter.h"
#include "ratcast/client/screen_share.h"
#include "ratcast/client/voice.h"
#include "ratcast/client/voice_meter.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

using namespace ratcast;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

double ms(Duration d) { return double(d.count()) / 1000; }

constexpr const char* kUsage = R"(usage: ratcast-room [rats://HOST:PORT | rats://PEER-ID] [options]

  rats://...           a member's link: join the room it is in; none: make a room
  --name NAME          who this member says it is (default: member)
  --silent             no voice; by default a tone, stamped as the pattern is
  --share              share the test pattern as a screen, as the app shares one
  --size WxH           the pattern's size (default 1280x720)
  --codec av1|h265|h264   default: av1
  --bitrate KBPS       the most the share's rate control goes up to (default 4000)
  --fps N              the pattern's frames a second (default 30)
  --port N             listen port (default 7700; 0 for any)
  --duration S         leave after S seconds
  --data-dir DIR       keep the peer id across runs
  --verbose            library logs

Every voice and the share watched (the first offered) play to nobody, in real
time. The tone's and the pattern's stamps carry the time of day they were
captured: on one host, or hosts whose clocks are synchronised, each voice's
latency from capture to the speaker is printed, and the share's to the picture.
On exit a summary line, key=value, sums up the run.
)";

struct Options {
    std::optional<RatsUrl> room;
    std::string name = "member";
    bool voice = true;
    bool share = false;
    uint32_t width = 1280;
    uint32_t height = 720;
    VideoEncoderConfig encoder;
    uint16_t port = 7700;
    uint32_t duration = 0;
    std::string data_dir;
    bool verbose = false;
};

std::optional<Options> parse(int argc, char** argv) {
    Arguments args(argc, argv);
    if (args.flag("help")) {
        std::cout << kUsage;
        std::exit(0);
    }
    Options o;
    bool ok = true;
    o.name = args.text("name").value_or(o.name);
    if (!is_valid(Presence{.name = o.name})) {
        std::cerr << "ratcast-room: --name wants at most 128 bytes of text\n";
        ok = false;
    }
    o.voice = !args.flag("silent");
    o.share = args.flag("share");
    if (const auto size = args.text("size")) {
        if (const auto parsed = parse_size(*size)) {
            std::tie(o.width, o.height) = *parsed;
        } else {
            std::cerr << "ratcast-room: --size wants WxH, like 1280x720\n";
            ok = false;
        }
    }
    if (const auto codec = args.text("codec")) {
        if (const std::optional<Codec> parsed = parse_codec(*codec)) {
            o.encoder.codec = *parsed;
        } else {
            std::cerr << "ratcast-room: --codec wants av1, h265 or h264\n";
            ok = false;
        }
    }
    o.encoder.bitrate = args.number("bitrate", 50, 500'000).value_or(4000) * 1000;
    o.encoder.frame_rate_num = args.number("fps", 1, 240).value_or(30);
    o.port = static_cast<uint16_t>(args.number("port", 0, 65535).value_or(7700));
    o.duration = args.number("duration", 1, 1'000'000).value_or(0);
    o.data_dir = args.text("data-dir").value_or("");
    o.verbose = args.flag("verbose");
    const std::vector<std::string> links = args.positional();
    for (const std::string& e : args.errors()) {
        std::cerr << "ratcast-room: " << e << "\n";
        ok = false;
    }
    if (links.size() > 1) {
        std::cerr << "ratcast-room: one room at a time\n";
        ok = false;
    } else if (links.size() == 1) {
        o.room = parse_rats_url(links[0]);
        if (!o.room) {
            std::cerr << "ratcast-room: '" << links[0] << "' is no rats:// link\n";
            ok = false;
        }
    }
    return ok ? std::optional(o) : std::nullopt;
}

/// A peer by the name it gave, and its short id.
std::string who(const Call& call, const PeerId& peer) {
    const std::optional<Presence> presence = call.presence(peer);
    const std::string name = presence && !presence->name.empty() ? presence->name : "?";
    return name + " (" + peer.short_hex() + ")";
}

/// A report a second, from the call's monitor, with each voice's latency;
/// a summary at the end.
class Reporter {
public:
    Reporter(Call& call, VoiceMeter& voices, PlaybackMeter& pictures)
        : call_(call), monitor_(call), voices_(voices), pictures_(pictures) {}

    void report() {
        std::fputs(report_text(monitor_.report(), seconds_++).c_str(), stdout);
        for (const auto& [peer, voice] : voices_.take_intervals()) {
            std::printf("        %-14s%-28s", "voice", who(call_, peer).c_str());
            if (voice.pictures == 0) {
                std::printf("  silent\n");
                continue;
            }
            print_latency(voice.latency);
            if (voice.stalls > 0) {
                std::printf("  breaks %llu (%.0f ms)",
                            static_cast<unsigned long long>(voice.stalls), ms(voice.stalled));
            }
            std::printf("\n");
        }
        const PlaybackStats pictures = pictures_.take_interval();
        if (pictures.latency.count() > 0) {
            std::printf("        %-14s%-28s", "share", "");
            print_latency(pictures.latency);
            std::printf("\n");
        }
        // The last word on each voice, for a summary after it left.
        for (auto& [peer, stats] : call_.voice_stats()) last_voices_[peer] = std::move(stats);
        std::fflush(stdout);
    }

    /// The whole run as key=value pairs, for scripts: every voice together
    /// (voices_ms: when the last of them was first heard), the share watched,
    /// and what this member sent. Call before the call ends.
    void summarise() {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started_;
        const double seconds = std::max(elapsed.count(), 1.0);
        std::printf("summary cpu=%.0f", cpu_.total());
        summarise_voices();
        summarise_share(seconds);
        summarise_sent(seconds);
        std::printf("\n");
        std::fflush(stdout);
    }

private:
    void summarise_voices() {
        const std::map<PeerId, PlaybackStats> heard = voices_.voices();
        const PlaybackStats all = VoiceMeter::together(heard);
        uint64_t lost = 0, late = 0;
        Duration recovered{}, concealed{};
        for (const auto& [peer, voice] : last_voices_) {
            lost += voice.jitter.lost;
            late += voice.jitter.late;
            recovered += voice.decoded.recovered;
            concealed += voice.decoded.concealed;
        }
        std::printf(
            " voices=%zu voices_ms=%.0f voice_frames=%llu voice_breaks=%llu voice_broken_ms=%.0f"
            " voice_longest_gap_ms=%.0f voice_lost=%llu voice_late=%llu voice_recovered_ms=%.0f"
            " voice_concealed_ms=%.0f",
            heard.size(), all.first_picture ? ms(*all.first_picture) : -1,
            static_cast<unsigned long long>(all.pictures),
            static_cast<unsigned long long>(all.stalls), ms(all.stalled), ms(all.longest_gap),
            static_cast<unsigned long long>(lost), static_cast<unsigned long long>(late),
            ms(recovered), ms(concealed));
        print_percentiles("voice_latency", all.latency);
    }

    void summarise_share(double seconds) {
        const PlaybackStats pictures = pictures_.total();
        if (pictures.pictures == 0) return;
        std::printf(" first_picture_ms=%.0f pictures=%llu fps=%.1f stalls=%llu stalled_ms=%.0f",
                    pictures.first_picture ? ms(*pictures.first_picture) : -1,
                    static_cast<unsigned long long>(pictures.pictures),
                    double(pictures.pictures) / seconds,
                    static_cast<unsigned long long>(pictures.stalls), ms(pictures.stalled));
        print_percentiles("latency", pictures.latency);
    }

    /// What this member's streams carried, before they went to each peer.
    void summarise_sent(double seconds) {
        uint64_t voice = 0, share = 0;
        for (const SentStreamStats& sent : call_.sent_stats()) {
            if (const auto* sound = std::get_if<AudioPublisherStats>(&sent.publisher))
                voice += sound->bytes;
            if (const auto* video = std::get_if<VideoPublisherStats>(&sent.publisher))
                share += video->bytes;
        }
        std::printf(" voice_kbps=%.1f share_mbps=%.2f", double(voice) * 8 / 1e3 / seconds,
                    double(share) * 8 / 1e6 / seconds);
    }

    static void print_latency(const LatencyHistogram& latency) {
        if (latency.count() == 0) return;
        std::printf("  %5.1f ms (p95 %5.1f, worst %5.1f)", ms(latency.mean()),
                    ms(latency.percentile(0.95)), ms(latency.max()));
    }

    static void print_percentiles(const char* name, const LatencyHistogram& latency) {
        if (latency.count() == 0) return;
        std::printf(" %s_mean_ms=%.1f %s_p50_ms=%.1f %s_p95_ms=%.1f %s_p99_ms=%.1f %s_max_ms=%.1f",
                    name, ms(latency.mean()), name, ms(latency.percentile(0.5)), name,
                    ms(latency.percentile(0.95)), name, ms(latency.percentile(0.99)), name,
                    ms(latency.max()));
    }

    Call& call_;
    CallMonitor monitor_;
    VoiceMeter& voices_;
    PlaybackMeter& pictures_;
    CpuMeter cpu_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::map<PeerId, AudioPlayerStats> last_voices_;
    unsigned seconds_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::cerr << "\n" << kUsage;
        return 2;
    }
    const Options& o = *options;
    set_verbose(o.verbose);
    catch_interrupts();

    // Everyone's voice and the share watched, played in real time to nobody;
    // each voice and the pictures measured from the moment we start.
    VirtualOutput speaker{VirtualOutputConfig{.format = {48'000, 2}}};
    VoiceMeter voices;
    PlaybackMeter pictures;
    const CallPlayback playback{
        .pictures = [&](const VideoFrame& picture) { pictures.picture(stamped_latency(picture)); },
        .sound = &speaker,
        .heard =
            [&](const PeerId& peer, Timestamp capture_time, Timestamp play_time) {
                voices.heard(peer, stamped_latency(capture_time, play_time));
            },
    };
    const CallHandlers handlers{
        .joined = [](const PeerId& peer) { std::printf("joined: %s\n", peer.short_hex().c_str()); },
        .left = [](const PeerId& peer) { std::printf("left: %s\n", peer.short_hex().c_str()); },
        .playing =
            [](const PeerId& peer, const StreamDescription& stream, Result<void> outcome) {
                if (!outcome) {
                    std::printf("cannot play '%s' from %s: %s\n", stream.label.c_str(),
                                peer.short_hex().c_str(), to_string(outcome.error()));
                }
            },
        .presence =
            [](const PeerId& peer, const Presence& presence) {
                std::printf("%s is %s\n", peer.short_hex().c_str(), presence.name.c_str());
            },
    };

    voices.start();
    pictures.start();
    Result<std::unique_ptr<Call>> started = Call::start({.port = o.port,
                                                         .data_dir = o.data_dir,
                                                         .dial = o.room,
                                                         .mesh = true,
                                                         .playback = playback},
                                                        handlers);
    if (!started) {
        std::cerr << "ratcast-room: cannot listen on port " << o.port << "\n";
        return 1;
    }
    Call& call = **started;
    (void)call.set_presence({.name = o.name});

    if (o.voice) {
        // Stamped with the time of day, as the pattern is. Loud enough for
        // Opus to take it for speech, and code FEC for it.
        auto tone = std::make_unique<ToneSource>(ToneConfig{.level = 0.5f}, WallClock::instance());
        if (const Result<AudioPublisher*> voice = publish_voice(call, std::move(tone)); !voice) {
            std::cerr << "ratcast-room: the voice did not start: " << to_string(voice.error())
                      << "\n";
            return 1;
        }
    }
    if (o.share) {
        ScreenShareConfig share{.sound = false, .video = o.encoder};
        auto pattern = std::make_unique<PatternSource>(
            PatternConfig{PixelFormat::NV12, o.width & ~1u, o.height & ~1u,
                          o.encoder.frame_rate_num, 1},
            WallClock::instance());
        if (const Result<void> shared =
                publish_screen_share(call, {.pictures = std::move(pattern)}, share);
            !shared) {
            std::cerr << "ratcast-room: the share did not start: " << to_string(shared.error())
                      << "\n";
            return 1;
        }
    }

    std::printf("peer %s, %s\n", call.id().to_hex().c_str(),
                o.room ? "joining" : "a room of its own");
    std::vector<std::string> invites = call.invites();
    if (invites.empty())
        invites.push_back(to_string(RatsUrl{librats::HostEndpoint{"<this host>", call.port()}}));
    const char* lead = "members join with: ";
    for (const std::string& invite : invites) {
        std::printf("%sratcast-room %s\n", lead, invite.c_str());
        lead = "               or: ";
    }
    std::fflush(stdout);

    Reporter reporter(call, voices, pictures);
    const auto start = std::chrono::steady_clock::now();
    auto next = start + 1s;
    while (!interrupted()) {
        std::this_thread::sleep_for(20ms);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += 1s;
            reporter.report();
        }
        if (o.duration > 0 && now - start >= std::chrono::seconds(o.duration)) break;
    }
    reporter.summarise();
    started->reset();
    return 0;
}
