#pragma once

/**
 * @file video_node.h
 * @brief VideoView's scene graph node: a picture's planes as textures, turned
 * into colour by a shader. Render thread only.
 */

#include <QRectF>
#include <QSGGeometryNode>
#include <QSize>

#include "ratcast/engine/video_frame.h"

namespace ratcast::app {

class VideoMaterial;

class VideoNode final : public QSGGeometryNode {
public:
    VideoNode();

    /// Draw `picture` from the next frame of the window on: I420 or NV12.
    void set_picture(VideoFrame picture);
    /// Lay the picture out in `bounds`: fitted, its shape kept, centred.
    void fit(const QRectF& bounds);

private:
    VideoMaterial* material_;  ///< the node's own
    QSize picture_size_;
    QRectF placed_;  ///< where the picture is drawn now
};

/// Where a `picture`-sized picture goes in `bounds`: as large as fits, its
/// shape kept, centred.
QRectF fitted(QSizeF picture, const QRectF& bounds);

}  // namespace ratcast::app
