#include "release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ratcast::app::update {

std::optional<Release> parse_release(const QByteArray& json, QStringView suffix) {
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    if (release.isEmpty() || release.value(u"draft").toBool() ||
        release.value(u"prerelease").toBool()) {
        return std::nullopt;
    }
    // "v0.2.0": the whole tag a version, nothing after it.
    QString tag = release.value(u"tag_name").toString();
    if (tag.startsWith(u'v')) tag.remove(0, 1);
    qsizetype parsed = 0;
    Release out;
    out.version = QVersionNumber::fromString(tag, &parsed);
    if (out.version.isNull() || parsed != tag.size()) return std::nullopt;
    out.page = QUrl(release.value(u"html_url").toString());

    for (const QJsonValue& entry : release.value(u"assets").toArray()) {
        const QJsonObject asset = entry.toObject();
        const QString name = asset.value(u"name").toString();
        if (!name.startsWith(u"ratcast-") || !name.endsWith(suffix)) continue;
        out.asset = name;
        out.url = QUrl(asset.value(u"browser_download_url").toString());
        out.size = asset.value(u"size").toInteger();
        // "sha256:<hex>"
        const QString digest = asset.value(u"digest").toString();
        if (digest.startsWith(u"sha256:")) {
            out.sha256 = QByteArray::fromHex(digest.mid(7).toLatin1());
        }
        if (!out.url.isValid() || out.url.scheme() != u"https") return std::nullopt;
        return out;
    }
    return std::nullopt;
}

}  // namespace ratcast::app::update
