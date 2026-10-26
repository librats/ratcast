#pragma once

/**
 * @file call_controller.h
 * @brief A room as QML sees it: make one or join one, talk, share, watch.
 *
 *   Idle ── create() ───────────────▶ InRoom     (alone until others join)
 *     └──── join() ──▶ Joining ─────▶ InRoom     (once a member answers)
 *
 * and leave() goes back to Idle from anywhere. A room is a call where
 * everyone meets everyone, whoever they joined through (CallConfig::mesh).
 * Everyone talks from the start: the microphone, cleaned of the echo of all
 * the computer plays and of noise, goes as a voice (VoiceChannel). Muted, it
 * sends silence; deafened, nobody is heard and the microphone is silent too,
 * as in Discord. Anyone may share a screen or a window — the desktop's dialog,
 * off the GUI thread — and stop, and turn their camera on and off, while the
 * room goes on. Whose share plays is chosen with watch(); until anyone is
 * chosen, the first one offered.
 *
 * Each side says who it is — the user's name, muted, deafened — and the others
 * are listed (`members`) by it, with who is speaking and whose camera and
 * share are on. Each member's camera goes to the view QML gives for it
 * (setCameraView()), the share watched to `video`, this side's own camera to
 * `preview`.
 *
 * What went wrong, or ended without the user, is the notice: a share the
 * desktop could not make or that stopped, a link that is none, a camera
 * gone. The user calling off the desktop's dialog is no news.
 *
 * In a room, a CallMonitor reports on it every second: what each stream sent
 * and played does and how well, laid out for a panel, and the call's quality
 * with the reason; and, if asked, on standard output every so many seconds.
 *
 * Everything that is not about the UI — the sources, what is published and
 * how, playing, judging how it goes — is client's (Call, screen_share.h,
 * call_monitor.h); this keeps the state, runs the dialog off the GUI thread
 * and brings the call's events onto it. GUI thread only.
 */

#include <QtQml/qqmlregistration.h>
#include <QAbstractItemModel>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

#include "members_model.h"
#include "ratcast/client/call.h"
#include "ratcast/client/call_monitor.h"
#include "ratcast/client/camera.h"
#include "ratcast/client/screen_share.h"
#include "ratcast/client/voice.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/screen_awake.h"
#include "video_view.h"

namespace ratcast::app {

/// Where the app keeps what lasts from one run to the next — its identities,
/// the DHT's contacts — and its log: "ratcast" in the user's local application
/// data (%LOCALAPPDATA% on Windows, ~/.local/share on Linux).
QString data_folder();

class CallController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    /// rats:// links to the room, for the user to send: by peer id for the
    /// internet if there is one, then the networks this host is on. Any
    /// member's will do.
    Q_PROPERTY(QStringList invites READ invites NOTIFY invitesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    /// What the others call this side: the user's login name until they say
    /// otherwise. Whitespace runs and control characters are not told, nor
    /// what is past Presence::kMaxNameSize bytes.
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    /// The server rooms are met at — HOST or HOST:PORT of a ratcast-server,
    /// through NATs no straight path gets through — "" for none: the
    /// environment's until the user says otherwise. Taken as a room is made
    /// or joined.
    Q_PROPERTY(QString server READ server WRITE setServer NOTIFY serverChanged)
    /// The others in the room (MembersModel): peerId, peer (a short one),
    /// name, muted, deafened, speaking, camera, sharing.
    Q_PROPERTY(QAbstractItemModel* members READ members CONSTANT)
    /// The loudest a member can be heard (setVolume()).
    Q_PROPERTY(double maxVolume READ maxVolume CONSTANT)
    /// How many of the others have their camera on.
    Q_PROPERTY(int cameras READ cameras NOTIFY camerasChanged)
    /// The microphone silenced, as the user says; kept from one room to the
    /// next. Deafened silences it as well, whatever this says.
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    /// Nobody heard: the speaker plays silence.
    Q_PROPERTY(bool deafened READ deafened WRITE setDeafened NOTIFY deafenedChanged)
    /// How loud the sound of the share watched plays, beside the voices: from
    /// 0 through 1, as it comes, to maxVolume; whoever's it is.
    Q_PROPERTY(double shareVolume READ shareVolume WRITE setShareVolume NOTIFY shareVolumeChanged)
    /// This side's voice on the microphone.
    Q_PROPERTY(bool speaking READ speaking NOTIFY speakingChanged)
    /// This side's camera on: sent while in a room, and with the next one;
    /// kept from one room to the next. Off again if there is none.
    Q_PROPERTY(bool camera READ camera WRITE setCamera NOTIFY cameraChanged)
    /// This side sharing a screen or a window, or the desktop's dialog open
    /// to choose one.
    Q_PROPERTY(bool sharing READ sharing NOTIFY sharingChanged)
    Q_PROPERTY(bool choosing READ choosing NOTIFY choosingChanged)
    /// The id of the member whose share plays into `video`; "" for none.
    Q_PROPERTY(QString watching READ watching NOTIFY watchingChanged)
    /// That member as a row of `members` (a map of its roles); empty for none.
    Q_PROPERTY(QVariantMap watched READ watched NOTIFY watchedChanged)
    /// Where the share watched is drawn, and this side's own camera.
    Q_PROPERTY(ratcast::app::VideoView* video READ video WRITE setVideo NOTIFY videoChanged)
    Q_PROPERTY(ratcast::app::VideoView* preview READ preview WRITE setPreview NOTIFY previewChanged)
    /// The call over the last second, as its monitor saw it: a section a part,
    /// the call first, each a map of `title`, `quality`, `reason` and `rows`
    /// (maps of `label` and `value`). Empty outside a room.
    Q_PROPERTY(QVariantList stats READ stats NOTIFY statsChanged)
    /// The call's quality — "good", "fair" or "poor" — and why it is not
    /// good; "" before the first report.
    Q_PROPERTY(QString quality READ quality NOTIFY statsChanged)
    Q_PROPERTY(QString qualityReason READ qualityReason NOTIFY statsChanged)
    /// Print the monitor's report on standard output every so many seconds
    /// while in a room; 0: never.
    Q_PROPERTY(int statsEvery READ statsEvery WRITE setStatsEvery NOTIFY statsEveryChanged)

public:
    enum class State { Idle, Joining, InRoom };
    Q_ENUM(State)

    /// The loudest a member is heard: twice the amplitude, +6 dB.
    static constexpr double kMaxVolume = 2.0;

    /// What the controller takes from the system; tests stand in for it.
    struct Environment {
        /// What every room starts from: where it listens, its protocol.
        CallConfig call = {};
        /// Where the identity is kept.
        std::string data_dir = {};
        std::function<Result<ScreenShareSources>(const ScreenShareConfig&, std::stop_token)>
            open_share = {};
        /// The speaker; null: nothing is played.
        std::function<std::unique_ptr<AudioOutput>()> open_speaker = {};
        /// The microphone; null: this side is not heard.
        std::function<std::unique_ptr<AudioSource>()> open_microphone = {};
        /// What the computer plays, in the speaker's format: the echo the
        /// microphone is cleaned of, a shared film's too; null: the speaker's.
        std::function<std::unique_ptr<AudioSource>()> open_system_sound = {};
        /// The camera; null: there is none.
        std::function<std::unique_ptr<VideoSource>()> open_camera = {};
        /// How the voice is cleaned: its devices' formats and what is done.
        VoiceConfig voice = {};
        /// What the others call this side until the user says otherwise.
        QString name = {};
        /// The server rooms are met at until the user says otherwise.
        QString server = {};
        /// The screen kept on while a share is watched; null: left alone.
        std::function<std::unique_ptr<ScreenAwake>()> keep_screen_awake = {};
        /// Where how each member is heard — how loud, muted here — is kept
        /// from one run to the next; null: while the controller lives.
        std::function<std::unique_ptr<QSettings>()> settings = {};

        /// This computer's: its desktop, its speaker, the user's data
        /// directory, settings and name, discoverable through the DHT, the
        /// build's server (RATCAST_DEFAULT_SERVER).
        static Environment system();
    };

    explicit CallController(QObject* parent = nullptr);
    ~CallController() override;

    /// Before the first create() or join().
    void setEnvironment(Environment environment);

    /// Make a room, and be in it: others join by its invites.
    Q_INVOKABLE void create();
    /// Join the room at `link`.
    Q_INVOKABLE void join(const QString& link);
    Q_INVOKABLE void leave();
    /// Ask the desktop for a screen (or a window) and share it, with the
    /// computer's sound if asked. In a room, while not sharing.
    Q_INVOKABLE void share(bool window, bool sound);
    /// Stop sharing, or call off the desktop's dialog.
    Q_INVOKABLE void stopSharing();
    /// Watch the share of the member of `id`; "": none.
    Q_INVOKABLE void watch(const QString& id);
    /// How loud the member of `id` is heard on this side alone: from 0
    /// through 1, as they come, to kMaxVolume; kept for them, in this room
    /// and the next.
    Q_INVOKABLE void setVolume(const QString& id, double volume);
    /// The member of `id` not heard on this side at all, their volume kept.
    Q_INVOKABLE void setMutedHere(const QString& id, bool muted);
    /// Draw the camera of the member of `id` into `view`; null: nowhere.
    Q_INVOKABLE void setCameraView(const QString& id, ratcast::app::VideoView* view);
    /// Put `text` on the clipboard: an invite, for the user to send.
    Q_INVOKABLE void copy(const QString& text);

    State state() const { return state_; }
    QStringList invites() const { return invites_; }
    QString notice() const { return notice_; }
    QString name() const { return name_; }
    void setName(const QString& name);
    QString server() const { return server_; }
    void setServer(const QString& server);
    MembersModel* members() { return &members_; }
    static double maxVolume() { return kMaxVolume; }
    const MembersModel& memberList() const { return members_; }
    int cameras() const { return cameras_on_; }
    bool muted() const { return muted_; }
    void setMuted(bool muted);
    bool deafened() const { return deafened_; }
    void setDeafened(bool deafened);
    double shareVolume() const { return share_volume_; }
    void setShareVolume(double volume);
    bool speaking() const { return speaking_; }
    bool camera() const { return camera_; }
    void setCamera(bool on);
    bool sharing() const { return sharing_; }
    bool choosing() const { return choosing_; }
    QString watching() const { return watching_; }
    QVariantMap watched() const { return watched_; }
    VideoView* video() const { return video_; }
    void setVideo(VideoView* video);
    VideoView* preview() const { return preview_; }
    void setPreview(VideoView* view);
    QVariantList stats() const { return stats_; }
    QString quality() const { return quality_; }
    QString qualityReason() const { return quality_reason_; }
    int statsEvery() const { return stats_every_; }
    void setStatsEvery(int seconds);

Q_SIGNALS:
    void stateChanged();
    void invitesChanged();
    void noticeChanged();
    void nameChanged();
    void serverChanged();
    void camerasChanged();
    void mutedChanged();
    void deafenedChanged();
    void shareVolumeChanged();
    void speakingChanged();
    void cameraChanged();
    void sharingChanged();
    void choosingChanged();
    void watchingChanged();
    void watchedChanged();
    void videoChanged();
    void previewChanged();
    void statsChanged();
    void statsEveryChanged();

private:
    struct Pictures;
    struct Cameras;

    void set_state(State state);
    void set_notice(QString notice);
    void set_sharing(bool sharing);
    void set_choosing(bool choosing);
    void set_watching(QString id);
    /// Start the call `config` says and be in `state`: talking, reporting.
    void start(CallConfig config, State state);
    /// The server named, into `config`; false, with a notice, if it is none.
    bool meet_at_server(CallConfig& config);
    /// The sources the desktop gave for the share asked in room `generation`,
    /// or why not.
    void share_opened(uint64_t generation, const ScreenShareConfig& config,
                      Result<ScreenShareSources> sources);
    /// A call's handlers, which bring its events onto the GUI thread for
    /// as long as it is the call of `generation`.
    CallHandlers handlers(uint64_t generation);
    /// Run `event` on the GUI thread if the call of `generation` is still on.
    void post(uint64_t generation, std::function<void()> event);
    /// This side's voice on the call just started, if there is a microphone,
    /// and its camera if on.
    void talk();
    /// The microphone and the speaker as muted and deafened say, and the
    /// others told.
    void apply_voice();
    /// The camera, sent and seen; off with `notice` if it cannot be.
    void start_camera();
    void camera_off(QString notice = {});
    /// Draw what `pictures` gets into `view` from now on, as `slot` keeps it.
    static bool attach(QPointer<VideoView>& slot, Pictures& pictures, VideoView* view);
    /// Say who this side is to the others, as it is now.
    void tell_presence();
    /// The others, and this side's own voice, as they are now: a few times a
    /// second, and as the call says something changed.
    void update_members();
    /// `watched` anew, from the row of the member watched.
    void update_watched();
    /// How a member is heard on this side, as the user set it.
    struct Hearing {
        double volume = 1.0;
        bool muted = false;
        friend bool operator==(const Hearing&, const Hearing&) = default;
    };
    /// What the settings keep, and the call told it.
    void load_hearing();
    /// `id` heard so from now on: kept, saved, the call told.
    void hear(const QString& id, Hearing hearing);
    void tell_hearing(const QString& id, const Hearing& hearing);
    void update_levels();
    /// Report on the call just started, every second and, if asked, on
    /// standard output.
    void start_monitoring();
    void stop_monitoring();
    void update_stats();
    void print_stats();
    /// Back to Idle; `notice` says why, if it was not the user.
    void end(QString notice = {});

    Environment environment_;
    State state_ = State::Idle;
    QStringList invites_;
    QString notice_;
    QString name_;
    QString server_;
    MembersModel members_;
    QMap<QString, Hearing> hearing_;  ///< the members not heard as they come, by id
    int cameras_on_ = 0;
    bool muted_ = false;
    bool deafened_ = false;
    double share_volume_ = 1.0;
    bool speaking_ = false;
    bool camera_ = false;
    bool sharing_ = false;
    bool choosing_ = false;
    QString watching_;
    QVariantMap watched_;
    QTimer levels_;
    QTimer reporting_;
    QTimer printing_;
    QVariantList stats_;
    QString quality_;
    QString quality_reason_;
    int stats_every_ = 0;
    unsigned printed_seconds_ = 0;  ///< into the call, as printed
    QPointer<VideoView> video_;
    QPointer<VideoView> preview_;
    /// Where the share watched goes, and this side's camera: each its view.
    std::shared_ptr<Pictures> pictures_;
    std::shared_ptr<Pictures> preview_pictures_;
    /// Where each member's camera goes, asked by the call's threads.
    std::shared_ptr<Cameras> cameras_;
    QMap<QString, QPointer<VideoView>> camera_views_;  ///< by member id

    /// Counts rooms begun: events and dialogs of an earlier one are dropped.
    uint64_t generation_ = 0;
    std::jthread dialog_;  ///< the desktop's
    std::unique_ptr<VoiceChannel> voice_;
    std::unique_ptr<ScreenAwake> awake_;    ///< while a share is watched
    std::unique_ptr<AudioOutput> speaker_;  ///< the voice channel's
    std::unique_ptr<Call> call_;            ///< after the speaker: it plays into it
    /// After the call, which they watch: the panel's and standard output's.
    std::unique_ptr<CallMonitor> monitor_;
    std::unique_ptr<CallMonitor> printer_;
};

}  // namespace ratcast::app
