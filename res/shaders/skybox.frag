#version 410 core
layout (location = 0) in vec3 a_Position;

out vec3 v_TexCoords;

uniform mat4 u_projection;
uniform mat4 u_view;

void main()
{
    v_TexCoords = a_Position;
    // Remove translation from the view matrix to make the skybox follow the camera
    mat4 viewNoTranslation = mat4(mat3(u_view));
    vec4 pos = u_projection * viewNoTranslation * vec4(a_Position, 1.0);
    // Use .xyww to ensure depth is always 1.0 (far plane)
    gl_Position = pos.xyww;
}