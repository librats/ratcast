#include "call_controller.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QStandardPaths>
#include <QVariantMap>
#ifdef Q_OS_ANDROID
#include <QPermissions>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast::app {
namespace {

QString describe(Errc error) { return QString::fromLatin1(to_string(error)); }

QString id_of(const PeerId& peer) { return QString::fromStdString(peer.to_hex()); }

/// A device chosen as the log names it: by its name, or the system's default.
std::string device_named(const QString& id, const QString& name) {
    if (id.isEmpty()) return "the default";
    return '"' + (name.isEmpty() ? id : name).toStdString() + '"';
}

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

/// `text` as a message may carry it: line breaks as "\n", other control
/// characters dropped, the ends trimmed.
std::string chat_text(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    QString kept;
    for (const QChar c : std::as_const(text)) {
        if (c == u'\n' || c == u'\t' || c.category() != QChar::Other_Control) kept += c;
    }
    return kept.trimmed().toStdString();
}

/// The login name of the user running this.
QString user_name() {
    QString name = qEnvironmentVariable("USER");
    if (name.isEmpty()) name = qEnvironmentVariable("USERNAME");  // Windows
    return name;
}

/// What was listed; nothing if it could not be.
template <typename Info>
std::vector<Info> or_none(Result<std::vector<Info>> listed) {
    return listed ? std::move(*listed) : std::vector<Info>{};
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
#ifdef Q_OS_ANDROID
    // The application's own storage: the shared one (Generic) asks the user.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#else
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/ratcast");
#endif
}

CallController::Environment CallController::Environment::system() {
    Environment environment;
    environment.name = user_name();
    environment.server = QStringLiteral(RATCAST_DEFAULT_SERVER);
    environment.call.discoverable = true;
    environment.call.playback.most_cameras = kMostCameras;
    environment.call.lost_after = kLostAfter;
    environment.call.chosen_servers = kChosenServers;
    environment.data_dir = data_folder().toStdString();
    // One list for the room made here and the one joined as a guest.
    environment.call.known_servers_file =
        (std::filesystem::path(environment.data_dir) / KnownServers::kFile).string();
    environment.open_share = [](const ScreenShareConfig& config, std::stop_token cancel) {
        return open_screen_share(config, std::move(cancel));
    };
    environment.open_speaker = [format = environment.voice.speaker](const std::string& device) {
        Result<std::unique_ptr<AudioOutput>> speaker =
            open_audio_output({.format = format, .device = device, .call = true});
        return speaker ? std::move(*speaker) : nullptr;
    };
    environment.open_microphone = [format =
                                       environment.voice.microphone](const std::string& device) {
        Result<std::unique_ptr<AudioSource>> microphone =
            open_audio_capture({.format = format, .device = device, .call = true});
        return microphone ? std::move(*microphone) : nullptr;
    };
    environment.open_camera = [](const std::string& device) -> std::unique_ptr<VideoSource> {
        CameraOptions options = call_camera_options();
        options.id = device;
        Result<std::unique_ptr<VideoSource>> camera = ratcast::open_camera(options);
        return camera ? std::move(*camera) : nullptr;
    };
    environment.open_system_sound = [format =
                                         environment.voice.speaker](const std::string& device) {
        Result<std::unique_ptr<AudioSource>> sound = open_audio_capture(
            {.format = format, .source = AudioCaptureSource::SystemSound, .device = device});
        return sound ? std::move(*sound) : nullptr;
    };
    // None listed is no reason to say so: the defaults are there all the same.
    environment.list_microphones = [] { return or_none(list_audio_inputs()); };
    environment.list_speakers = [] { return or_none(list_audio_outputs()); };
    environment.list_cameras = [] { return or_none(ratcast::list_cameras()); };
    environment.keep_screen_awake = [] { return ratcast::keep_screen_awake("Watching a share"); };
    environment.enter_call = [] { return ratcast::enter_call(); };
#ifdef Q_OS_ANDROID
    // Asked of the user here; desktops ask as the microphone opens, or not.
    environment.ask_microphone = [](std::function<void(bool)> allowed) {
        const QMicrophonePermission microphone;
        if (qApp->checkPermission(microphone) == Qt::PermissionStatus::Granted) {
            allowed(true);
            return;
        }
        qApp->requestPermission(microphone, qApp,
                                [allowed = std::move(allowed)](const QPermission& answer) {
                                    allowed(answer.status() == Qt::PermissionStatus::Granted);
                                });
    };
    environment.ask_camera = [](std::function<void(bool)> allowed) {
        const QCameraPermission camera;
        if (qApp->checkPermission(camera) == Qt::PermissionStatus::Granted) {
            allowed(true);
            return;
        }
        qApp->requestPermission(camera, qApp,
                                [allowed = std::move(allowed)](const QPermission& answer) {
                                    allowed(answer.status() == Qt::PermissionStatus::Granted);
                                });
    };
#endif
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
    logging_.setInterval(std::chrono::milliseconds(kLogStatsEvery));
    connect(&logging_, &QTimer::timeout, this, &CallController::log_stats);
    load_hearing();
    load_devices();
}

CallController::~CallController() {
    ++generation_;
    dialog_.request_stop();
    if (dialog_.joinable()) dialog_.join();
    monitor_.reset();
    printer_.reset();
    logger_.reset();
    call_.reset();
    speaker_.reset();
    voice_.reset();
}

void CallController::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
    setName(environment_.name);
    setServer(environment_.server);
    load_hearing();
    load_devices();
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

void CallController::setSharePreview(VideoView* view) {
    if (view == share_preview_view_) return;
    share_preview_view_ = view;
    // None: nothing taken or made small for it.
    if (share_preview_) share_preview_->set_sink(view ? view->sink() : VideoView::Sink{});
    Q_EMIT sharePreviewChanged();
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
    if (std::ranges::find(config.servers, *parsed) == config.servers.end())
        config.servers.push_back(*parsed);
    return true;
}

void CallController::create() {
    if (state_ != State::Idle) return;
    CallConfig config = environment_.call;
    config.data_dir = environment_.data_dir;
    if (!meet_at_server(config)) return;
    LOG_INFO("app", "making a room");
    start(std::move(config), State::InRoom);
}

void CallController::join(const QString& link) {
    if (state_ != State::Idle) return;
    const std::optional<RatsLink> url = parse_rats_link(link.trimmed().toStdString());
    if (!url) {
        set_notice(tr("That is not a rats:// link."));
        return;
    }
    LOG_INFO("app", "joining " << to_string(*url));
    CallConfig config = environment_.call;
    // An identity of its own, so that one host can be in a room twice.
    if (!environment_.data_dir.empty()) {
        config.data_dir = (std::filesystem::path(environment_.data_dir) / "guest").string();
    }
    config.discoverable = false;
    config.dial = url->url;
    // The room's servers first: they are what its invites name on.
    config.servers = url->via;
    if (!meet_at_server(config)) return;
    start(std::move(config), State::Joining);
}

void CallController::start(CallConfig config, State state) {
    set_notice({});
    chat_.clear();
    const uint64_t generation = ++generation_;
    // Told first: the call's sound goes where the call is routed.
    in_call_ = environment_.enter_call ? environment_.enter_call() : nullptr;
    heard_ = false;
    voice_ = std::make_unique<VoiceChannel>(environment_.voice);
    std::unique_ptr<AudioOutput> device =
        environment_.open_speaker ? environment_.open_speaker(chosen_id(Speaker)) : nullptr;
    speaker_ =
        device ? std::make_unique<SwitchableOutput>(voice_->speaker(std::move(device))) : nullptr;
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
    const auto named = [this](Device device) {
        return device_named(chosen_[device].id, chosen_[device].name);
    };
    LOG_INFO("app", "microphone " << named(Microphone) << ", speaker " << named(Speaker)
                                  << ", camera " << named(Camera));
    for (auto it = hearing_.cbegin(); it != hearing_.cend(); ++it)
        tell_hearing(it.key(), it.value());
    call_->set_share_volume(float(share_volume_));
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
    if (call_) LOG_INFO("app", "leaving the room");
    ++generation_;
    dialog_.request_stop();
    if (dialog_.joinable()) dialog_.join();
    levels_.stop();
    stop_monitoring();
    call_.reset();
    speaker_.reset();
    voice_.reset();
    in_call_.reset();
    heard_ = false;
    cameras_->clear();
    for (const QPointer<VideoView>& view : std::as_const(camera_views_)) {
        if (view) view->clear();
    }
    if (video_) video_->clear();
    if (preview_) preview_->clear();
    if (share_preview_view_) share_preview_view_->clear();
    share_preview_.reset();
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
    const auto heard = [this] {
        heard_ = true;
        start_microphone();
    };
    if (!environment_.ask_microphone) {
        if (camera_) start_camera();
        heard();
        return;
    }
    environment_.ask_microphone([self = QPointer(this), generation = generation_,
                                 heard](bool allowed) {
        if (!self || self->generation_ != generation) return;
        // The camera's leave asked for after: Android asks one at a time.
        if (self->camera_) self->start_camera();
        if (!allowed) {
            self->set_notice(tr("The microphone is not allowed: the others will not hear you."));
            return;
        }
        // Heard with the app in the background as well, from now on.
        if (self->in_call_) self->in_call_->update();
        heard();
    });
}

void CallController::start_microphone() {
    if (!heard_) return;
    std::unique_ptr<AudioSource> device = environment_.open_microphone
                                              ? environment_.open_microphone(chosen_id(Microphone))
                                              : nullptr;
    // Not heard is no reason to leave: said, and the room goes on.
    if (!device) {
        set_notice(tr("There is no microphone: the others will not hear you."));
        return;
    }
    std::unique_ptr<AudioSource> echo = environment_.open_system_sound
                                            ? environment_.open_system_sound(chosen_id(Speaker))
                                            : nullptr;
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

void CallController::setShareVolume(double volume) {
    volume = volume > 0.0 ? std::min(volume, kMaxVolume) : 0.0;  // NaN: 0
    if (volume == share_volume_) return;
    share_volume_ = volume;
    Q_EMIT shareVolumeChanged();
    if (call_) call_->set_share_volume(float(volume));
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
        const std::vector<PeerId> shown = call_->cameras();
        const std::vector<OfferedStream> offered = call_->session().offered();
        const auto offers = [&](const PeerId& peer, bool camera) {
            return std::ranges::any_of(offered, [&](const OfferedStream& stream) {
                return stream.peer == peer && stream.description.kind() == MediaKind::Video &&
                       (stream.description.label == kCameraLabel) == camera;
            });
        };
        // The members lost are listed too, while they are dialed again.
        std::vector<PeerId> peers = call_->peers();
        std::ranges::copy(call_->away(), std::back_inserter(peers));
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
                .camera_shown = std::ranges::find(shown, peer) != shown.end(),
                .sharing = offers(peer, false),
                .reconnecting = call_->interrupted(peer),
                .volume = hearing.volume,
                .muted_here = hearing.muted,
            });
        }
    }
    const int cameras = int(std::ranges::count_if(members, &Member::camera_shown));
    members_.set(std::move(members));
    update_watched();
    if (cameras == cameras_on_) return;
    cameras_on_ = cameras;
    Q_EMIT camerasChanged();
}

// ── Sharing and watching ────────────────────────────────────────────────────

void CallController::share(bool window, bool sound) {
    if (state_ != State::InRoom || choosing_) return;
    set_notice({});
    // Sharing, another in place of what is shared: the sound shared goes on,
    // none opened beside it.
    const bool switching = sharing_;
    ScreenShareConfig config;
    config.window = window;
    config.sound = sound && !switching;
#ifdef Q_OS_ANDROID
    // What the applications play is heard only with the microphone's leave:
    // without it, the screen alone.
    if (qApp->checkPermission(QMicrophonePermission{}) != Qt::PermissionStatus::Granted)
        config.sound = false;
#endif
    // No data_dir: the desktop asks every time, as a room's member chooses
    // anew what to show. A choice kept would stand for the dialog for good.
    set_choosing(true);
    // The desktop's dialog waits on the user: off the GUI thread. The last
    // one has said its say: assigning joins it.
    dialog_ = std::jthread([this, generation = generation_, config, switching,
                            open = environment_.open_share](std::stop_token cancel) {
        auto sources = std::make_shared<Result<ScreenShareSources>>(open(config, cancel));
        QMetaObject::invokeMethod(
            this,
            [this, generation, config, switching, sources] {
                share_opened(generation, config, switching, std::move(*sources));
            },
            Qt::QueuedConnection);
    });
}

void CallController::share_opened(uint64_t generation, const ScreenShareConfig& config,
                                  bool switching, Result<ScreenShareSources> sources) {
    if (generation != generation_) return;  // left meanwhile
    set_choosing(false);
    if (!sources) {
        if (sources.error() != Errc::Cancelled) {
            set_notice(tr("The screen cannot be shared (%1).").arg(describe(sources.error())));
        }
        return;
    }
    // A switch whose share ended meanwhile (stopped, or its window closed)
    // has nothing to switch: the user shares again if they want to.
    if (switching && !sharing_) return;
    sources->pictures = previewed(std::move(sources->pictures));
    const Result<void> shared = switching
                                    ? switch_screen_share(*call_, std::move(*sources))
                                    : publish_screen_share(*call_, std::move(*sources), config);
    if (!shared) {
        set_notice(tr("The screen cannot be shared (%1).").arg(describe(shared.error())));
        if (!sharing_) share_preview_.reset();
        return;
    }
    set_sharing(true);
}

std::unique_ptr<VideoSource> CallController::previewed(std::unique_ptr<VideoSource> pictures) {
    if (!share_preview_) {
        share_preview_ = std::make_shared<Preview>();
        if (share_preview_view_) share_preview_->set_sink(share_preview_view_->sink());
    }
    return with_preview(std::move(pictures), [preview = share_preview_](const VideoFrame& picture) {
        preview->take(picture);
    });
}

void CallController::stop_share() {
    unpublish_screen_share(*call_);
    share_preview_.reset();
    if (share_preview_view_) share_preview_view_->clear();
    set_sharing(false);
}

void CallController::stopSharing() {
    if (choosing_) {
        dialog_.request_stop();  // its answer, Cancelled, ends it
        return;
    }
    if (sharing_) stop_share();
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

// ── The devices ─────────────────────────────────────────────────────────────
//
// Kept in the settings as devices/<device>/id and devices/<device>/name, for
// those not the default.

namespace {

/// Where the settings keep the choice of each device.
QString device_group(int device) {
    static constexpr std::array kGroups{"devices/microphone", "devices/speaker", "devices/camera"};
    return QString::fromLatin1(kGroups[size_t(device)]);
}

/// `devices` as QML lists them.
template <typename Info>
QVariantList to_variant(const std::vector<Info>& devices) {
    QVariantList list;
    for (const Info& device : devices) {
        list.push_back(QVariantMap{{QStringLiteral("id"), QString::fromStdString(device.id)},
                                   {QStringLiteral("name"), QString::fromStdString(device.name)}});
    }
    return list;
}

}  // namespace

std::string CallController::chosen_id(Device device) const {
    return chosen_[device].id.toStdString();
}

void CallController::listDevices() {
    const auto listed = [](const auto& list) { return list ? to_variant(list()) : QVariantList{}; };
    listed_ = {listed(environment_.list_microphones), listed(environment_.list_speakers),
               listed(environment_.list_cameras)};
    // The one chosen, while it is not there: shown all the same, as it was.
    for (int device = 0; device < kDevices; ++device) {
        const Chosen& chosen = chosen_[device];
        QVariantList& list = listed_[device];
        if (chosen.id.isEmpty() || std::ranges::any_of(list, [&](const QVariant& listed) {
                return listed.toMap().value(QStringLiteral("id")) == chosen.id;
            })) {
            continue;
        }
        list.push_back(QVariantMap{{QStringLiteral("id"), chosen.id},
                                   {QStringLiteral("name"), chosen.name},
                                   {QStringLiteral("missing"), true}});
    }
    Q_EMIT devicesChanged();
}

void CallController::load_devices() {
    chosen_ = {};
    if (environment_.settings) {
        const std::unique_ptr<QSettings> settings = environment_.settings();
        for (int device = 0; device < kDevices; ++device) {
            const QString group = device_group(device);
            chosen_[device] = {settings->value(group + QStringLiteral("/id")).toString(),
                               settings->value(group + QStringLiteral("/name")).toString()};
        }
    }
    Q_EMIT microphoneDeviceChanged();
    Q_EMIT speakerDeviceChanged();
    Q_EMIT cameraDeviceChanged();
}

void CallController::choose(Device device, const QString& id) {
    if (id == chosen_[device].id) return;
    QString name;
    for (const QVariant& listed : std::as_const(listed_[device])) {
        const QVariantMap map = listed.toMap();
        if (map.value(QStringLiteral("id")) == id)
            name = map.value(QStringLiteral("name")).toString();
    }
    chosen_[device] = {id, name.isEmpty() ? id : name};
    LOG_INFO("app", device_group(device).toStdString()
                        << " chosen: " << device_named(id, chosen_[device].name));
    if (environment_.settings) {
        const std::unique_ptr<QSettings> settings = environment_.settings();
        const QString group = device_group(device);
        if (id.isEmpty()) {
            settings->remove(group);
        } else {
            settings->setValue(group + QStringLiteral("/id"), id);
            settings->setValue(group + QStringLiteral("/name"), chosen_[device].name);
        }
    }
    switch (device) {
        case Microphone:
            Q_EMIT microphoneDeviceChanged();
            // Sent anew from the one chosen; the others hear a moment's gap.
            if (call_) {
                call_->unpublish(kVoiceStream);
                start_microphone();
            }
            break;
        case Speaker:
            Q_EMIT speakerDeviceChanged();
            if (call_) switch_speaker();
            break;
        case Camera:
            Q_EMIT cameraDeviceChanged();
            if (call_ && camera_) {
                call_->unpublish(kCameraStream);
                start_camera();
            }
            break;
        case kDevices: break;
    }
}

void CallController::switch_speaker() {
    std::unique_ptr<AudioOutput> device =
        environment_.open_speaker ? environment_.open_speaker(chosen_id(Speaker)) : nullptr;
    if (!device || !speaker_) {
        set_notice(tr("That speaker cannot be used."));
        return;
    }
    if (Result<void> switched = speaker_->switch_to(voice_->speaker(std::move(device)));
        !switched) {
        set_notice(tr("That speaker cannot be used (%1).").arg(describe(switched.error())));
        return;
    }
    // The echo the microphone is cleaned of is what this one plays now.
    if (environment_.open_system_sound) {
        call_->unpublish(kVoiceStream);
        start_microphone();
    }
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
    if (!environment_.ask_camera) {
        open_camera();
        return;
    }
    environment_.ask_camera([self = QPointer(this), turn = ++camera_turn_](bool allowed) {
        // Turned off, or on again, or the room left, since: not this answer's.
        if (!self || self->camera_turn_ != turn || !self->camera_ || !self->call_) return;
        if (!allowed) {
            self->camera_off(tr("The camera is not allowed."));
            return;
        }
        // Seen with the app in the background as well, from now on.
        if (self->in_call_) self->in_call_->update();
        self->open_camera();
    });
}

void CallController::open_camera() {
    std::unique_ptr<VideoSource> device =
        environment_.open_camera ? environment_.open_camera(chosen_id(Camera)) : nullptr;
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
    ++camera_turn_;
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
    logger_ = std::make_unique<CallMonitor>(*call_);
    logging_.start();
    if (stats_every_ > 0) {
        printer_ = std::make_unique<CallMonitor>(*call_);
        printing_.start(stats_every_ * 1000);
    }
}

void CallController::stop_monitoring() {
    reporting_.stop();
    printing_.stop();
    logging_.stop();
    monitor_.reset();
    printer_.reset();
    logger_.reset();
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

void CallController::log_stats() {
    if (!logger_) return;
    for (const std::string& line : report_lines(logger_->report())) LOG_INFO("stats", line);
}

// ── What is said ────────────────────────────────────────────────────────────

bool CallController::say(const QString& text) {
    if (!call_) return false;
    std::string words = chat_text(text);
    if (words.empty()) return false;
    if (words.size() > ChatMessage::kMaxTextSize) {
        set_notice(tr("That is too long to send: say it in a few messages."));
        return false;
    }
    const Result<ChatMessage> said = call_->say(std::move(words));
    if (!said) {
        set_notice(tr("That cannot be sent (%1).").arg(describe(said.error())));
        return false;
    }
    chat_.add(*said, /*mine=*/true);
    return true;
}

void CallController::copy(const QString& text) { QGuiApplication::clipboard()->setText(text); }

// ── The call's events ───────────────────────────────────────────────────────

CallHandlers CallController::handlers(uint64_t generation) {
    return CallHandlers{
        .joined =
            [this, generation](const PeerId&) {
                post(generation, [this] {
                    if (state_ == State::Joining) {
                        LOG_INFO("app", "in the room");
                        set_state(State::InRoom);
                    }
                    update_members();
                });
            },
        .left = [this,
                 generation](const PeerId&) { post(generation, [this] { update_members(); }); },
        .lost = [this,
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
        .said =
            [this, generation](const ChatMessage& message) {
                // This side's own too, said before it came back.
                post(generation,
                     [this, message] { chat_.add(message, message.author == call_->id()); });
            },
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
                    stop_share();
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
    // What the user is told went wrong, as they were told it.
    if (!notice.isEmpty()) LOG_WARN("app", notice.toStdString());
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
