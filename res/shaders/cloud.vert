#version 410
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoords;

// The model matrix is now an instanced vertex attribute.
// This allows each instance (cloud) to have its own transformation.
layout (location = 2) in mat4 aInstanceMatrix;

out vec2 TexCoords;

uniform mat4 view;
uniform mat4 projection;

void main()
{
    TexCoords = aTexCoords;
    // Use the instanced matrix attribute for the transformation.
    gl_Position = projection * view * aInstanceMatrix * vec4(aPos, 1.0);
}