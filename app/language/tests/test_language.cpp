#include "language.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>

namespace ratcast::app {
namespace {

class LanguageTest : public ::testing::Test {
protected:
    Language::SettingsFactory settings() const {
        return [file = folder_.filePath(QStringLiteral("ratcast.conf"))] {
            return std::make_unique<QSettings>(file, QSettings::IniFormat);
        };
    }

    QTemporaryDir folder_;
    QQmlEngine engine_;
};

bool listed(const Language& language, const QString& code) {
    for (const QVariant& entry : language.languages())
        if (entry.toMap().value(QStringLiteral("code")) == code) return true;
    return false;
}

TEST_F(LanguageTest, EnglishIsAlwaysThereAndFirst) {
    const Language language(&engine_, settings());
    const QVariantList languages = language.languages();
    ASSERT_FALSE(languages.isEmpty());
    EXPECT_EQ(languages.front().toMap().value(QStringLiteral("code")), QStringLiteral("en"));
    EXPECT_EQ(languages.front().toMap().value(QStringLiteral("name")), QStringLiteral("English"));
}

TEST_F(LanguageTest, NoneChosenIsTheSystems) {
    const Language language(&engine_, settings());
    EXPECT_TRUE(language.chosen().isEmpty());
    EXPECT_TRUE(listed(language, language.current()));
}

TEST_F(LanguageTest, ALanguageThereIsNotIsNotTaken) {
    Language language(&engine_, settings());
    language.setChosen(QStringLiteral("xx"));
    EXPECT_TRUE(language.chosen().isEmpty());
    EXPECT_FALSE(settings()()->contains(QStringLiteral("language")));
}

TEST_F(LanguageTest, EnglishChosenIsKeptAndSpoken) {
    {
        Language language(&engine_, settings());
        language.setChosen(QStringLiteral("en"));
        EXPECT_EQ(language.current(), QStringLiteral("en"));
        EXPECT_EQ(QGuiApplication::layoutDirection(), Qt::LeftToRight);
    }
    const Language again(&engine_, settings());
    EXPECT_EQ(again.chosen(), QStringLiteral("en"));
}

TEST_F(LanguageTest, RussianTranslatesTheWindowAtOnce) {
    Language language(&engine_, settings());
    if (!listed(language, QStringLiteral("ru"))) GTEST_SKIP() << "built without translations";
    language.setChosen(QStringLiteral("en"));

    // A binding of qsTr(), as the window's are, in the context of its file.
    QQmlComponent component(&engine_);
    component.setData("import QtQml\nQtObject { property string text: qsTr(\"Settings\") }",
                      QUrl(QStringLiteral("qrc:/SettingsDialog.qml")));
    const std::unique_ptr<QObject> object(component.create());
    ASSERT_TRUE(object) << component.errorString().toStdString();
    EXPECT_EQ(object->property("text").toString(), QStringLiteral("Settings"));

    language.setChosen(QStringLiteral("ru"));
    EXPECT_EQ(language.current(), QStringLiteral("ru"));
    EXPECT_EQ(object->property("text").toString(), QString::fromUtf8("Настройки"));
    // C++ too, the call's notices.
    EXPECT_NE(QCoreApplication::translate("ratcast::app::CallController",
                                          "That is not a link to a room."),
              QStringLiteral("That is not a link to a room."));

    language.setChosen(QString());
    EXPECT_FALSE(settings()()->contains(QStringLiteral("language")));
}

TEST_F(LanguageTest, ArabicTurnsTheLayoutRound) {
    Language language(&engine_, settings());
    if (!listed(language, QStringLiteral("ar"))) GTEST_SKIP() << "built without translations";
    language.setChosen(QStringLiteral("ar"));
    EXPECT_EQ(QGuiApplication::layoutDirection(), Qt::RightToLeft);
    language.setChosen(QStringLiteral("en"));
    EXPECT_EQ(QGuiApplication::layoutDirection(), Qt::LeftToRight);
}

}  // namespace
}  // namespace ratcast::app

int main(int argc, char** argv) {
    // Nothing is drawn: no display needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
