#include "language.h"

#include <QFile>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLocale>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <utility>

namespace ratcast::app {

namespace {

/// The languages the window may speak, by their codes as the translations are
/// named (i18n/ratcast_<code>.ts), and each by its own name. English is the
/// strings themselves.
struct Known {
    const char* code;
    const char* name;  // UTF-8
};
constexpr std::array kKnown{
    Known{"en", "English"},   Known{"zh_CN", "简体中文"}, Known{"hi", "हिन्दी"},
    Known{"es", "Español"},   Known{"ar", "العربية"},     Known{"fr", "Français"},
    Known{"pt", "Português"}, Known{"ru", "Русский"},     Known{"de", "Deutsch"},
    Known{"ja", "日本語"},    Known{"ko", "한국어"},      Known{"tr", "Türkçe"},
};

const QString kEnglish = QStringLiteral("en");
const QString kSetting = QStringLiteral("language");

QString translation_of(const QString& code) {
    return QStringLiteral(":/i18n/ratcast_%1.qm").arg(code);
}

/// A language there is: English, or one with its translation built in.
bool available(const QString& code) {
    return code == kEnglish || QFile::exists(translation_of(code));
}

/// The language there is nearest to what the system's user prefers: their
/// languages in order, each matched by language and script (Brazil's
/// Portuguese is Portugal's too, Taiwan's Chinese is not China's) — the first
/// there is, or English. English first is English, though other languages of
/// theirs come after it.
QString systems_language() {
    for (const QString& wanted : QLocale::system().uiLanguages()) {
        const QLocale locale(wanted);
        for (const Known& known : kKnown) {
            const QString code = QString::fromLatin1(known.code);
            const QLocale ours(code);
            if (ours.language() == locale.language() && ours.script() == locale.script() &&
                available(code))
                return code;
        }
    }
    return kEnglish;
}

Language* instance = nullptr;

}  // namespace

Language::Language(QQmlEngine* engine, SettingsFactory settings, QObject* parent)
    : QObject(parent), engine_(engine), settings_(std::move(settings)) {
    if (!settings_) settings_ = [] { return std::make_unique<QSettings>(); };
    const QString kept = settings_()->value(kSetting).toString();
    if (available(kept)) chosen_ = kept;
    apply();
    instance = this;
}

Language::~Language() {
    if (translator_) QCoreApplication::removeTranslator(translator_.get());
    if (instance == this) instance = nullptr;
}

Language* Language::create(QQmlEngine*, QJSEngine*) {
    Q_ASSERT(instance);
    QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}

void Language::setChosen(const QString& code) {
    if (code == chosen_ || (!code.isEmpty() && !available(code))) return;
    chosen_ = code;
    const std::unique_ptr<QSettings> settings = settings_();
    if (code.isEmpty())
        settings->remove(kSetting);
    else
        settings->setValue(kSetting, code);
    apply();
    Q_EMIT chosenChanged();
}

QVariantList Language::languages() const {
    QVariantList list;
    for (const Known& known : kKnown) {
        const QString code = QString::fromLatin1(known.code);
        if (available(code))
            list.push_back(QVariantMap{{QStringLiteral("code"), code},
                                       {QStringLiteral("name"), QString::fromUtf8(known.name)}});
    }
    return list;
}

void Language::apply() {
    if (translator_) {
        QCoreApplication::removeTranslator(translator_.get());
        translator_.reset();
    }
    current_ = chosen_.isEmpty() ? systems_language() : chosen_;
    if (current_ != kEnglish) {
        auto translator = std::make_unique<QTranslator>();
        if (translator->load(translation_of(current_)) &&
            QCoreApplication::installTranslator(translator.get()))
            translator_ = std::move(translator);
        else
            current_ = kEnglish;
    }
    // Controls' own words and numbers in the language too, and its direction.
    QLocale::setDefault(chosen_.isEmpty() ? QLocale::system() : QLocale(current_));
    QGuiApplication::setLayoutDirection(QLocale(current_).textDirection());
    if (engine_) {
        engine_->setUiLanguage(current_);
        engine_->retranslate();
    }
}

}  // namespace ratcast::app
