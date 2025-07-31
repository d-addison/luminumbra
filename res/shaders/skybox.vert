// In res/shaders/skybox.vert

#version 410
layout (location = 0) in vec3 aPos;

out vec3 TexCoords;

uniform mat4 projection;
uniform mat4 view;

void main()
{
    TexCoords = aPos;
    
    // Convert view matrix to mat3 to discard translation, then back to mat4
    mat4 viewNoTranslation = mat4(mat3(view));
    
    // Use the translation-less view matrix for the calculation
    vec4 pos = projection * viewNoTranslation * vec4(aPos, 1.0);
    
    // Set z = w to force depth to 1.0, ensuring it's drawn behind everything
    gl_Position = pos.xyww;
}