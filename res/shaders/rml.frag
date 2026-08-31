#version 450 core
in vec4 fragColor;
in vec2 fragTexCoord;

out vec4 outColor;

uniform sampler2D uTexture;
uniform int uUseTexture;

void main() {
    vec4 texColor = texture(uTexture, fragTexCoord);
    outColor = texColor * fragColor;
}