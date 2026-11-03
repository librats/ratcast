#include "qr_code.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

#include "qrcodegen.hpp"

namespace ratcast::app {

QrCode::QrCode(QQuickItem* parent) : QQuickPaintedItem(parent) { setAntialiasing(false); }

void QrCode::setText(const QString& text) {
    if (text == text_) return;
    text_ = text;
    size_ = 0;
    dark_.clear();
    if (!text_.isEmpty()) {
        try {
            const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(
                text_.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
            size_ = code.getSize();
            dark_.resize(size_t(size_) * size_t(size_));
            for (int y = 0; y < size_; ++y)
                for (int x = 0; x < size_; ++x)
                    dark_[size_t(y) * size_t(size_) + size_t(x)] = code.getModule(x, y);
        } catch (const qrcodegen::data_too_long&) {
            size_ = 0;  // too long for any QR code: nothing drawn
            dark_.clear();
        }
    }
    Q_EMIT textChanged();
    update();
}

void QrCode::paint(QPainter* painter) {
    const int side = int(std::min(width(), height()));
    if (size_ == 0 || side <= 0) return;
    const int all = modules();
    // Whole pixels a module, as large as fits; a code too small for one
    // pixel a module is drawn at one all the same.
    const int module = std::max(1, side / all);
    const int drawn = module * all;
    const int left = int(std::floor((width() - drawn) / 2));
    const int top = int(std::floor((height() - drawn) / 2));
    painter->fillRect(left, top, drawn, drawn, Qt::white);
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            if (!dark(x, y)) continue;
            painter->fillRect(left + (x + kQuiet) * module, top + (y + kQuiet) * module, module,
                              module, Qt::black);
        }
    }
}

}  // namespace ratcast::app
