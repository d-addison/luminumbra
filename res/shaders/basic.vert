#version 410
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;

out vec3 FragPos;
out vec3 Normal;
out vec3 Color;
out vec4 FragPosLightSpace;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 lightSpaceMatrix;

void main()
{
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    Color = aColor;
    
    // This line is the critical fix.
    gl_Position = projection * view * vec4(FragPos, 1.0);
    
    // This part is for shadow mapping, which you can re-enable later
    FragPosLightSpace = lightSpaceMatrix * vec4(FragPos, 1.0);
}