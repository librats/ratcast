#include "call_controller.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <utility>

namespace ratcast::app {
namespace {

QString describe(Errc error) { return QString::fromLatin1(to_string(error)); }

/// A report's sections as QML reads them.
QVariantList to_variant(const std::vector<ReportSection>& sections) {
    QVariantList list;
    for (const ReportSection& section : sections) {
        QVariantList rows;
        for (const auto& [label, value] : section.rows) {
            rows.push_back(QVariantMap{{QStringLiteral("label"), QString::fromStdString(label)},
                                       {QStringLiteral("value"), QString::fromStdString(value)}});
        }
        list.push_back(QVariantMap{
            {QStringLiteral("title"), QString::fromStdString(section.title)},
            {QStringLiteral("quality"), QString::fromLatin1(to_string(section.verdict.quality))},
            {QStringLiteral("reason"), QString::fromStdString(section.verdict.reason)},
            {QStringLiteral("rows"), rows},
        });
    }
    return list;
}

}  // namespace

/// Where pictures go: whichever view is set, from a player's or a source's
/// thread.
struct CallController::Pictures {
    std::mutex mutex;
    VideoView::Sink sink;

    void set(VideoView::Sink to) {
        std::lock_guard lock(mutex);
        sink = std::move(to);
    }
    void operator()(VideoFrame picture) {
        std::lock_guard lock(mutex);
        if (sink) sink(std::move(picture));
    }
};

CallController::Environment CallController::Environment::system() {
    Environment environment;
    environment.call.discoverable = true;
    environment.data_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString();
    environment.open_share = [](const ScreenShareConfig& config, std::stop_token cancel) {
        return open_screen_share(config, std::move(cancel));
    };
    environment.open_speaker = [format = environment.voice.speaker] {
        Result<std::unique_ptr<AudioOutput>> speaker = open_audio_output({.format = format});
        return speaker ? std::move(*speaker) : nullptr;
    };
    environment.open_microphone = [format = environment.voice.microphone] {
        Result<std::unique_ptr<AudioSource>> microphone = open_audio_capture({.format = format});
        return microphone ? std::move(*microphone) : nullptr;
    };
    environment.open_camera = []() -> std::unique_ptr<VideoSource> {
        Result<std::unique_ptr<VideoSource>> camera = ratcast::open_camera(call_camera_options());
        return camera ? std::move(*camera) : nullptr;
    };
    environment.open_system_sound = [format = environment.voice.speaker] {
        Result<std::unique_ptr<AudioSource>> sound =
            open_audio_capture({.format = format, .source = AudioCaptureSource::SystemSound});
        return sound ? std::move(*sound) : nullptr;
    };
    return environment;
}

CallController::CallController(QObject* parent)
    : QObject(parent),
      environment_(Environment::system()),
      pictures_(std::make_shared<Pictures>()),
      camera_pictures_(std::make_shared<Pictures>()),
      preview_pictures_(std::make_shared<Pictures>()) {
    levels_.setInterval(100);
    connect(&levels_, &QTimer::timeout, this, &CallController::update_levels);
    reporting_.setInterval(1000);
    connect(&reporting_, &QTimer::timeout, this, &CallController::update_stats);
    connect(&printing_, &QTimer::timeout, this, &CallController::print_stats);
}

CallController::~CallController() {
    ++generation_;
    choosing_.request_stop();
    if (choosing_.joinable()) choosing_.join();
    monitor_.reset();
    printer_.reset();
    call_.reset();
    speaker_.reset();
    voice_.reset();
}

void CallController::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
}

bool CallController::attach(QPointer<VideoView>& slot, Pictures& pictures, VideoView* view) {
    if (view == slot) return false;
    slot = view;
    pictures.set(view ? view->sink() : VideoView::Sink{});
    return true;
}

void CallController::setVideo(VideoView* video) {
    if (attach(video_, *pictures_, video)) Q_EMIT videoChanged();
}

void CallController::setCameraView(VideoView* view) {
    if (attach(camera_view_, *camera_pictures_, view)) Q_EMIT cameraViewChanged();
}

void CallController::setPreview(VideoView* view) {
    if (attach(preview_, *preview_pictures_, view)) Q_EMIT previewChanged();
}

// ── Sharing ─────────────────────────────────────────────────────────────────

void CallController::share(bool window, bool sound) {
    if (state_ != State::Idle) return;
    set_notice({});
    ScreenShareConfig config;
    config.window = window;
    config.sound = sound;
    config.data_dir = environment_.data_dir;
    const uint64_t generation = ++generation_;
    set_state(State::Choosing);
    // The desktop's dialog waits on the user: off the GUI thread.
    choosing_ = std::jthread(
        [this, generation, config, open = environment_.open_share](std::stop_token cancel) {
            auto sources = std::make_shared<Result<ScreenShareSources>>(open(config, cancel));
            QMetaObject::invokeMethod(
                this,
                [this, generation, config, sources] {
                    share_opened(generation, config, std::move(*sources));
                },
                Qt::QueuedConnection);
        });
}

void CallController::share_opened(uint64_t generation, const ScreenShareConfig& config,
                                  Result<ScreenShareSources> sources) {
    if (generation != generation_) return;  // left meanwhile
    if (!sources) {
        end(sources.error() == Errc::Cancelled
                ? QString()
                : tr("The screen cannot be shared (%1).").arg(describe(sources.error())));
        return;
    }
    open_voice();
    CallConfig config_of_call = environment_.call;
    config_of_call.data_dir = environment_.data_dir;
    config_of_call.playback.sound = speaker_.get();  // the viewers' voices
    config_of_call.playback.camera = [pictures = camera_pictures_](VideoFrame picture) {
        (*pictures)(std::move(picture));
    };
    Result<std::unique_ptr<Call>> started =
        Call::start(std::move(config_of_call), handlers(generation));
    if (!started) {
        end(tr("The call cannot start (%1).").arg(describe(started.error())));
        return;
    }
    call_ = std::move(*started);
    if (Result<void> shared = publish_screen_share(*call_, std::move(*sources), config); !shared) {
        end(tr("The screen cannot be shared (%1).").arg(describe(shared.error())));
        return;
    }
    invites_.clear();
    for (const std::string& invite : call_->invites()) {
        invites_.push_back(QString::fromStdString(invite));
    }
    Q_EMIT invitesChanged();
    count_viewers();
    set_state(State::Sharing);
    start_monitoring();
    talk();
}

// ── Watching ────────────────────────────────────────────────────────────────

void CallController::join(const QString& link) {
    if (state_ != State::Idle) return;
    const std::optional<RatsUrl> url = parse_rats_url(link.trimmed().toStdString());
    if (!url) {
        set_notice(tr("That is not a rats:// link."));
        return;
    }
    set_notice({});
    const uint64_t generation = ++generation_;
    open_voice();

    CallConfig config = environment_.call;
    // An identity of its own, so that a host can share and watch at once.
    if (!environment_.data_dir.empty()) {
        config.data_dir = (std::filesystem::path(environment_.data_dir) / "guest").string();
    }
    config.listen = false;
    config.discoverable = false;
    config.dial = *url;
    config.playback.pictures = [pictures = pictures_](VideoFrame picture) {
        (*pictures)(std::move(picture));
    };
    config.playback.camera = [pictures = camera_pictures_](VideoFrame picture) {
        (*pictures)(std::move(picture));
    };
    config.playback.sound = speaker_.get();
    Result<std::unique_ptr<Call>> started = Call::start(std::move(config), handlers(generation));
    if (!started) {
        end(tr("The call cannot start (%1).").arg(describe(started.error())));
        return;
    }
    call_ = std::move(*started);
    set_state(State::Joining);
    start_monitoring();
    talk();
}

// ── Either ──────────────────────────────────────────────────────────────────

void CallController::open_voice() {
    voice_ = std::make_unique<VoiceChannel>(environment_.voice);
    voice_->set_muted(muted_);
    std::unique_ptr<AudioOutput> device =
        environment_.open_speaker ? environment_.open_speaker() : nullptr;
    speaker_ = device ? voice_->speaker(std::move(device)) : nullptr;
}

void CallController::talk() {
    levels_.start();
    if (camera_) start_camera();
    std::unique_ptr<AudioSource> device =
        environment_.open_microphone ? environment_.open_microphone() : nullptr;
    // Not heard is no reason to end the call: said, and the call goes on.
    if (!device) {
        set_notice(tr("There is no microphone: the others will not hear you."));
        return;
    }
    std::unique_ptr<AudioSource> echo =
        environment_.open_system_sound ? environment_.open_system_sound() : nullptr;
    if (Result<AudioPublisher*> published =
            publish_voice(*call_, voice_->microphone(std::move(device), std::move(echo)));
        !published) {
        set_notice(tr("The microphone cannot be used (%1).").arg(describe(published.error())));
    }
}

void CallController::setCamera(bool on) {
    if (on == camera_) return;
    if (!on) {
        camera_off();
        return;
    }
    camera_ = true;
    Q_EMIT cameraChanged();
    if (call_) start_camera();
}

void CallController::start_camera() {
    std::unique_ptr<VideoSource> device =
        environment_.open_camera ? environment_.open_camera() : nullptr;
    if (!device) {
        camera_off(tr("There is no camera."));
        return;
    }
    std::unique_ptr<VideoSource> seen = with_preview(
        std::move(device),
        [pictures = preview_pictures_](const VideoFrame& picture) { (*pictures)(picture); });
    if (Result<VideoPublisher*> published = publish_camera(*call_, std::move(seen)); !published) {
        camera_off(tr("The camera cannot be used (%1).").arg(describe(published.error())));
    }
}

void CallController::camera_off(QString notice) {
    if (call_) call_->unpublish(kCameraStream);
    if (preview_) preview_->clear();
    if (!notice.isEmpty()) set_notice(std::move(notice));
    if (!camera_) return;
    camera_ = false;
    Q_EMIT cameraChanged();
}

void CallController::set_camera_shown(bool shown) {
    if (!shown && camera_view_) camera_view_->clear();
    if (shown == camera_shown_) return;
    camera_shown_ = shown;
    Q_EMIT cameraShownChanged();
}

void CallController::setMuted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    if (voice_) voice_->set_muted(muted);
    Q_EMIT mutedChanged();
    update_levels();
}

void CallController::update_levels() {
    const bool speaking = voice_ && voice_->speaking();
    const bool hearing = voice_ && voice_->hearing();
    if (speaking != speaking_) {
        speaking_ = speaking;
        Q_EMIT speakingChanged();
    }
    if (hearing != hearing_) {
        hearing_ = hearing;
        Q_EMIT hearingChanged();
    }
}

void CallController::setStatsEvery(int seconds) {
    seconds = std::max(seconds, 0);
    if (seconds == stats_every_) return;
    stats_every_ = seconds;
    Q_EMIT statsEveryChanged();
    if (!call_) return;
    printer_.reset();
    printing_.stop();
    if (seconds == 0) return;
    printer_ = std::make_unique<CallMonitor>(*call_);
    printing_.start(seconds * 1000);
}

void CallController::start_monitoring() {
    monitor_ = std::make_unique<CallMonitor>(*call_);
    reporting_.start();
    printed_seconds_ = 0;
    if (stats_every_ > 0) {
        printer_ = std::make_unique<CallMonitor>(*call_);
        printing_.start(stats_every_ * 1000);
    }
}

void CallController::stop_monitoring() {
    reporting_.stop();
    printing_.stop();
    monitor_.reset();
    printer_.reset();
    if (stats_.isEmpty() && quality_.isEmpty()) return;
    stats_.clear();
    quality_.clear();
    quality_reason_.clear();
    Q_EMIT statsChanged();
}

void CallController::update_stats() {
    if (!monitor_) return;
    const CallReport report = monitor_->report();
    stats_ = to_variant(report_sections(report));
    quality_ = QString::fromLatin1(to_string(report.verdict.quality));
    quality_reason_ = QString::fromStdString(report.verdict.reason);
    Q_EMIT statsChanged();
}

void CallController::print_stats() {
    if (!printer_) return;
    printed_seconds_ += unsigned(stats_every_);
    std::fputs(report_text(printer_->report(), printed_seconds_).c_str(), stdout);
    std::fflush(stdout);
}

void CallController::leave() { end(); }

void CallController::copy(const QString& text) { QGuiApplication::clipboard()->setText(text); }

void CallController::end(QString notice) {
    ++generation_;
    choosing_.request_stop();
    if (choosing_.joinable()) choosing_.join();
    levels_.stop();
    stop_monitoring();
    call_.reset();
    speaker_.reset();
    voice_.reset();
    update_levels();
    if (video_) video_->clear();
    if (preview_) preview_->clear();
    set_camera_shown(false);
    if (!invites_.isEmpty()) {
        invites_.clear();
        Q_EMIT invitesChanged();
    }
    if (viewers_ != 0) {
        viewers_ = 0;
        Q_EMIT viewersChanged();
    }
    set_notice(std::move(notice));
    set_state(State::Idle);
}

CallHandlers CallController::handlers(uint64_t generation) {
    return CallHandlers{
        .joined = [this,
                   generation](const PeerId&) { post(generation, [this] { count_viewers(); }); },
        .left =
            [this, generation](const PeerId&) {
                post(generation, [this] {
                    count_viewers();
                    set_camera_shown(false);
                    if (state_ != State::Watching && state_ != State::Joining) return;
                    if (video_) video_->clear();
                    set_notice(tr("The other side left."));
                    other_side_left_ = true;
                    set_state(State::Joining);
                });
            },
        .playing =
            [this, generation](const PeerId&, const StreamDescription& stream,
                               Result<void> outcome) {
                if (stream.kind() != MediaKind::Video && outcome) return;
                const QString label = QString::fromStdString(stream.label);
                const bool camera = stream.label == kCameraLabel;
                post(generation, [this, outcome, label, camera] {
                    if (!outcome) {
                        set_notice(tr("'%1' cannot be played (%2).")
                                       .arg(label, describe(outcome.error())));
                        return;
                    }
                    if (camera) {
                        set_camera_shown(true);
                        return;
                    }
                    // Back: no longer news. What else was said stands.
                    if (other_side_left_) set_notice({});
                    set_state(State::Watching);
                });
            },
        .ended =
            [this, generation](const PeerId&, const StreamDescription& stream) {
                if (stream.kind() != MediaKind::Video) return;
                const bool camera = stream.label == kCameraLabel;
                post(generation, [this, camera] {
                    if (camera) {
                        set_camera_shown(false);
                        return;
                    }
                    if (video_) video_->clear();
                    if (state_ == State::Watching) set_state(State::Joining);
                });
            },
        .source_ended =
            [this, generation](StreamId stream, Result<void> outcome) {
                post(generation, [this, stream, outcome] {
                    // The camera going is the camera's business; the screen's,
                    // the share's.
                    if (stream == kCameraStream) {
                        camera_off(
                            outcome ? tr("The camera went.")
                                    : tr("The camera failed (%1).").arg(describe(outcome.error())));
                        return;
                    }
                    end(outcome ? tr("Sharing stopped.")
                                : tr("Sharing failed (%1).").arg(describe(outcome.error())));
                });
            },
    };
}

void CallController::post(uint64_t generation, std::function<void()> event) {
    QMetaObject::invokeMethod(
        this,
        [this, generation, event = std::move(event)] {
            if (generation == generation_) event();
        },
        Qt::QueuedConnection);
}

void CallController::count_viewers() {
    const int viewers = call_ && state_ != State::Joining && state_ != State::Watching
                            ? int(call_->peers().size())
                            : 0;
    if (viewers == viewers_) return;
    viewers_ = viewers;
    Q_EMIT viewersChanged();
}

void CallController::set_state(State state) {
    if (state == state_) return;
    state_ = state;
    Q_EMIT stateChanged();
}

void CallController::set_notice(QString notice) {
    other_side_left_ = false;
    if (notice == notice_) return;
    notice_ = std::move(notice);
    Q_EMIT noticeChanged();
}

}  // namespace ratcast::app
