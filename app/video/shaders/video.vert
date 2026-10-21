#version 440

layout(location = 0) in vec4 vertexCoord;
layout(location = 1) in vec2 textureCoord;
layout(location = 0) out vec2 texCoord;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    mat4 yuv_to_rgb;
    float qt_Opacity;
    int nv12;
};

void main() {
    texCoord = textureCoord;
    gl_Position = qt_Matrix * vertexCoord;
}
