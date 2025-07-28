#version 410 core
// Per-quad attributes
layout (location = 0) in vec2 a_VertexPos;
layout (location = 1) in vec2 a_TexCoord;

// Per-instance attributes
layout (location = 2) in vec3 a_WorldPos;
layout (location = 3) in float a_Size;
layout (location = 4) in vec4 a_Color;
layout (location = 5) in float a_Rotation;

out vec2 v_TexCoord;
out vec4 v_Color;

uniform mat4 u_projection;
uniform mat4 u_view;
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;

void main()
{
    v_TexCoord = a_TexCoord;
    v_Color = a_Color;

    // Rotate vertex around its center
    float cos_rot = cos(a_Rotation);
    float sin_rot = sin(a_Rotation);
    vec2 vertex_rotated;
    vertex_rotated.x = a_VertexPos.x * cos_rot - a_VertexPos.y * sin_rot;
    vertex_rotated.y = a_VertexPos.x * sin_rot + a_VertexPos.y * cos_rot;
    
    // Create billboard effect
    vec3 pos_worldspace = a_WorldPos 
                        + u_cameraRight * vertex_rotated.x * a_Size 
                        + u_cameraUp * vertex_rotated.y * a_Size;

    gl_Position = u_projection * u_view * vec4(pos_worldspace, 1.0);
}