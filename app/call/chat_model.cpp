#include "chat_model.h"

#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <tuple>

namespace ratcast::app {
namespace {

/// What tells messages apart, and orders them.
auto key(const ChatMessage& message) { return std::tie(message.sent, message.author); }

QString escaped(const QString& text) {
    return text.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"));
}

/// `link` without what ends the sentence it ends: a full stop, a comma, a
/// bracket it does not open.
QString trimmed_link(QString link) {
    while (!link.isEmpty()) {
        const QChar last = link.back();
        const bool unopened = (last == u')' && link.count(u'(') < link.count(u')')) ||
                              (last == u']' && link.count(u'[') < link.count(u']'));
        if (!unopened && !QStringLiteral(".,;:!?'\"").contains(last)) break;
        link.chop(1);
    }
    return link;
}

QString time_of(WallTime sent) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sent.time_since_epoch());
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms.count());
    const QLocale locale;
    if (at.date() == QDate::currentDate()) return locale.toString(at.time(), QLocale::ShortFormat);
    return locale.toString(at, QLocale::ShortFormat);
}

}  // namespace

QString chat_html(const QString& text) {
    static const QRegularExpression links(QStringLiteral(R"((?:https?://|www\.)[^\s<>"]+)"),
                                          QRegularExpression::CaseInsensitiveOption);
    // Spaces as written: an indented line stays indented.
    QString html = QStringLiteral("<span style=\"white-space: pre-wrap\">");
    qsizetype from = 0;
    for (QRegularExpressionMatchIterator it = links.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString link = trimmed_link(match.captured());
        const bool web = link.startsWith(QStringLiteral("www."), Qt::CaseInsensitive);
        if (link.size() <= (web ? 4 : link.indexOf(u':') + 3)) continue;  // a prefix alone
        const QString href = web ? QStringLiteral("http://") + link : link;
        html += escaped(text.mid(from, match.capturedStart() - from));
        html += QStringLiteral("<a href=\"%1\">%2</a>").arg(href.toHtmlEscaped(), escaped(link));
        from = match.capturedStart() + link.size();
    }
    html += escaped(text.mid(from));
    html += QStringLiteral("</span>");
    return html;
}

void ChatModel::add(const ChatMessage& message, bool mine) {
    const auto at = std::lower_bound(
        rows_.begin(), rows_.end(), message,
        [](const Row& row, const ChatMessage& m) { return key(row.message) < key(m); });
    if (at != rows_.end() && key(at->message) == key(message)) return;
    const int row = int(at - rows_.begin());
    beginInsertRows({}, row, row);
    rows_.insert(at, Row{message, chat_html(QString::fromStdString(message.text)), mine});
    endInsertRows();
    // The one after may continue this one, where it continued another.
    if (row + 1 < rowCount()) Q_EMIT dataChanged(index(row + 1), index(row + 1), {ContinuedRole});
    if (!mine && !reading_) set_unread(unread_ + 1);
}

void ChatModel::clear() {
    beginResetModel();
    rows_.clear();
    endResetModel();
    set_unread(0);
}

void ChatModel::setReading(bool reading) {
    if (reading == reading_) return;
    reading_ = reading;
    Q_EMIT readingChanged();
    if (reading) set_unread(0);
}

void ChatModel::set_unread(int unread) {
    if (unread == unread_) return;
    unread_ = unread;
    Q_EMIT unreadChanged();
}

bool ChatModel::continued(size_t row) const {
    if (row == 0) return false;
    const ChatMessage& before = rows_[row - 1].message;
    const ChatMessage& message = rows_[row].message;
    return before.author == message.author && message.sent - before.sent < kContinuing;
}

int ChatModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant ChatModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const auto row = size_t(index.row());
    const ChatMessage& message = rows_[row].message;
    switch (role) {
        case AuthorRole: return QString::fromStdString(message.author.to_hex());
        case PeerRole: return QString::fromStdString(message.author.short_hex());
        case NameRole: return QString::fromStdString(message.name);
        case TextRole: return QString::fromStdString(message.text);
        case HtmlRole: return rows_[row].html;
        case TimeRole: return time_of(message.sent);
        case MineRole: return rows_[row].mine;
        case ContinuedRole: return continued(row);
        default: return {};
    }
}

QHash<int, QByteArray> ChatModel::roleNames() const {
    return {
        {AuthorRole, "author"}, {PeerRole, "peer"},           {NameRole, "name"},
        {TextRole, "text"},     {HtmlRole, "html"},           {TimeRole, "time"},
        {MineRole, "mine"},     {ContinuedRole, "continued"},
    };
}

}  // namespace ratcast::app
