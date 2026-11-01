#version 440

// A picture in Android's GPU memory, drawn where it is: an external texture
// of OpenGL ES's, which turns the buffer's YUV into colour itself. This is
// what other APIs are compiled from; OpenGL ES takes external_gles.frag, the
// same with the external sampler, which only it has.

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    mat4 yuv_to_rgb;
    float qt_Opacity;
    int nv12;
} ubuf;  // named: external_gles.frag names it so too

layout(binding = 1) uniform sampler2D picture;

void main() {
    fragColor = vec4(texture(picture, texCoord).rgb, 1.0) * ubuf.qt_Opacity;
}
