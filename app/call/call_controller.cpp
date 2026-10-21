#include "call_controller.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QStandardPaths>

#include <filesystem>
#include <mutex>
#include <utility>

namespace ratcast::app {
namespace {

QString describe(Errc error) { return QString::fromLatin1(to_string(error)); }

}  // namespace

/// Where the call's pictures go: whichever view is set, from the player's
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
    environment.open_speaker = []() -> std::unique_ptr<AudioOutput> {
        Result<std::unique_ptr<AudioOutput>> speaker = open_audio_output({});
        return speaker ? std::move(*speaker) : nullptr;
    };
    return environment;
}

CallController::CallController(QObject* parent)
    : QObject(parent),
      environment_(Environment::system()),
      pictures_(std::make_shared<Pictures>()) {}

CallController::~CallController() {
    ++generation_;
    choosing_.request_stop();
    if (choosing_.joinable()) choosing_.join();
    call_.reset();
    speaker_.reset();
}

void CallController::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
}

void CallController::setVideo(VideoView* video) {
    if (video == video_) return;
    video_ = video;
    pictures_->set(video ? video->sink() : VideoView::Sink{});
    Q_EMIT videoChanged();
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
    CallConfig config_of_call = environment_.call;
    config_of_call.data_dir = environment_.data_dir;
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
    speaker_ = environment_.open_speaker ? environment_.open_speaker() : nullptr;

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
    config.playback.sound = speaker_.get();
    Result<std::unique_ptr<Call>> started = Call::start(std::move(config), handlers(generation));
    if (!started) {
        end(tr("The call cannot start (%1).").arg(describe(started.error())));
        return;
    }
    call_ = std::move(*started);
    set_state(State::Joining);
}

// ── Either ──────────────────────────────────────────────────────────────────

void CallController::leave() { end(); }

void CallController::copy(const QString& text) { QGuiApplication::clipboard()->setText(text); }

void CallController::end(QString notice) {
    ++generation_;
    choosing_.request_stop();
    if (choosing_.joinable()) choosing_.join();
    call_.reset();
    speaker_.reset();
    if (video_) video_->clear();
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
                    if (state_ != State::Watching && state_ != State::Joining) return;
                    if (video_) video_->clear();
                    set_notice(tr("The other side left."));
                    set_state(State::Joining);
                });
            },
        .playing =
            [this, generation](const PeerId&, const StreamDescription& stream,
                               Result<void> outcome) {
                if (stream.kind() != MediaKind::Video && outcome) return;
                const QString label = QString::fromStdString(stream.label);
                post(generation, [this, outcome, label] {
                    if (!outcome) {
                        set_notice(tr("'%1' cannot be played (%2).")
                                       .arg(label, describe(outcome.error())));
                        return;
                    }
                    set_notice({});
                    set_state(State::Watching);
                });
            },
        .ended =
            [this, generation](const PeerId&, const StreamDescription& stream) {
                if (stream.kind() != MediaKind::Video) return;
                post(generation, [this] {
                    if (video_) video_->clear();
                    if (state_ == State::Watching) set_state(State::Joining);
                });
            },
        .source_ended =
            [this, generation](StreamId, Result<void> outcome) {
                post(generation, [this, outcome] {
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
    if (notice == notice_) return;
    notice_ = std::move(notice);
    Q_EMIT noticeChanged();
}

}  // namespace ratcast::app
