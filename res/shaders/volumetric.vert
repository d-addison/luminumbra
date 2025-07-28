#version 410
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoords;
layout (location = 2) in vec3 a_WorldPos;
layout (location = 3) in float a_Size;
layout (location = 4) in vec4 a_Color;
layout (location = 5) in float a_Rotation;
layout (location = 6) in float a_Layer;  // New: layer depth

out vec2 TexCoords;
out vec3 WorldPos;
out vec4 Color;
out float Layer;

uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;
uniform mat4 u_projection;
uniform vec3 u_viewPos;

void main() {
    vec2 pos = a_Position;
    float c = cos(a_Rotation);
    float s = sin(a_Rotation);
    pos = vec2(
        pos.x * c - pos.y * s,
        pos.x * s + pos.y * c
    );
    
    vec3 centerWorldPos = a_WorldPos;
    vec3 vertPos = centerWorldPos 
        + u_cameraRight * pos.x * a_Size
        + u_cameraUp * pos.y * a_Size
        + normalize(centerWorldPos - u_viewPos) * a_Layer;  // Offset by layer depth

    gl_Position = u_projection * vec4(vertPos - u_viewPos, 1.0);
    
    TexCoords = a_TexCoords;
    WorldPos = vertPos;
    Color = a_Color;
    Layer = a_Layer;
}