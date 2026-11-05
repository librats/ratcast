#pragma once

/**
 * @file links.h
 * @brief Links the system opens the app with: rooms', groups', people's,
 * devices'.
 *
 * However a link comes, it goes to the window's openLink(), which joins a
 * room's or a group's (group.h) unless a room is already on, and takes a
 * person's or a device's (people.h):
 * - Android: rats:// links and the website's (https://ratcast.app/r/#…,
 *   /g/#…, /u/#…, /d/#…: its App Links) by the manifest's intent filters, handed on
 *   by Qt to QDesktopServices' handlers;
 * - macOS: rats:// by the bundle's CFBundleURLTypes, as a QFileOpenEvent (and
 *   the handler, where Qt calls it);
 * - Windows and Linux: rats:// as the command line's link, the scheme
 *   registered by register_link_scheme(); a link given to a second process
 *   is handed to the first by SingleInstance.
 */

#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

namespace ratcast::app {

/// Links handed to the window (its openLink(link), which says whether it took
/// it); one that comes before the window is kept for it.
class LinkOpener final : public QObject {
    Q_OBJECT

public:
    /// QDesktopServices' handlers set; on macOS an event filter on the
    /// application as well.
    LinkOpener();
    ~LinkOpener() override;

    void show_to(QObject* window);

    /// The link handed to the window, which is brought to the front if it
    /// took it; whether it did (false before there is one, the link kept).
    bool hand_on(const QString& link);

public Q_SLOTS:
    /// QDesktopServices' handler. An https:// link that is no room's goes on
    /// to the browser.
    void open(const QUrl& url);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<QObject> window_;
    QString pending_;
};

/// One app on the desktop for the links: a process given a link hands it to
/// the one already running with the same data folder, which joins it unless
/// in a room already — then the new process goes on and joins it itself, as
/// a host may be in a room twice. A process with no link always goes on.
class SingleInstance final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(const QString& data_folder);

    /// Whether the process running took `link`: true, and this one ends.
    bool handed_over(const QString& link);

    /// Links handed over from now on go to `opener`.
    void listen(LinkOpener* opener);

private:
    QString name_;
    QLocalServer server_;
    QPointer<LinkOpener> opener_;
};

/// The rats:// scheme opens this app: on Windows the current user's
/// registration (taken over by a release package; another build's kept while
/// its program is there), for an AppImage a desktop entry of its own made the
/// scheme's default. Elsewhere the package's own registration does it (the
/// desktop entry's MimeType, the bundle's CFBundleURLTypes, Android's
/// manifest).
void register_link_scheme();

}  // namespace ratcast::app
