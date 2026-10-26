// MembersModel: members going, coming and changing as rows removed, inserted
// and changed in place — never a row made again for a member that stays.

#include "members_model.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ratcast::app;

namespace {

Member member(const char* id, bool speaking = false) {
    return Member{.id = id,
                  .peer = QString(id).left(8),
                  .name = QString("name of ") + id,
                  .speaking = speaking};
}

/// What a view would hear from the model.
struct Heard {
    explicit Heard(MembersModel& model) {
        QObject::connect(&model, &MembersModel::rowsInserted,
                         [this](const QModelIndex&, int first, int) { inserted.push_back(first); });
        QObject::connect(&model, &MembersModel::rowsRemoved,
                         [this](const QModelIndex&, int first, int) { removed.push_back(first); });
        QObject::connect(
            &model, &MembersModel::dataChanged,
            [this](const QModelIndex& from, const QModelIndex&) { changed.push_back(from.row()); });
    }
    std::vector<int> inserted;
    std::vector<int> removed;
    std::vector<int> changed;
};

std::vector<QString> ids(const MembersModel& model) {
    std::vector<QString> ids;
    for (int row = 0; row < model.rowCount(); ++row)
        ids.push_back(model.get(row).value("peerId").toString());
    return ids;
}

}  // namespace

TEST(MembersModel, MembersComeGoAndChangeInPlace) {
    MembersModel model;
    model.set({member("b"), member("d")});
    Heard heard(model);

    // "a" and "c" come, "d" goes, "b" starts speaking.
    model.set({member("a"), member("b", true), member("c")});
    EXPECT_EQ(ids(model), (std::vector<QString>{"a", "b", "c"}));
    EXPECT_EQ(heard.removed, std::vector{1});
    EXPECT_EQ(heard.inserted, (std::vector{0, 2}));
    EXPECT_EQ(heard.changed, std::vector{1});
    EXPECT_TRUE(model.get(1).value("speaking").toBool());

    // The same again: nothing to tell.
    model.set({member("a"), member("b", true), member("c")});
    EXPECT_EQ(heard.inserted.size() + heard.removed.size() + heard.changed.size(), 4u);

    model.set({});
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(MembersModel, ARowIsAMapOfItsRoles) {
    MembersModel model;
    model.set({member("a")});
    const QVariantMap row = model.get(0);
    EXPECT_EQ(row.value("name").toString(), "name of a");
    EXPECT_EQ(row.value("peer").toString(), "a");
    EXPECT_FALSE(row.value("sharing").toBool());
    EXPECT_EQ(row.value("volume").toDouble(), 1.0) << "heard as it comes";
    EXPECT_FALSE(row.value("mutedHere").toBool());
    EXPECT_FALSE(row.value("reconnecting").toBool());
    EXPECT_TRUE(row.contains("cameraShown"));
    EXPECT_TRUE(model.get(1).isEmpty());
    EXPECT_TRUE(model.get(-1).isEmpty());
}
