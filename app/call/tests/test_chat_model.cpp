// What is said as QML lists it: in the order it was said, each once, read or
// not; and a text shown as it was written, its web links made links.

#include "chat_model.h"

#include <gtest/gtest.h>

#include <QTextBlock>
#include <QTextDocument>

#include <array>
#include <vector>

using namespace ratcast;
using namespace ratcast::app;
using namespace std::chrono_literals;

namespace {

constexpr WallTime kNoon{std::chrono::hours(482'000)};

PeerId peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

ChatMessage said(const std::string& text, WallTime sent, uint8_t by = 1) {
    return {peer(by), "member " + std::to_string(by), sent, text};
}

QStringList texts(const ChatModel& chat) {
    QStringList out;
    for (int row = 0; row < chat.rowCount(); ++row) {
        out << chat.data(chat.index(row), ChatModel::TextRole).toString();
    }
    return out;
}

bool continued(const ChatModel& chat, int row) {
    return chat.data(chat.index(row), ChatModel::ContinuedRole).toBool();
}

/// What `html` shows, and the links in it.
struct Shown {
    QString text;
    QStringList links;
};

Shown shown(const QString& html) {
    QTextDocument document;
    document.setHtml(html);
    Shown out{document.toPlainText(), {}};
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (format.isAnchor()) out.links << format.anchorHref();
        }
    }
    return out;
}

}  // namespace

TEST(ChatModel, ListsWhatIsSaidInTheOrderItWasSaidEachOnce) {
    ChatModel chat;
    chat.add(said("second", kNoon + 1s), false);
    chat.add(said("third", kNoon + 2s), false);
    chat.add(said("first", kNoon), false);  // told late
    chat.add(said("second", kNoon + 1s), false);
    EXPECT_EQ(texts(chat), (QStringList{"first", "second", "third"}));

    const QModelIndex first = chat.index(0);
    EXPECT_EQ(chat.data(first, ChatModel::NameRole), "member 1");
    EXPECT_EQ(chat.data(first, ChatModel::AuthorRole).toString().size(), 64);
    EXPECT_EQ(chat.data(first, ChatModel::PeerRole).toString().size(), 8);
    EXPECT_FALSE(chat.data(first, ChatModel::TimeRole).toString().isEmpty());
    EXPECT_EQ(chat.roleNames().value(ChatModel::HtmlRole), "html");

    chat.clear();
    EXPECT_EQ(chat.rowCount(), 0);
}

TEST(ChatModel, AMessageSoonAfterItsAuthorsLastContinuesIt) {
    ChatModel chat;
    chat.add(said("a", kNoon, 1), false);
    chat.add(said("a, more", kNoon + 1min, 1), false);
    chat.add(said("b", kNoon + 2min, 2), false);
    chat.add(said("b, later", kNoon + 2min + ChatModel::kContinuing, 2), false);
    EXPECT_FALSE(continued(chat, 0));
    EXPECT_TRUE(continued(chat, 1));
    EXPECT_FALSE(continued(chat, 2)) << "another's";
    EXPECT_FALSE(continued(chat, 3)) << "too long after";

    // One told late, between: what followed no longer continues.
    std::vector<int> changed;
    QObject::connect(&chat, &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex& from) { changed.push_back(from.row()); });
    chat.add(said("c", kNoon + 30s, 3), false);
    EXPECT_EQ(texts(chat), (QStringList{"a", "c", "a, more", "b", "b, later"}));
    EXPECT_FALSE(continued(chat, 2));
    EXPECT_EQ(changed, std::vector{2});
}

TEST(ChatModel, CountsWhatOthersSayWhileNobodyReads) {
    ChatModel chat;
    int unread = 0;
    QObject::connect(&chat, &ChatModel::unreadChanged, [&] { ++unread; });
    chat.add(said("hi", kNoon, 1), false);
    chat.add(said("hi", kNoon + 1s, 2), false);
    chat.add(said("mine", kNoon + 2s, 3), true);
    EXPECT_EQ(chat.unread(), 2);
    chat.setReading(true);
    EXPECT_EQ(chat.unread(), 0);
    chat.add(said("read at once", kNoon + 3s, 1), false);
    EXPECT_EQ(chat.unread(), 0);
    chat.setReading(false);
    chat.add(said("again", kNoon + 4s, 1), false);
    EXPECT_EQ(chat.unread(), 1);
    chat.clear();
    EXPECT_EQ(chat.unread(), 0);
    EXPECT_EQ(unread, 5);
}

TEST(ChatHtml, ShowsATextAsItWasWritten) {
    for (const QString& text :
         {QString("<b>not bold</b> & <a href=\"x\">no link</a>"), QString("one\ntwo"),
          QString("  indented\tand  spaced"), QString("Привет, 李明 🎮"), QString("&amp; &lt;")}) {
        const Shown out = shown(chat_html(text));
        EXPECT_EQ(out.text, text);
        EXPECT_TRUE(out.links.isEmpty()) << text.toStdString();
    }
}

TEST(ChatHtml, MakesWebLinksLinksAndNothingElse) {
    const Shown out = shown(
        chat_html("see https://example.com/a?b=1&c=<2>. Or (www.example.org/x_(y)), "
                  "http://example.net/\"q\" — not javascript:alert(1), file:///etc nor http://"));
    EXPECT_EQ(out.links, (QStringList{"https://example.com/a?b=1&c=",
                                      "http://www.example.org/x_(y)", "http://example.net/"}));
    EXPECT_EQ(out.text,
              "see https://example.com/a?b=1&c=<2>. Or (www.example.org/x_(y)), "
              "http://example.net/\"q\" — not javascript:alert(1), file:///etc nor http://");
}
