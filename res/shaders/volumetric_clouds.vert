#version 410
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoords;

out vec2 v_TexCoords;

void main()
{
    // Pass texture coordinates to the fragment shader
    v_TexCoords = aTexCoords;
    
    // Output the vertex position directly in clip space.
    // THIS LINE IS REQUIRED.
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0); 
}