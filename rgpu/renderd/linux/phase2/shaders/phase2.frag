#version 450
layout(set = 0, binding = 0) uniform sampler2D uploadedTexture;
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 sampled = texture(uploadedTexture, inUv);
    outColor = vec4(sampled.rgb, 1.0);
}
