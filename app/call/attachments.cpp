#include "attachments.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QUuid>

#include <algorithm>

namespace ratcast::app {

Result<SharedFile> share_file(const QString& path, const std::atomic<bool>* stop) {
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) return fail(Errc::Unavailable);
    const std::string utf8 = info.absoluteFilePath().toStdString();
    const Result<FileId> id = file_id(utf8, stop);
    if (!id) return fail(id.error());

    SharedFile file{.attachment = {.id = *id,
                                   .name = info.fileName().toStdString(),
                                   .size = static_cast<uint64_t>(info.size())},
                    .path = utf8};
    // An image: what it looks like, as it is shown — turned as it says.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.canRead()) {
        const QImage image = reader.read();
        if (!image.isNull() && image.width() <= int(ChatAttachment::kMaxImageSide) &&
            image.height() <= int(ChatAttachment::kMaxImageSide)) {
            file.attachment.width = static_cast<uint32_t>(image.width());
            file.attachment.height = static_cast<uint32_t>(image.height());
            file.attachment.preview = preview_of(image);
        }
    }
    if (!is_valid(file.attachment)) return fail(Errc::InvalidArgument);
    return file;
}

std::vector<uint8_t> preview_of(const QImage& image) {
    if (image.isNull()) return {};
    for (int side = kPreviewSide; side >= 16; side = side * 3 / 4) {
        QImage small = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        // JPEG has no transparency: what shows through, the chat's colour.
        QImage flat(small.size(), QImage::Format_RGB888);
        flat.fill(QColor(0x2b, 0x2d, 0x31));
        QPainter(&flat).drawImage(0, 0, small);
        for (int quality = 70; quality >= 30; quality -= 20) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (!flat.save(&buffer, "JPG", quality)) return {};
            if (size_t(bytes.size()) <= ChatAttachment::kMaxPreviewSize)
                return {bytes.begin(), bytes.end()};
        }
    }
    return {};
}

QString preview_url(const std::vector<uint8_t>& preview) {
    if (preview.empty()) return {};
    const QByteArray bytes(reinterpret_cast<const char*>(preview.data()),
                           qsizetype(preview.size()));
    return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

QString save_pasted(const QImage& image, const QString& folder) {
    if (image.isNull()) return {};
    const QString into = QDir(folder).filePath(QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().mkpath(into)) return {};
    const QString name = QStringLiteral("image-%1.png")
                             .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    const QString path = QDir(into).filePath(name);
    return image.save(path, "PNG") ? path : QString();
}

QString free_path(const QString& folder, const QString& name) {
    const QDir dir(folder);
    if (!dir.exists(name)) return dir.filePath(name);
    const QFileInfo info(name);
    // "archive.tar.gz" as "archive (1).tar.gz".
    const QString base = info.completeBaseName().isEmpty() ? name : info.baseName();
    const QString suffix = info.completeBaseName().isEmpty() || info.completeSuffix().isEmpty()
                               ? QString()
                               : QStringLiteral(".") + info.completeSuffix();
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        if (!dir.exists(candidate)) return dir.filePath(candidate);
    }
}

QString size_text(uint64_t size) {
    return QLocale().formattedDataSize(qint64(std::min<uint64_t>(size, INT64_MAX)), 1,
                                       QLocale::DataSizeTraditionalFormat);
}

}  // namespace ratcast::app
