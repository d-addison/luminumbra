#version 450 core
layout (location = 0) in vec3 aPos;
// per-DRAW chunk world origin (instanced attribute, divisor 1) for the
// glMultiDrawElementsIndirect live-terrain path shared with the G-buffer pass.
// The pool VAO always supplies this attribute; it is only USED when
// u_useInstanceOrigin == 1 (the legacy per-chunk u_model path is otherwise kept).
layout (location = 3) in vec3 aOrigin;

uniform mat4 u_lightSpaceMatrix;
uniform mat4 u_model;
uniform int u_useInstanceOrigin;

void main()
{
    vec3 worldPos = (u_useInstanceOrigin == 1)
        ? (aPos + aOrigin)
: vec3(u_model * vec4(aPos, 1.0));
    gl_Position = u_lightSpaceMatrix * vec4(worldPos, 1.0);
}