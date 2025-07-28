#version 410 core
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in mat4 a_InstanceMatrix; // This will now be the instance's LOCAL transform

out vec3 v_FragPos;
out vec3 v_Normal;
out vec4 v_FragPosLightSpace;

uniform mat4 u_view;
uniform mat4 u_projection;
uniform mat4 u_lightSpaceMatrix;
uniform vec4 u_clipPlane;
uniform mat4 u_model; // NEW: The chunk's model matrix

void main()
{
    // Combine the chunk's model matrix with the instance's local transform
    mat4 finalModelMatrix = u_model * a_InstanceMatrix;

    vec4 worldPos = finalModelMatrix * vec4(a_Position, 1.0);
    v_FragPos = worldPos.xyz;

    // Correctly calculate the normal using the final combined matrix
    v_Normal = mat3(transpose(inverse(finalModelMatrix))) * a_Normal;

    v_FragPosLightSpace = u_lightSpaceMatrix * worldPos;

    gl_Position = u_projection * u_view * worldPos;

    gl_ClipDistance[0] = dot(worldPos, u_clipPlane);
}