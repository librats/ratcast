#version 100
#extension GL_OES_EGL_image_external : require

// external.frag for OpenGL ES, as qsb writes it for GLSL ES 100 but for the
// sampler: an external texture's (Android's), which turns YUV into colour.

precision highp float;
precision highp int;

struct buf
{
    mat4 qt_Matrix;
    mat4 yuv_to_rgb;
    float qt_Opacity;
    int nv12;
};

uniform buf ubuf;

uniform highp samplerExternalOES picture;

varying vec2 texCoord;

void main()
{
    gl_FragData[0] = vec4(texture2D(picture, texCoord).xyz, 1.0) * ubuf.qt_Opacity;
}
