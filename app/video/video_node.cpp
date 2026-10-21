#include "video_node.h"

#include <rhi/qrhi.h>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>

#include <array>
#include <cstring>
#include <memory>

#include "ratcast/engine/video_convert.h"

namespace ratcast::app {
namespace {

/// Releases a QRhi resource once the frames in flight that use it are done.
struct DeleteLater {
    void operator()(QRhiResource* resource) const { resource->deleteLater(); }
};

}  // namespace

/// One plane of the picture as a texture: made on the GPU when first needed
/// or when the picture's size changes, and filled from the picture's memory
/// each time the picture does.
class PlaneTexture final : public QSGTexture {
public:
    PlaneTexture() {
        setFiltering(Linear);
        setHorizontalWrapMode(ClampToEdge);
        setVerticalWrapMode(ClampToEdge);
    }

    /// Plane `index` of `picture`, to be uploaded at the next commit. The
    /// picture stays alive until the next set(): the upload reads it in place.
    void set(const VideoFrame& picture, size_t index) {
        picture_ = picture;
        index_ = index;
        const bool chroma = index > 0;
        size_ = chroma ? QSize(int(picture.width() + 1) / 2, int(picture.height() + 1) / 2)
                       : QSize(int(picture.width()), int(picture.height()));
        format_ =
            chroma && picture.format() == PixelFormat::NV12 ? QRhiTexture::RG8 : QRhiTexture::R8;
        pending_ = true;
    }

    qint64 comparisonKey() const override { return qint64(reinterpret_cast<quintptr>(this)); }
    QRhiTexture* rhiTexture() const override { return texture_.get(); }
    QSize textureSize() const override { return size_; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    void commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch* batch) override {
        if (!pending_) return;
        pending_ = false;
        if (!texture_ || texture_->pixelSize() != size_ || texture_->format() != format_) {
            texture_.reset(rhi->newTexture(format_, size_));
            if (!texture_->create()) {
                texture_.reset();
                return;
            }
        }
        const VideoFrame::Plane& plane = picture_.plane(index_);
        const size_t rows = plane_rows(picture_.format(), index_, picture_.height());
        QRhiTextureSubresourceUploadDescription upload(QByteArray::fromRawData(
            reinterpret_cast<const char*>(plane.data), qsizetype(plane.stride * rows)));
        upload.setDataStride(quint32(plane.stride));
        batch->uploadTexture(texture_.get(), QRhiTextureUploadEntry(0, 0, upload));
    }

private:
    VideoFrame picture_;
    size_t index_ = 0;
    QSize size_;
    QRhiTexture::Format format_ = QRhiTexture::R8;
    bool pending_ = false;
    std::unique_ptr<QRhiTexture, DeleteLater> texture_;
};

/// The picture's planes and how they map to colour.
class VideoMaterial final : public QSGMaterial {
public:
    void set_picture(const VideoFrame& picture) {
        nv12_ = picture.format() == PixelFormat::NV12;
        for (size_t i = 0; i < picture.plane_count(); ++i) planes_[i].set(picture, i);
        yuv_to_rgb_ = rgb_from_yuv(picture.color_space());
    }

    /// The texture a sampler binding reads: Y at 1, then U at 2 and V at 3 —
    /// NV12's interleaved pair at both.
    PlaneTexture& plane_at(int binding) {
        const int index = binding - 1;
        return planes_[nv12_ && index == 2 ? 1 : index];
    }
    bool nv12() const { return nv12_; }
    const YuvToRgb& yuv_to_rgb() const { return yuv_to_rgb_; }

    QSGMaterialType* type() const override {
        static QSGMaterialType type;
        return &type;
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override {
        // Each picture is its own: never drawn in one batch with another.
        return this == other ? 0 : (this < other ? -1 : 1);
    }

private:
    std::array<PlaneTexture, 3> planes_;
    bool nv12_ = false;
    YuvToRgb yuv_to_rgb_{};
};

/// shaders/video.vert and video.frag: the uniform block is
/// { mat4 qt_Matrix; mat4 yuv_to_rgb; float qt_Opacity; int nv12; }.
class VideoShader final : public QSGMaterialShader {
public:
    VideoShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/ratcast/video/shaders/video.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/ratcast/video/shaders/video.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* now, QSGMaterial*) override {
        auto* material = static_cast<VideoMaterial*>(now);
        char* data = state.uniformData()->data();
        if (state.isMatrixDirty()) std::memcpy(data, state.combinedMatrix().constData(), 64);
        // Column by column, as GLSL lays a mat4 out; alpha is always 1.
        std::array<float, 16> matrix{};
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 3; ++row) {
                matrix[column * 4 + row] = material->yuv_to_rgb()[row][column];
            }
        }
        matrix[15] = 1;
        std::memcpy(data + 64, matrix.data(), 64);
        if (state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(data + 128, &opacity, 4);
        }
        const qint32 nv12 = material->nv12() ? 1 : 0;
        std::memcpy(data + 132, &nv12, 4);
        return true;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture, QSGMaterial* now,
                            QSGMaterial*) override {
        PlaneTexture& plane = static_cast<VideoMaterial*>(now)->plane_at(binding);
        plane.commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = &plane;
    }
};

QSGMaterialShader* VideoMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new VideoShader;
}

// ── The node ────────────────────────────────────────────────────────────────

VideoNode::VideoNode() : material_(new VideoMaterial) {
    setMaterial(material_);
    setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4));
    setFlags(OwnsMaterial | OwnsGeometry);
}

void VideoNode::set_picture(VideoFrame picture) {
    material_->set_picture(picture);
    picture_size_ = QSize(int(picture.width()), int(picture.height()));
    markDirty(DirtyMaterial);
}

void VideoNode::fit(const QRectF& bounds) {
    const QRectF place = fitted(picture_size_, bounds);
    if (place == placed_) return;
    placed_ = place;
    QSGGeometry::updateTexturedRectGeometry(geometry(), place, QRectF(0, 0, 1, 1));
    markDirty(DirtyGeometry);
}

QRectF fitted(QSizeF picture, const QRectF& bounds) {
    if (picture.isEmpty() || bounds.isEmpty()) return {};
    const QSizeF size = picture.scaled(bounds.size(), Qt::KeepAspectRatio);
    return {bounds.x() + (bounds.width() - size.width()) / 2,
            bounds.y() + (bounds.height() - size.height()) / 2, size.width(), size.height()};
}

}  // namespace ratcast::app
