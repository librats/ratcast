#pragma once

/**
 * @file chat_model.h
 * @brief What is said in a room, as a list QML shows: one row a message.
 *
 * Rows are in the order the messages were said (ChatLog's: by when, then by
 * whom), so that a message told late — said before this side came, or while
 * it was away — goes where it belongs. A text is shown as it was written:
 * whatever it says is escaped, and only its web links are made links (`html`).
 * What comes while nobody reads is counted unread. GUI thread only.
 */

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QString>

#include <chrono>
#include <vector>

#include "ratcast/session/chat.h"

namespace ratcast::app {

/// `text` as rich text that shows it as it is: every character escaped, line
/// breaks kept, and each http://, https:// or www. link made a link to it.
QString chat_html(const QString& text);

class ChatModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("A room's: CallController::chat")
    /// Messages of others that came while nobody was reading.
    Q_PROPERTY(int unread READ unread NOTIFY unreadChanged)
    /// Somebody reads: the chat is shown. Nothing is unread while so.
    Q_PROPERTY(bool reading READ reading WRITE setReading NOTIFY readingChanged)

public:
    enum Role {
        AuthorRole = Qt::UserRole + 1,  ///< the author's peer id, in full
        PeerRole,                       ///< a short one, to tell authors of one name apart
        NameRole,                       ///< what the author called itself; "" for nothing
        TextRole,                       ///< as written
        HtmlRole,                       ///< as chat_html() shows it
        TimeRole,                       ///< when, by the author's clock, as this locale says it
        MineRole,                       ///< said by this side
        /// By the author of the message before, soon after it: shown under it.
        ContinuedRole,
    };

    /// How long after a message one by the same author continues it.
    static constexpr std::chrono::minutes kContinuing{5};

    using QAbstractListModel::QAbstractListModel;

    /// `message` where it was said among the rest; nothing for one listed.
    /// `mine`: this side said it. Unread unless mine or being read.
    void add(const ChatMessage& message, bool mine);
    /// Nothing said: a new room.
    void clear();

    int unread() const { return unread_; }
    bool reading() const { return reading_; }
    void setReading(bool reading);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void unreadChanged();
    void readingChanged();

private:
    struct Row {
        ChatMessage message;
        QString html;
        bool mine = false;
    };

    bool continued(size_t row) const;
    void set_unread(int unread);

    std::vector<Row> rows_;
    int unread_ = 0;
    bool reading_ = false;
};

}  // namespace ratcast::app
