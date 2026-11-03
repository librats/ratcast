#pragma once

/**
 * @file qr_code.h
 * @brief A link drawn as a QR code, for a phone's camera to open.
 *
 * Its text encoded by qrcodegen (external/qrcodegen), medium error
 * correction, drawn black on white with the quiet zone the standard asks for
 * (four modules), the modules as large as the item lets them be in whole
 * pixels, centred.
 */

#include <QtQml/qqmlregistration.h>
#include <QQuickPaintedItem>
#include <QString>

#include <vector>

namespace ratcast::app {

class QrCode : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    /// The modules a side, quiet zone included; 0 for no text.
    Q_PROPERTY(int modules READ modules NOTIFY textChanged)

public:
    explicit QrCode(QQuickItem* parent = nullptr);

    QString text() const { return text_; }
    void setText(const QString& text);
    int modules() const { return size_ > 0 ? size_ + 2 * kQuiet : 0; }
    /// Whether the module at (x, y) — the code's own, no quiet zone — is dark.
    bool dark(int x, int y) const { return dark_[size_t(y) * size_t(size_) + size_t(x)]; }

    void paint(QPainter* painter) override;

    static constexpr int kQuiet = 4;

Q_SIGNALS:
    void textChanged();

private:
    QString text_;
    int size_ = 0;
    std::vector<bool> dark_;
};

}  // namespace ratcast::app
