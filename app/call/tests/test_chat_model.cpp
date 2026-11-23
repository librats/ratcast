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

TEST(ChatModel, ShowsEachAuthorAsWhoseItIs) {
    ChatModel chat;
    chat.add(said("hi", kNoon, 1), false);
    chat.add(said("hello", kNoon + 1s, 2), false);
    EXPECT_EQ(chat.data(chat.index(0), ChatModel::TagRole), "") << "not known yet";
    std::vector<int> changed;
    QObject::connect(&chat, &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex& from) { changed.push_back(from.row()); });
    chat.know(peer(1), "4F2K");
    EXPECT_EQ(chat.data(chat.index(0), ChatModel::TagRole), "4F2K");
    EXPECT_EQ(chat.data(chat.index(1), ChatModel::TagRole), "");
    EXPECT_EQ(changed, std::vector{0});
    chat.add(said("again", kNoon + 2s, 1), false);
    EXPECT_EQ(chat.data(chat.index(2), ChatModel::TagRole), "4F2K") << "and what it says later";
    EXPECT_EQ(chat.roleNames().value(ChatModel::TagRole), "tag");
}

TEST(ChatModel, ShowsEachFileAsItIsHere) {
    ChatModel chat;
    ChatAttachment photo{.name = "photo.jpg", .size = 2'000'000, .width = 4000, .height = 3000};
    photo.id.fill(1);
    photo.preview = {0xFF, 0xD8};
    ChatAttachment notes{.name = "notes.txt", .size = 1000};
    notes.id.fill(2);
    ChatMessage message = said("", kNoon, 1);
    message.attachments = {photo, notes};
    chat.add(message, false);
    chat.add(said("nothing", kNoon + 1s, 2), false);

    const auto files = [&] {
        return chat.data(chat.index(0), ChatModel::AttachmentsRole).toList();
    };
    ASSERT_EQ(files().size(), 2);
    QVariantMap shown = files()[0].toMap();
    EXPECT_EQ(shown.value("name"), "photo.jpg");
    EXPECT_EQ(shown.value("id").toString().size(), 64);
    EXPECT_TRUE(shown.value("image").toBool());
    EXPECT_EQ(shown.value("width"), 4000);
    EXPECT_EQ(shown.value("preview"), "data:image/jpeg;base64,/9g=");
    EXPECT_EQ(shown.value("status"), "none");
    EXPECT_FALSE(files()[1].toMap().value("image").toBool());
    EXPECT_EQ(files()[1].toMap().value("preview"), "");

    std::vector<int> changed;
    QObject::connect(&chat, &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex& from) { changed.push_back(from.row()); });
    using Status = ChatModel::FileState::Status;
    chat.set_file(photo.id, {.status = Status::Fetching, .received = 500'000});
    EXPECT_EQ(changed, std::vector{0}) << "the message carrying it alone";
    shown = files()[0].toMap();
    EXPECT_EQ(shown.value("status"), "fetching");
    EXPECT_DOUBLE_EQ(shown.value("progress").toDouble(), 0.25);
    chat.set_file(photo.id, {.status = Status::Here, .path = "/tmp/photo.jpg"});
    shown = files()[0].toMap();
    EXPECT_EQ(shown.value("status"), "here");
    EXPECT_EQ(shown.value("url"), "file:///tmp/photo.jpg");
    EXPECT_EQ(chat.file(photo.id).path, "/tmp/photo.jpg");

    const auto found = chat.attachment(notes.id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->first, notes);
    EXPECT_EQ(found->second, peer(1));
    FileId unknown{};
    EXPECT_FALSE(chat.attachment(unknown));

    chat.clear();
    EXPECT_EQ(chat.file(photo.id).status, Status::None);
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
