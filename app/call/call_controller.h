#pragma once

/**
 * @file call_controller.h
 * @brief A call as QML sees it: share a screen or join one, and leave.
 *
 * The controller holds at most one Call and says where it stands:
 *
 *   Idle ── share() ──▶ Choosing ──▶ Sharing     (the desktop's dialog, then
 *     │                    │                       others may join)
 *     └──── join() ──▶ Joining ◀──▶ Watching      (the picture plays, or is
 *                                                  waited for)
 *
 * and leave() goes back to Idle from anywhere. Either way both sides talk:
 * the microphone, cleaned of the speaker's echo and of noise, goes as a voice
 * (VoiceChannel), muted or not as the user says. What went wrong, or ended
 * without the user, is the notice: a share the desktop could not make, a link
 * that is none, the other side gone. The user calling off the desktop's
 * dialog is no news.
 *
 * Everything that is not about the UI — the sources, what is published and
 * how, playing — is client's (Call, screen_share.h); this keeps the state, runs
 * the dialog off the GUI thread and brings the call's events onto it. GUI
 * thread only.
 */

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

#include "ratcast/client/call.h"
#include "ratcast/client/screen_share.h"
#include "ratcast/client/voice.h"
#include "ratcast/engine/audio_device.h"
#include "video_view.h"

namespace ratcast::app {

class CallController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    /// rats:// links to this call while sharing: the first for the internet
    /// (the peer id), the rest for the networks this host is on.
    Q_PROPERTY(QStringList invites READ invites NOTIFY invitesChanged)
    /// Peers connected while sharing.
    Q_PROPERTY(int viewers READ viewers NOTIFY viewersChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    /// The microphone silenced; kept from one call to the next.
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    /// This side's voice on the microphone, and the other side's on the speaker.
    Q_PROPERTY(bool speaking READ speaking NOTIFY speakingChanged)
    Q_PROPERTY(bool hearing READ hearing NOTIFY hearingChanged)
    /// Where the other side's pictures are drawn.
    Q_PROPERTY(ratcast::app::VideoView* video READ video WRITE setVideo NOTIFY videoChanged)

public:
    enum class State { Idle, Choosing, Sharing, Joining, Watching };
    Q_ENUM(State)

    /// What the controller takes from the system; tests stand in for it.
    struct Environment {
        /// What every call starts from: where it listens, its protocol.
        CallConfig call = {};
        /// Where the identity and the desktop's choices are kept.
        std::string data_dir = {};
        std::function<Result<ScreenShareSources>(const ScreenShareConfig&, std::stop_token)>
            open_share = {};
        /// The speaker; null: the sound is not played.
        std::function<std::unique_ptr<AudioOutput>()> open_speaker = {};
        /// The microphone; null: this side is not heard.
        std::function<std::unique_ptr<AudioSource>()> open_microphone = {};
        /// How the voice is cleaned: its devices' formats and what is done.
        VoiceConfig voice = {};

        /// This computer's: its desktop, its speaker, the user's data
        /// directory, discoverable through the DHT.
        static Environment system();
    };

    explicit CallController(QObject* parent = nullptr);
    ~CallController() override;

    /// Before the first share() or join().
    void setEnvironment(Environment environment);

    /// Ask the desktop for a screen (or a window) and share it, with the
    /// computer's sound if asked.
    Q_INVOKABLE void share(bool window, bool sound);
    /// Call the sharer at `link` and watch.
    Q_INVOKABLE void join(const QString& link);
    Q_INVOKABLE void leave();
    /// Put `text` on the clipboard: an invite, for the user to send.
    Q_INVOKABLE void copy(const QString& text);

    State state() const { return state_; }
    QStringList invites() const { return invites_; }
    int viewers() const { return viewers_; }
    QString notice() const { return notice_; }
    VideoView* video() const { return video_; }
    void setVideo(VideoView* video);
    bool muted() const { return muted_; }
    void setMuted(bool muted);
    bool speaking() const { return speaking_; }
    bool hearing() const { return hearing_; }

Q_SIGNALS:
    void stateChanged();
    void invitesChanged();
    void viewersChanged();
    void noticeChanged();
    void videoChanged();
    void mutedChanged();
    void speakingChanged();
    void hearingChanged();

private:
    struct Pictures;

    void set_state(State state);
    void set_notice(QString notice);
    void count_viewers();
    /// The sources the desktop gave for share number `generation`, or why not.
    void share_opened(uint64_t generation, const ScreenShareConfig& config,
                      Result<ScreenShareSources> sources);
    /// A call's handlers, which bring its events onto the GUI thread for
    /// as long as it is the call of `generation`.
    CallHandlers handlers(uint64_t generation);
    /// Run `event` on the GUI thread if the call of `generation` is still on.
    void post(uint64_t generation, std::function<void()> event);
    /// The speaker, through a voice channel of its own, for a call about to
    /// start.
    void open_voice();
    /// This side's voice on the call just started, if there is a microphone.
    void talk();
    /// Who is speaking, as the voice channel says: a few times a second.
    void update_levels();
    /// Back to Idle; `notice` says why, if it was not the user.
    void end(QString notice = {});

    Environment environment_;
    State state_ = State::Idle;
    QStringList invites_;
    int viewers_ = 0;
    QString notice_;
    bool muted_ = false;
    bool speaking_ = false;
    bool hearing_ = false;
    QTimer levels_;
    QPointer<VideoView> video_;
    /// Where the call's pictures go: the view, as it changes.
    std::shared_ptr<Pictures> pictures_;

    /// Counts calls and shares begun: events of an earlier one are dropped.
    uint64_t generation_ = 0;
    std::jthread choosing_;  ///< the desktop's dialog
    std::unique_ptr<VoiceChannel> voice_;
    std::unique_ptr<AudioOutput> speaker_;  ///< the voice channel's
    std::unique_ptr<Call> call_;            ///< after the speaker: it plays into it
};

}  // namespace ratcast::app
