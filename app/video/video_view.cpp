#include "video_view.h"

#include <QMetaObject>
#include <QQuickWindow>
#include <utility>

#include "video_node.h"

namespace ratcast::app {

VideoView::VideoView(QQuickItem* parent) : QQuickItem(parent), inbox_(std::make_shared<Inbox>()) {
    inbox_->view = this;
    setFlag(ItemHasContents);
}

VideoView::~VideoView() {
    std::lock_guard lock(inbox_->mutex);
    inbox_->view = nullptr;
    inbox_->picture.reset();
}

VideoView::Sink VideoView::sink() {
    return [inbox = inbox_](VideoFrame picture) {
        std::lock_guard lock(inbox->mutex);
        if (!inbox->view) return;
        inbox->picture = std::move(picture);
        inbox->fresh = true;
        if (std::exchange(inbox->update_posted, true)) return;
        // Under the lock: the view cannot go before the call is queued, and
        // a queued call to a view that has gone is dropped.
        QMetaObject::invokeMethod(inbox->view, &VideoView::picture_arrived, Qt::QueuedConnection);
    };
}

void VideoView::clear() {
    {
        std::lock_guard lock(inbox_->mutex);
        inbox_->picture.reset();
        inbox_->fresh = false;
        inbox_->cleared = true;
    }
    if (!picture_size_.isEmpty()) {
        picture_size_ = {};
        Q_EMIT pictureSizeChanged();
    }
    update();
}

void VideoView::picture_arrived() {
    QSize size;
    {
        std::lock_guard lock(inbox_->mutex);
        inbox_->update_posted = false;
        if (inbox_->picture) {
            size = QSize(int(inbox_->picture->width()), int(inbox_->picture->height()));
        }
    }
    if (size != picture_size_) {
        picture_size_ = size;
        Q_EMIT pictureSizeChanged();
    }
    update();
}

QSGNode* VideoView::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<VideoNode*>(old);
    std::optional<VideoFrame> picture;
    bool cleared = false;
    {
        std::lock_guard lock(inbox_->mutex);
        if (inbox_->fresh || !node) picture = inbox_->picture;
        inbox_->fresh = false;
        cleared = std::exchange(inbox_->cleared, false);
    }
    if (picture) {
        if (!node) node = new VideoNode;
        node->set_picture(std::move(*picture));
    } else if (cleared) {
        delete node;
        return nullptr;
    }
    if (node) node->place(placed(node->picture_size()));
    return node;
}

QRectF VideoView::placed(QSize picture) const {
    const QRectF fit = fitted(picture, boundingRect());
    if (fit.isEmpty() || !window()) return fit;
    // In the window's units, where its pixels lie; a transform (a mirror) and
    // the item's own position go there and back.
    return mapRectFromScene(snapped(mapRectToScene(fit), window()->effectiveDevicePixelRatio()));
}

void VideoView::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    // Moved, the picture may be off the screen's pixels.
    if (now != before) update();
}

void VideoView::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged) update();
}

}  // namespace ratcast::app
