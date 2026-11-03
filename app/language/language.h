#pragma once

/**
 * @file language.h
 * @brief The language the window speaks: the system's, or one the user chose.
 *
 * The translations are the app's own (`i18n/ratcast_<code>.ts`, built into
 * its resources as `:/i18n/ratcast_<code>.qm`), one per language listed; the
 * strings are English as they are written. Chosen — at once, the window
 * redrawn in it (QQmlEngine::retranslate()) — the language is kept in the
 * settings as `language`; none chosen, the system's, as near as there is one,
 * or English. A language written from right to left (Arabic) turns the
 * window's layout round: QGuiApplication's layoutDirection, which Main.qml
 * mirrors by.
 *
 * Made once, in main(), before the window is loaded, so its first frame is in
 * the language; QML reaches that one, `Language` of Ratcast.Language. What C++
 * has already said (a notice) stays in the language it was said in. GUI
 * thread only.
 */

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QTranslator>
#include <QVariantList>

#include <functional>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace ratcast::app {

class Language : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// The language chosen, by its code ("ru", "zh_CN"); empty: the system's.
    Q_PROPERTY(QString chosen READ chosen WRITE setChosen NOTIFY chosenChanged)
    /// The language spoken now: the one chosen, or the system's as it was
    /// matched; "en" without a translation.
    Q_PROPERTY(QString current READ current NOTIFY chosenChanged)
    /// The languages there are, each a map of code and name (its own name, in
    /// itself): English first, then the translations built in.
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)

public:
    using SettingsFactory = std::function<std::unique_ptr<QSettings>()>;

    /// `engine`: the one whose QML is retranslated. `settings`: where the
    /// choice is kept (the app's own QSettings by default).
    explicit Language(QQmlEngine* engine, SettingsFactory settings = {}, QObject* parent = nullptr);
    ~Language() override;

    /// QML's: the one main() made.
    static Language* create(QQmlEngine* engine, QJSEngine* js);

    QString chosen() const { return chosen_; }
    void setChosen(const QString& code);
    QString current() const { return current_; }
    QVariantList languages() const;

Q_SIGNALS:
    void chosenChanged();

private:
    void apply();

    QPointer<QQmlEngine> engine_;
    SettingsFactory settings_;
    QString chosen_;
    QString current_ = QStringLiteral("en");
    std::unique_ptr<QTranslator> translator_;
};

}  // namespace ratcast::app
