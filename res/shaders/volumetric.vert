#version 410
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoords;
layout (location = 2) in vec3 aWorldPos;
layout (location = 3) in float aSize;
layout (location = 4) in vec4 aColor;
layout (location = 5) in float aRotation;
layout (location = 6) in float aLayer;  // New: layer depth

out vec2 TexCoords;
out vec3 WorldPos;
out vec4 Color;
out float Layer;

uniform vec3 u_CameraRight;
uniform vec3 u_CameraUp;
uniform mat4 u_Projection;
uniform vec3 u_ViewPos;

void main() {
    vec2 pos = aPos;
    float c = cos(aRotation);
    float s = sin(aRotation);
    pos = vec2(
        pos.x * c - pos.y * s,
        pos.x * s + pos.y * c
    );
    
    vec3 centerWorldPos = aWorldPos;
    vec3 vertPos = centerWorldPos 
        + u_CameraRight * pos.x * aSize
        + u_CameraUp * pos.y * aSize
        + normalize(centerWorldPos - u_ViewPos) * aLayer;  // Offset by layer depth

    gl_Position = u_Projection * vec4(vertPos - u_ViewPos, 1.0);
    
    TexCoords = aTexCoords;
    WorldPos = vertPos;
    Color = aColor;
    Layer = aLayer;
}