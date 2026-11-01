#include "video_node.h"

#include <rhi/qrhi.h>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>

#include "ratcast/engine/video_convert.h"

#ifdef Q_OS_ANDROID
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include "ratcast/engine/android.h"
#endif

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

#ifdef Q_OS_ANDROID

// ── A picture in Android's GPU memory ───────────────────────────────────────

/// The buffer of a picture in GPU memory as an external texture of OpenGL
/// ES's, through an EGLImage of it: drawn where it is, the GPU turning its
/// YUV into colour by what the buffer says it is. Made on the render thread,
/// Qt's context current.
class ExternalTexture final : public QSGTexture {
public:
    ExternalTexture() {
        setFiltering(Linear);
        setHorizontalWrapMode(ClampToEdge);
        setVerticalWrapMode(ClampToEdge);
    }

    ~ExternalTexture() override {
        if (image_ != EGL_NO_IMAGE_KHR) destroy_image_(display_, image_);
    }

    /// `picture`, in `buffer`, drawn from the next commit on. The picture
    /// before is kept a frame longer: the GPU may still be drawing from it.
    void set(const VideoFrame& picture, AHardwareBuffer* buffer) {
        before_ = std::exchange(picture_, picture);
        buffer_ = buffer;
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buffer, &desc);
        size_ = QSize(int(desc.width), int(desc.height));
        pending_ = true;
    }

    qint64 comparisonKey() const override { return qint64(reinterpret_cast<quintptr>(this)); }
    QRhiTexture* rhiTexture() const override { return texture_.get(); }
    QSize textureSize() const override { return size_; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    void commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch*) override {
        if (!pending_) return;
        pending_ = false;
        if (!native_client_buffer_) {
            native_client_buffer_ = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
                eglGetProcAddress("eglGetNativeClientBufferANDROID"));
            create_image_ =
                reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
            destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR"));
            image_target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        }
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (!context || !native_client_buffer_ || !create_image_ || !destroy_image_ ||
            !image_target_) {
            return;
        }
        // An external texture: Qt makes it, the image gives it its contents.
        if (!texture_) {
            texture_.reset(rhi->newTexture(QRhiTexture::RGBA8, size_, 1, QRhiTexture::ExternalOES));
            if (!texture_->create()) {
                texture_.reset();
                return;
            }
        }
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLint preserved[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        const EGLImageKHR image = create_image_(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                                native_client_buffer_(buffer_), preserved);
        if (image == EGL_NO_IMAGE_KHR) return;
        QOpenGLFunctions* gl = context->functions();
        gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, GLuint(texture_->nativeTexture().object));
        image_target_(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
        gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        // The image before let go of: the texture holds this one now.
        if (image_ != EGL_NO_IMAGE_KHR) destroy_image_(display_, image_);
        image_ = image;
        display_ = display;
    }

private:
    VideoFrame picture_;
    VideoFrame before_;
    AHardwareBuffer* buffer_ = nullptr;  ///< kept by the picture
    QSize size_;
    bool pending_ = false;
    std::unique_ptr<QRhiTexture, DeleteLater> texture_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLImageKHR image_ = EGL_NO_IMAGE_KHR;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC native_client_buffer_ = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
};

class ExternalMaterial final : public QSGMaterial {
public:
    ExternalTexture& texture() { return texture_; }

    QSGMaterialType* type() const override {
        static QSGMaterialType type;
        return &type;
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override {
        return this == other ? 0 : (this < other ? -1 : 1);
    }

private:
    ExternalTexture texture_;
};

/// shaders/video.vert and external.frag (external_gles.frag on OpenGL ES):
/// the uniform block is video.frag's, of which only the matrix and the
/// opacity are read.
class ExternalShader final : public QSGMaterialShader {
public:
    ExternalShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/ratcast/video/shaders/video.vert.qsb"));
        setShaderFileName(FragmentStage,
                          QStringLiteral(":/ratcast/video/shaders/external.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial*, QSGMaterial*) override {
        char* data = state.uniformData()->data();
        if (state.isMatrixDirty()) std::memcpy(data, state.combinedMatrix().constData(), 64);
        if (state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(data + 128, &opacity, 4);
        }
        return true;
    }

    void updateSampledImage(RenderState& state, int, QSGTexture** texture, QSGMaterial* now,
                            QSGMaterial*) override {
        ExternalTexture& picture = static_cast<ExternalMaterial*>(now)->texture();
        picture.commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = &picture;
    }
};

QSGMaterialShader* ExternalMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new ExternalShader;
}

#endif

// ── The node ────────────────────────────────────────────────────────────────

VideoNode::VideoNode() {
    setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4));
    setFlags(OwnsMaterial | OwnsGeometry);
    planes();
}

VideoMaterial* VideoNode::planes() {
    if (!material_) {
        material_ = new VideoMaterial;
        setMaterial(material_);  // the one before deleted
    }
    return material_;
}

void VideoNode::set_picture(VideoFrame picture, bool gles) {
    QRectF source(0, 0, 1, 1);
    bool drawn = false;  // where it is
#ifdef Q_OS_ANDROID
    if (AHardwareBuffer* buffer = android::hardware_buffer_of(picture); buffer && gles) {
        auto* external = dynamic_cast<ExternalMaterial*>(material());
        if (!external) {
            external = new ExternalMaterial;
            setMaterial(external);  // the one before deleted
            material_ = nullptr;
        }
        external->texture().set(picture, buffer);
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buffer, &desc);
        source = QRectF(0, 0, double(picture.width()) / desc.width,
                        double(picture.height()) / desc.height);
        drawn = true;
    }
#else
    (void)gles;
#endif
    if (!drawn) {
        if (picture.gpu()) {  // read out first
            Result<VideoFrame> read = download_frame(picture);
            if (!read) return;
            picture = std::move(*read);
        }
        planes()->set_picture(picture);
    }
    picture_size_ = QSize(int(picture.width()), int(picture.height()));
    if (source != source_) {
        source_ = source;
        placed_ = {};  // the geometry made anew by place()
    }
    markDirty(DirtyMaterial);
}

void VideoNode::place(const QRectF& rect) {
    if (rect == placed_) return;
    placed_ = rect;
    QSGGeometry::updateTexturedRectGeometry(geometry(), rect, source_);
    markDirty(DirtyGeometry);
}

QRectF fitted(QSizeF picture, const QRectF& bounds) {
    if (picture.isEmpty() || bounds.isEmpty()) return {};
    const QSizeF size = picture.scaled(bounds.size(), Qt::KeepAspectRatio);
    return {bounds.x() + (bounds.width() - size.width()) / 2,
            bounds.y() + (bounds.height() - size.height()) / 2, size.width(), size.height()};
}

QRectF snapped(const QRectF& rect, qreal ratio) {
    if (rect.isEmpty() || ratio <= 0) return rect;
    const auto snap = [ratio](qreal at) { return std::round(at * ratio) / ratio; };
    return QRectF(QPointF(snap(rect.left()), snap(rect.top())),
                  QPointF(snap(rect.right()), snap(rect.bottom())));
}

}  // namespace ratcast::app
