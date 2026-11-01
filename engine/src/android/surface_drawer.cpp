// Pictures drawn into a window through OpenGL ES: see surface_drawer.h.

#include "android/surface_drawer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// After GLES 3's own: the extensions' names, GL_TEXTURE_EXTERNAL_OES.
#include <GLES2/gl2ext.h>

#include <array>
#include <utility>

#include "android/hardware_picture.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::android {
namespace {

// A strip of two triangles over the window, the picture's top at the top:
// GL's rows go up, a picture's down.
constexpr const char* kVertexShader = R"(#version 300 es
uniform vec2 extent;
out vec2 at;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    at = vec2(corner.x, 1.0 - corner.y) * extent;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// A buffer's own RGBA, through the EGLImage of it.
constexpr const char* kBufferShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES picture;
in vec2 at;
out vec4 colour;
void main() {
    colour = vec4(texture(picture, at).rgb, 1.0);
}
)";

// A 4:2:0 picture's samples into colour, as the app's view does
// (app/video/shaders/video.frag): each of R, G and B a row of `to_rgb`.
constexpr const char* kPlanesShader = R"(#version 300 es
precision highp float;
uniform sampler2D y_plane;
uniform sampler2D u_plane;
uniform sampler2D v_plane;
uniform bool nv12;
uniform vec4 to_rgb[3];
in vec2 at;
out vec4 colour;
void main() {
    vec2 uv = nv12 ? texture(u_plane, at).rg
                   : vec2(texture(u_plane, at).r, texture(v_plane, at).r);
    vec4 yuv = vec4(texture(y_plane, at).r, uv, 1.0);
    vec3 rgb = vec3(dot(to_rgb[0], yuv), dot(to_rgb[1], yuv), dot(to_rgb[2], yuv));
    colour = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

GLuint compile(GLenum stage, const char* source) {
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) return shader;
    std::array<char, 512> log{};
    glGetShaderInfoLog(shader, GLsizei(log.size()), nullptr, log.data());
    LOG_WARN("engine", "a shader does not compile: " << log.data());
    glDeleteShader(shader);
    return 0;
}

GLuint link(const char* fragment_source) {
    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program = 0;
    if (vertex && fragment) {
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return program;
}

/// One plane's texture, made anew only when its size changes.
struct PlaneTexture {
    GLuint id = 0;
    GLsizei width = 0;
    GLsizei height = 0;
    GLenum format = GL_RED;

    /// Plane `plane`, of `width` x `height` samples of `channels` bytes,
    /// on texture unit `unit`.
    void fill(GLenum unit, const VideoFrame::Plane& plane, GLsizei w, GLsizei h, int channels) {
        const GLenum fmt = channels == 2 ? GL_RG : GL_RED;
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(plane.stride / size_t(channels)));
        if (w != width || h != height || fmt != format) {
            glTexImage2D(GL_TEXTURE_2D, 0, channels == 2 ? GL_RG8 : GL_R8, w, h, 0, fmt,
                         GL_UNSIGNED_BYTE, plane.data);
            width = w;
            height = h;
            format = fmt;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, plane.data);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
};

void set_sampling(GLenum target, GLuint texture) {
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

class EglDrawer final : public SurfaceDrawer {
public:
    explicit EglDrawer(ANativeWindow* window) : window_(window) { ANativeWindow_acquire(window_); }

    ~EglDrawer() override {
        if (context_ != EGL_NO_CONTEXT) {
            if (const Current current(*this); current) {
                glDeleteProgram(buffer_program_);
                glDeleteProgram(planes_program_);
                glDeleteTextures(1, &external_);
                for (PlaneTexture& plane : planes_) glDeleteTextures(1, &plane.id);
            }
            eglDestroyContext(display_, context_);
        }
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        ANativeWindow_release(window_);
    }

    EglDrawer(const EglDrawer&) = delete;
    EglDrawer& operator=(const EglDrawer&) = delete;

    Result<void> open() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr))
            return fail(Errc::Unavailable);
        const EGLint wanted[] = {EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT_KHR,
                                 EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT,
                                 EGL_RECORDABLE_ANDROID,
                                 EGL_TRUE,
                                 EGL_NONE};
        EGLConfig config = nullptr;
        EGLint found = 0;
        if (!eglChooseConfig(display_, wanted, &config, 1, &found) || found < 1)
            return fail(Errc::Unavailable);
        const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, version);
        surface_ = eglCreateWindowSurface(display_, config, window_, nullptr);
        native_client_buffer_ = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        create_image_ =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image_ =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        image_target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        presentation_time_ = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(
            eglGetProcAddress("eglPresentationTimeANDROID"));
        if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE || !native_client_buffer_ ||
            !create_image_ || !destroy_image_ || !image_target_ || !presentation_time_) {
            return fail(Errc::Unavailable);
        }

        const Current current(*this);
        if (!current) return fail(Errc::Unavailable);
        buffer_program_ = link(kBufferShader);
        planes_program_ = link(kPlanesShader);
        if (!buffer_program_ || !planes_program_) return fail(Errc::Unavailable);
        glGenTextures(1, &external_);
        set_sampling(GL_TEXTURE_EXTERNAL_OES, external_);
        for (PlaneTexture& plane : planes_) {
            glGenTextures(1, &plane.id);
            set_sampling(GL_TEXTURE_2D, plane.id);
        }
        glUseProgram(buffer_program_);
        glUniform1i(glGetUniformLocation(buffer_program_, "picture"), 0);
        buffer_extent_ = glGetUniformLocation(buffer_program_, "extent");
        glUseProgram(planes_program_);
        glUniform1i(glGetUniformLocation(planes_program_, "y_plane"), 0);
        glUniform1i(glGetUniformLocation(planes_program_, "u_plane"), 1);
        glUniform1i(glGetUniformLocation(planes_program_, "v_plane"), 2);
        glUniform2f(glGetUniformLocation(planes_program_, "extent"), 1, 1);
        nv12_ = glGetUniformLocation(planes_program_, "nv12");
        to_rgb_ = glGetUniformLocation(planes_program_, "to_rgb");
        return glGetError() == GL_NO_ERROR ? Result<void>{} : fail(Errc::Unavailable);
    }

    Result<void> draw(const VideoFrame& picture, int64_t time_ns) override {
        AHardwareBuffer* buffer = hardware_buffer_of(picture);
        const bool planes = !picture.gpu() && (picture.format() == PixelFormat::NV12 ||
                                               picture.format() == PixelFormat::I420);
        if (picture.empty() || (!buffer && !planes)) return fail(Errc::InvalidArgument);

        const Current current(*this);
        if (!current) return fail(Errc::Internal);
        EGLint width = 0;
        EGLint height = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &width);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &height);
        glViewport(0, 0, width, height);

        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        if (buffer) {
            const EGLint preserved[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            image = create_image_(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                  native_client_buffer_(buffer), preserved);
            if (image == EGL_NO_IMAGE_KHR) return fail(Errc::Internal);
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(buffer, &desc);
            glUseProgram(buffer_program_);
            glUniform2f(buffer_extent_, float(picture.width()) / float(desc.width),
                        float(picture.height()) / float(desc.height));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_);
            image_target_(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
        } else {
            fill(picture);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const bool drawn = glGetError() == GL_NO_ERROR;
        presentation_time_(display_, surface_, time_ns);
        const bool swapped = drawn && eglSwapBuffers(display_, surface_);
        if (image != EGL_NO_IMAGE_KHR) destroy_image_(display_, image);
        if (!swapped) {
            LOG_WARN("engine", "a picture was not drawn into the encoder's surface: "
                                   << (drawn ? eglGetError() : EGLint(glGetError())));
            return fail(Errc::Internal);
        }
        return {};
    }

private:
    /// Ours current on this thread for as long as it lives, whatever was
    /// before current again after.
    class Current {
    public:
        explicit Current(const EglDrawer& drawer)
            : drawer_(drawer),
              display_(eglGetCurrentDisplay()),
              draw_(eglGetCurrentSurface(EGL_DRAW)),
              read_(eglGetCurrentSurface(EGL_READ)),
              context_(eglGetCurrentContext()),
              made_(eglMakeCurrent(drawer.display_, drawer.surface_, drawer.surface_,
                                   drawer.context_)) {}
        ~Current() {
            if (display_ != EGL_NO_DISPLAY) {
                eglMakeCurrent(display_, draw_, read_, context_);
            } else {
                eglMakeCurrent(drawer_.display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        }
        Current(const Current&) = delete;
        Current& operator=(const Current&) = delete;
        explicit operator bool() const noexcept { return made_; }

    private:
        const EglDrawer& drawer_;
        const EGLDisplay display_;
        const EGLSurface draw_;
        const EGLSurface read_;
        const EGLContext context_;
        const bool made_;
    };

    /// A picture in memory, up as its planes, the program set to turn them
    /// into colour.
    void fill(const VideoFrame& picture) {
        const bool nv12 = picture.format() == PixelFormat::NV12;
        const auto width = GLsizei(picture.width());
        const auto height = GLsizei(picture.height());
        glUseProgram(planes_program_);
        planes_[0].fill(GL_TEXTURE0, picture.plane(0), width, height, 1);
        planes_[1].fill(GL_TEXTURE1, picture.plane(1), width / 2, height / 2, nv12 ? 2 : 1);
        if (!nv12) planes_[2].fill(GL_TEXTURE2, picture.plane(2), width / 2, height / 2, 1);
        glUniform1i(nv12_, nv12 ? 1 : 0);
        const YuvToRgb rows = rgb_from_yuv(picture.color_space());
        glUniform4fv(to_rgb_, 3, rows[0].data());
    }

    ANativeWindow* const window_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC native_client_buffer_ = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time_ = nullptr;

    GLuint buffer_program_ = 0;
    GLint buffer_extent_ = -1;
    GLuint external_ = 0;
    GLuint planes_program_ = 0;
    GLint nv12_ = -1;
    GLint to_rgb_ = -1;
    std::array<PlaneTexture, 3> planes_;
};

}  // namespace

Result<std::unique_ptr<SurfaceDrawer>> SurfaceDrawer::open(ANativeWindow* window) {
    if (!window) return fail(Errc::InvalidArgument);
    auto drawer = std::make_unique<EglDrawer>(window);
    if (Result<void> opened = drawer->open(); !opened) {
        LOG_DEBUG("engine", "no OpenGL ES 3 to draw into the window with: " << eglGetError());
        return fail(opened.error());
    }
    return std::unique_ptr<SurfaceDrawer>(std::move(drawer));
}

}  // namespace ratcast::android
