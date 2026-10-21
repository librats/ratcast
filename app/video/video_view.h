#pragma once

/**
 * @file video_view.h
 * @brief A Qt Quick item that draws a player's pictures.
 *
 * Pictures come from any thread — a VideoPlayer's sink() — and the newest one
 * is drawn at the next frame of the window: those that come between two are
 * skipped, never queued. The picture is fitted into the item, its shape kept,
 * centred; what it leaves of the item is not drawn.
 *
 * Drawing is the GPU's: the picture's planes go up as they are (one byte a
 * sample for Y, U and V; two for NV12's interleaved chroma) and a shader turns
 * them into colour by the picture's own colour space. It needs a window drawn
 * through QRhi (the default: Vulkan, Metal, Direct3D or OpenGL), not Qt
 * Quick's software renderer.
 */

#include <QtQml/qqmlregistration.h>
#include <QQuickItem>
#include <QSize>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "ratcast/engine/video_frame.h"

namespace ratcast::app {

class VideoView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    /// The size of the picture drawn now; empty before the first.
    Q_PROPERTY(QSize pictureSize READ pictureSize NOTIFY pictureSizeChanged)

public:
    using Sink = std::function<void(VideoFrame picture)>;

    explicit VideoView(QQuickItem* parent = nullptr);
    ~VideoView() override;

    /// Where pictures go, from any thread. It may outlive the view: pictures
    /// then go nowhere.
    Sink sink();

    /// Draw nothing until the next picture.
    Q_INVOKABLE void clear();

    QSize pictureSize() const { return picture_size_; }

Q_SIGNALS:
    void pictureSizeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    /// What the sink and the view share: the newest picture, and the view
    /// while there is one. Locked by the sink while it tells the view.
    struct Inbox {
        std::mutex mutex;
        VideoView* view = nullptr;
        /// The newest, kept after it is drawn: a scene graph made anew (the
        /// window hidden and shown, the GPU reset) draws it again.
        std::optional<VideoFrame> picture;
        bool fresh = false;    ///< not yet given to the node
        bool cleared = false;  ///< the node to go
        bool update_posted = false;
    };

    /// On the GUI thread, after a sink took a picture.
    void picture_arrived();

    std::shared_ptr<Inbox> inbox_;
    QSize picture_size_;
};

}  // namespace ratcast::app
