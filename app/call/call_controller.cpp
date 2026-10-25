#include "call_controller.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

namespace ratcast::app {
namespace {

QString describe(Errc error) { return QString::fromLatin1(to_string(error)); }

QString id_of(const PeerId& peer) { return QString::fromStdString(peer.to_hex()); }

/// `name` as a presence may carry it: whitespace runs made one space, control
/// characters dropped, and cut to whole characters within kMaxNameSize bytes.
std::string presence_name(const QString& name) {
    QString kept;
    for (const QChar c : name.simplified()) {
        if (c.category() != QChar::Other_Control) kept += c;
    }
    while (kept.toUtf8().size() > qsizetype(Presence::kMaxNameSize)) {
        kept.chop(kept.back().isLowSurrogate() ? 2 : 1);  // a whole pair, never half
    }
    return kept.toStdString();
}

/// The login name of the user running this.
QString user_name() {
    QString name = qEnvironmentVariable("USER");
    if (name.isEmpty()) name = qEnvironmentVariable("USERNAME");  // Windows
    return name;
}

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

/// Where each member's camera goes, by member id: asked by a camera's player
/// as it starts, set by QML as it shows one, whichever comes first.
struct CallController::Cameras {
    std::mutex mutex;
    std::map<QString, std::shared_ptr<Pictures>> by_member;

    std::shared_ptr<Pictures> of(const QString& id) {
        std::lock_guard lock(mutex);
        std::shared_ptr<Pictures>& pictures = by_member[id];
        if (!pictures) pictures = std::make_shared<Pictures>();
        return pictures;
    }
    void clear() {
        std::lock_guard lock(mutex);
        by_member.clear();
    }
};

QString data_folder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/ratcast");
}

CallController::Environment CallController::Environment::system() {
    Environment environment;
    environment.name = user_name();
    environment.server = QStringLiteral(RATCAST_DEFAULT_SERVER);
    environment.call.discoverable = true;
    environment.data_dir = data_folder().toStdString();
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
    environment.keep_screen_awake = [] { return ratcast::keep_screen_awake("Watching a share"); };
    environment.settings = [] { return std::make_unique<QSettings>(); };
    return environment;
}

CallController::CallController(QObject* parent)
    : QObject(parent),
      environment_(Environment::system()),
      name_(environment_.name),
      server_(environment_.server),
      pictures_(std::make_shared<Pictures>()),
      preview_pictures_(std::make_shared<Pictures>()),
      cameras_(std::make_shared<Cameras>()) {
    levels_.setInterval(100);
    connect(&levels_, &QTimer::timeout, this, [this] {
        update_levels();
        update_members();
    });
    reporting_.setInterval(1000);
    connect(&reporting_, &QTimer::timeout, this, &CallController::update_stats);
    connect(&printing_, &QTimer::timeout, this, &CallController::print_stats);
    load_hearing();
}

CallController::~CallController() {
    ++generation_;
    dialog_.request_stop();
    if (dialog_.joinable()) dialog_.join();
    monitor_.reset();
    printer_.reset();
    call_.reset();
    speaker_.reset();
    voice_.reset();
}

void CallController::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
    setName(environment_.name);
    setServer(environment_.server);
    load_hearing();
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

void CallController::setPreview(VideoView* view) {
    if (attach(preview_, *preview_pictures_, view)) Q_EMIT previewChanged();
}

void CallController::setCameraView(const QString& id, VideoView* view) {
    QPointer<VideoView>& slot = camera_views_[id];
    attach(slot, *cameras_->of(id), view);
    if (!view) camera_views_.remove(id);
}

// ── In and out of a room ────────────────────────────────────────────────────

void CallController::setServer(const QString& server) {
    if (server == server_) return;
    server_ = server;
    Q_EMIT serverChanged();
}

bool CallController::meet_at_server(CallConfig& config) {
    const std::string server = server_.trimmed().toStdString();
    if (server.empty()) return true;
    const std::optional<librats::HostEndpoint> parsed = parse_server(server);
    if (!parsed) {
        set_notice(tr("'%1' is no server: HOST or HOST:PORT, please.").arg(server_.trimmed()));
        return false;
    }
    config.servers = {*parsed};
    return true;
}

void CallController::create() {
    if (state_ != State::Idle) return;
    CallConfig config = environment_.call;
    config.data_dir = environment_.data_dir;
    if (!meet_at_server(config)) return;
    start(std::move(config), State::InRoom);
}

void CallController::join(const QString& link) {
    if (state_ != State::Idle) return;
    const std::optional<RatsUrl> url = parse_rats_url(link.trimmed().toStdString());
    if (!url) {
        set_notice(tr("That is not a rats:// link."));
        return;
    }
    CallConfig config = environment_.call;
    // An identity of its own, so that one host can be in a room twice.
    if (!environment_.data_dir.empty()) {
        config.data_dir = (std::filesystem::path(environment_.data_dir) / "guest").string();
    }
    config.discoverable = false;
    config.dial = *url;
    if (!meet_at_server(config)) return;
    start(std::move(config), State::Joining);
}

void CallController::start(CallConfig config, State state) {
    set_notice({});
    const uint64_t generation = ++generation_;
    voice_ = std::make_unique<VoiceChannel>(environment_.voice);
    std::unique_ptr<AudioOutput> device =
        environment_.open_speaker ? environment_.open_speaker() : nullptr;
    speaker_ = device ? voice_->speaker(std::move(device)) : nullptr;
    apply_voice();

    config.mesh = true;  // whoever joins meets everyone
    config.playback.pictures = [pictures = pictures_](VideoFrame picture) {
        (*pictures)(std::move(picture));
    };
    config.playback.cameras = [cameras = cameras_](const PeerId& peer) -> VideoPlayer::Sink {
        return [pictures = cameras->of(id_of(peer))](VideoFrame picture) {
            (*pictures)(std::move(picture));
        };
    };
    config.playback.sound = speaker_.get();
    Result<std::unique_ptr<Call>> started = Call::start(std::move(config), handlers(generation));
    if (!started) {
        end(tr("The call cannot start (%1).").arg(describe(started.error())));
        return;
    }
    call_ = std::move(*started);
    for (auto it = hearing_.cbegin(); it != hearing_.cend(); ++it)
        tell_hearing(it.key(), it.value());
    for (const std::string& invite : call_->invites()) {
        invites_.push_back(QString::fromStdString(invite));
    }
    Q_EMIT invitesChanged();
    set_state(state);
    tell_presence();
    start_monitoring();
    talk();
}

void CallController::leave() { end(); }

void CallController::end(QString notice) {
    ++generation_;
    dialog_.request_stop();
    if (dialog_.joinable()) dialog_.join();
    levels_.stop();
    stop_monitoring();
    call_.reset();
    speaker_.reset();
    voice_.reset();
    cameras_->clear();
    for (const QPointer<VideoView>& view : std::as_const(camera_views_)) {
        if (view) view->clear();
    }
    if (video_) video_->clear();
    if (preview_) preview_->clear();
    update_levels();
    update_members();
    set_choosing(false);
    set_sharing(false);
    set_watching({});
    if (!invites_.isEmpty()) {
        invites_.clear();
        Q_EMIT invitesChanged();
    }
    set_notice(std::move(notice));
    set_state(State::Idle);
}

// ── Talking ─────────────────────────────────────────────────────────────────

void CallController::talk() {
    levels_.start();
    if (camera_) start_camera();
    std::unique_ptr<AudioSource> device =
        environment_.open_microphone ? environment_.open_microphone() : nullptr;
    // Not heard is no reason to leave: said, and the room goes on.
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

void CallController::setMuted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    Q_EMIT mutedChanged();
    apply_voice();
}

void CallController::setDeafened(bool deafened) {
    if (deafened == deafened_) return;
    deafened_ = deafened;
    Q_EMIT deafenedChanged();
    apply_voice();
}

void CallController::apply_voice() {
    if (voice_) {
        voice_->set_muted(muted_ || deafened_);
        voice_->set_deafened(deafened_);
    }
    update_levels();
    tell_presence();
}

void CallController::setName(const QString& name) {
    if (name == name_) return;
    name_ = name;
    Q_EMIT nameChanged();
    tell_presence();
}

void CallController::tell_presence() {
    if (!call_) return;
    // Valid by construction: presence_name() keeps only what may be told.
    (void)call_->set_presence(
        {.name = presence_name(name_), .muted = muted_ || deafened_, .deafened = deafened_});
}

void CallController::update_levels() {
    const bool speaking = voice_ && voice_->speaking();
    if (speaking == speaking_) return;
    speaking_ = speaking;
    Q_EMIT speakingChanged();
}

void CallController::update_members() {
    std::vector<Member> members;
    if (call_) {
        const std::vector<PeerId> speaking = call_->speaking();
        const std::vector<OfferedStream> offered = call_->session().offered();
        const auto offers = [&](const PeerId& peer, bool camera) {
            return std::ranges::any_of(offered, [&](const OfferedStream& stream) {
                return stream.peer == peer && stream.description.kind() == MediaKind::Video &&
                       (stream.description.label == kCameraLabel) == camera;
            });
        };
        std::vector<PeerId> peers = call_->peers();
        std::sort(peers.begin(), peers.end());
        for (const PeerId& peer : peers) {
            const Presence presence = call_->presence(peer).value_or(Presence{});
            const Hearing hearing = hearing_.value(id_of(peer));
            members.push_back({
                .id = id_of(peer),
                .peer = QString::fromStdString(peer.short_hex()),
                .name = QString::fromStdString(presence.name),
                .muted = presence.muted,
                .deafened = presence.deafened,
                .speaking = std::ranges::find(speaking, peer) != speaking.end(),
                .camera = offers(peer, true),
                .sharing = offers(peer, false),
                .volume = hearing.volume,
                .muted_here = hearing.muted,
            });
        }
    }
    const int cameras = int(std::ranges::count_if(members, &Member::camera));
    members_.set(std::move(members));
    update_watched();
    if (cameras == cameras_on_) return;
    cameras_on_ = cameras;
    Q_EMIT camerasChanged();
}

// ── Sharing and watching ────────────────────────────────────────────────────

void CallController::share(bool window, bool sound) {
    if (state_ != State::InRoom || sharing_ || choosing_) return;
    set_notice({});
    ScreenShareConfig config;
    config.window = window;
    config.sound = sound;
    // No data_dir: the desktop asks every time, as a room's member chooses
    // anew what to show. A choice kept would stand for the dialog for good.
    set_choosing(true);
    // The desktop's dialog waits on the user: off the GUI thread. The last
    // one has said its say: assigning joins it.
    dialog_ = std::jthread([this, generation = generation_, config,
                            open = environment_.open_share](std::stop_token cancel) {
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
    set_choosing(false);
    if (!sources) {
        if (sources.error() != Errc::Cancelled) {
            set_notice(tr("The screen cannot be shared (%1).").arg(describe(sources.error())));
        }
        return;
    }
    if (Result<void> shared = publish_screen_share(*call_, std::move(*sources), config); !shared) {
        set_notice(tr("The screen cannot be shared (%1).").arg(describe(shared.error())));
        return;
    }
    set_sharing(true);
}

void CallController::stopSharing() {
    if (choosing_) dialog_.request_stop();  // its answer, Cancelled, ends it
    if (!sharing_) return;
    unpublish_screen_share(*call_);
    set_sharing(false);
}

void CallController::watch(const QString& id) {
    if (!call_) return;
    if (id.isEmpty()) {
        call_->watch(std::nullopt);
        return;
    }
    if (const std::optional<PeerId> peer = PeerId::from_hex(id.toStdString())) call_->watch(*peer);
}

// ── How each member is heard ────────────────────────────────────────────────
//
// Kept in the settings as hearing/<id>/volume and hearing/<id>/muted, for
// the members not heard as they come.

void CallController::setVolume(const QString& id, double volume) {
    Hearing hearing = hearing_.value(id);
    hearing.volume = volume > 0.0 ? std::min(volume, kMaxVolume) : 0.0;  // NaN: 0
    hear(id, hearing);
}

void CallController::setMutedHere(const QString& id, bool muted) {
    Hearing hearing = hearing_.value(id);
    hearing.muted = muted;
    hear(id, hearing);
}

void CallController::load_hearing() {
    hearing_.clear();
    if (!environment_.settings) return;
    const std::unique_ptr<QSettings> settings = environment_.settings();
    settings->beginGroup(QStringLiteral("hearing"));
    for (const QString& id : settings->childGroups()) {
        if (!PeerId::from_hex(id.toStdString())) continue;
        const double volume = settings->value(id + QStringLiteral("/volume"), 1.0).toDouble();
        hearing_[id] = {.volume = volume > 0.0 ? std::min(volume, kMaxVolume) : 0.0,
                        .muted = settings->value(id + QStringLiteral("/muted")).toBool()};
    }
}

void CallController::hear(const QString& id, Hearing hearing) {
    const std::optional<PeerId> peer = PeerId::from_hex(id.toStdString());
    if (!peer || hearing == hearing_.value(id)) return;
    const bool as_they_come = hearing == Hearing{};
    if (as_they_come) {
        hearing_.remove(id);
    } else {
        hearing_[id] = hearing;
    }
    if (environment_.settings) {
        const std::unique_ptr<QSettings> settings = environment_.settings();
        const QString group = QStringLiteral("hearing/") + id;
        if (as_they_come) {
            settings->remove(group);
        } else {
            settings->setValue(group + QStringLiteral("/volume"), hearing.volume);
            settings->setValue(group + QStringLiteral("/muted"), hearing.muted);
        }
    }
    tell_hearing(id, hearing);
    update_members();
}

void CallController::tell_hearing(const QString& id, const Hearing& hearing) {
    const std::optional<PeerId> peer = PeerId::from_hex(id.toStdString());
    if (call_ && peer) call_->set_volume(*peer, hearing.muted ? 0.0f : float(hearing.volume));
}

// ── The camera ──────────────────────────────────────────────────────────────

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

// ── How it goes ─────────────────────────────────────────────────────────────

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

void CallController::copy(const QString& text) { QGuiApplication::clipboard()->setText(text); }

// ── The call's events ───────────────────────────────────────────────────────

CallHandlers CallController::handlers(uint64_t generation) {
    return CallHandlers{
        .joined =
            [this, generation](const PeerId&) {
                post(generation, [this] {
                    if (state_ == State::Joining) set_state(State::InRoom);
                    update_members();
                });
            },
        .left = [this,
                 generation](const PeerId&) { post(generation, [this] { update_members(); }); },
        .playing =
            [this, generation](const PeerId& peer, const StreamDescription& stream,
                               Result<void> outcome) {
                if (stream.kind() != MediaKind::Video && outcome) return;
                const QString label = QString::fromStdString(stream.label);
                const bool share = stream.label != kCameraLabel;
                post(generation, [this, id = id_of(peer), outcome, label, share] {
                    update_members();
                    if (!outcome) {
                        set_notice(tr("'%1' cannot be played (%2).")
                                       .arg(label, describe(outcome.error())));
                        return;
                    }
                    if (share) set_watching(id);
                });
            },
        .ended =
            [this, generation](const PeerId& peer, const StreamDescription& stream) {
                if (stream.kind() != MediaKind::Video) return;
                const bool share = stream.label != kCameraLabel;
                post(generation, [this, id = id_of(peer), share] {
                    update_members();
                    if (!share) {
                        if (const QPointer<VideoView> view = camera_views_.value(id)) view->clear();
                        return;
                    }
                    if (id != watching_) return;
                    if (video_) video_->clear();
                    set_watching({});
                });
            },
        .presence = [this, generation](
                        const PeerId&,
                        const Presence&) { post(generation, [this] { update_members(); }); },
        .source_ended =
            [this, generation](StreamId stream, Result<void> outcome) {
                post(generation, [this, stream, outcome] {
                    if (stream == kCameraStream) {
                        camera_off(
                            outcome ? tr("The camera went.")
                                    : tr("The camera failed (%1).").arg(describe(outcome.error())));
                        return;
                    }
                    if (stream != kScreenStream && stream != kScreenSoundStream) return;
                    if (!sharing_) return;
                    // Either half gone is the share over: the desktop stopped it.
                    unpublish_screen_share(*call_);
                    set_sharing(false);
                    set_notice(outcome ? tr("Sharing stopped.")
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

// ── State ───────────────────────────────────────────────────────────────────

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

void CallController::set_sharing(bool sharing) {
    if (sharing == sharing_) return;
    sharing_ = sharing;
    Q_EMIT sharingChanged();
}

void CallController::set_choosing(bool choosing) {
    if (choosing == choosing_) return;
    choosing_ = choosing;
    Q_EMIT choosingChanged();
}

void CallController::set_watching(QString id) {
    if (id == watching_) return;
    watching_ = std::move(id);
    // Nobody touches anything while a film plays: the screen kept on.
    if (watching_.isEmpty()) {
        awake_.reset();
    } else if (!awake_ && environment_.keep_screen_awake) {
        awake_ = environment_.keep_screen_awake();
    }
    Q_EMIT watchingChanged();
    update_watched();
}

void CallController::update_watched() {
    QVariantMap watched;
    const std::vector<Member>& rows = members_.members();
    const auto it = std::ranges::find(rows, watching_, &Member::id);
    if (!watching_.isEmpty() && it != rows.end()) watched = members_.get(int(it - rows.begin()));
    if (watched == watched_) return;
    watched_ = std::move(watched);
    Q_EMIT watchedChanged();
}

}  // namespace ratcast::app
