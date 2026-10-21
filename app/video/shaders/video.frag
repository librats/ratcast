#version 440

// A 4:2:0 picture's samples into colour: Y from one texture, U and V from two
// (I420) or from one's two channels (NV12), through the picture's matrix.

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    mat4 yuv_to_rgb;
    float qt_Opacity;
    int nv12;
};

layout(binding = 1) uniform sampler2D yPlane;
layout(binding = 2) uniform sampler2D uPlane;
layout(binding = 3) uniform sampler2D vPlane;

void main() {
    float y = texture(yPlane, texCoord).r;
    vec2 uv = nv12 != 0 ? texture(uPlane, texCoord).rg
                        : vec2(texture(uPlane, texCoord).r, texture(vPlane, texCoord).r);
    vec3 rgb = clamp((yuv_to_rgb * vec4(y, uv, 1.0)).rgb, 0.0, 1.0);
    fragColor = vec4(rgb, 1.0) * qt_Opacity;
}
