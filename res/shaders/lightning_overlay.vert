#version 450 core
// lightning light-pulse + bolt overlay. Full-screen quad; reuses the
// lighting pass's screen-quad attribute layout (pos + uv). Drawn AFTER the skybox
// so the full-scene flash + the screen-space bolt composite over BOTH the lit
// terrain and the sky (the lighting pass shades only G-buffer geometry, and the
// skybox overwrites sky pixels, so the pulse must land here to be full-scene).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoords;

out vec2 TexCoords;

void main() {
    TexCoords = aTexCoords;
    gl_Position = vec4(aPos, 1.0);
}
