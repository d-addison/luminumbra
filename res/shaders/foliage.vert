#version 410

// Vertex attributes from C++
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in mat4 a_InstanceMatrix;

// Outputs to Fragment Shader
out vec3 FragPos;
out vec3 Normal;
out vec4 FragPosLightSpace;
out float v_ClipDistance;

// Uniforms
uniform mat4 u_projection;
uniform mat4 u_view;
uniform mat4 lightSpaceMatrix;
uniform vec4 u_ClipPlane;

void main()
{
    // NORMAL PASS: Your original code for rendering from the camera's view.
    // Transform position and normal to world space
    FragPos = vec3(a_InstanceMatrix * vec4(a_Position, 1.0));
    Normal = mat3(transpose(inverse(a_InstanceMatrix))) * a_Normal;
    
    // Transform world position to light space for shadow mapping in the fragment shader
    FragPosLightSpace = lightSpaceMatrix * vec4(FragPos, 1.0);
    
    // Final screen position for the main camera
    gl_Position = u_projection * u_view * vec4(FragPos, 1.0);
    
    // Calculate the clip distance for water reflections/refractions
    v_ClipDistance = dot(vec4(FragPos, 1.0), u_ClipPlane);
}