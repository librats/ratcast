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

    /// Draw `picture` from the next frame of the window on: I420 or NV12 —
    /// in memory, or in Android's GPU memory, drawn where it is when the
    /// window is drawn through OpenGL ES (`gles`), else copied out first.
    void set_picture(VideoFrame picture, bool gles);
    /// The size of the picture drawn; empty before the first.
    QSize picture_size() const noexcept { return picture_size_; }
    /// Draw the picture over `rect`.
    void place(const QRectF& rect);

private:
    /// The material for pictures in memory, made the node's if it is not.
    VideoMaterial* planes();

    VideoMaterial* material_ = nullptr;  ///< the node's, if pictures in memory are drawn
    QSize picture_size_;
    QRectF placed_;  ///< where the picture is drawn now
    /// What of the texture is the picture: a buffer in GPU memory may be
    /// larger than its picture.
    QRectF source_{0, 0, 1, 1};
};

/// Where a `picture`-sized picture goes in `bounds`: as large as fits, its
/// shape kept, centred.
QRectF fitted(QSizeF picture, const QRectF& bounds);

/// `rect` with its edges moved to the nearest of the screen's pixels, at
/// `ratio` of them to a unit of the window's (its device pixel ratio): a
/// picture drawn there is not blurred by a fraction of a pixel's shift, and one
/// of the screen's size is drawn pixel for pixel.
QRectF snapped(const QRectF& rect, qreal ratio);

}  // namespace ratcast::app
