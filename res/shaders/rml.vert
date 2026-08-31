#version 450 core
layout (location = 0) in vec2 inPosition;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inTexCoord;

out vec4 fragColor;
out vec2 fragTexCoord;

uniform mat4 projection;
uniform mat4 model; // Changed from 'translation' to 'model' for clarity

void main() {
    fragColor = inColor.bgra; // RmlUi vertex colors are BGRA
    fragTexCoord = inTexCoord;
    gl_Position = projection * model * vec4(inPosition, 0.0, 1.0);
}