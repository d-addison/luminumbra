#version 410
layout (location = 0) in vec3 a_Pos;
layout (location = 1) in vec3 a_Normal;

out vec3 v_FragPos;
out vec3 v_Normal;
out float v_ClipDistance;
out vec4 v_FragPosLightSpace;

// Camera matrices
uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec4 u_ClipPlane;

// Shadow matrices
uniform mat4 u_lightSpaceMatrix;

void main()
{
    // NORMAL PASS: Calculate all varyings for the fragment shader.
    v_FragPos = vec3(u_model * vec4(a_Pos, 1.0));
    v_Normal = mat3(transpose(inverse(u_model))) * a_Normal;
    
    // Needed for shadow lookup
    v_FragPosLightSpace = u_lightSpaceMatrix * vec4(v_FragPos, 1.0);
    
    // Needed for water clipping
    gl_ClipDistance[0] = dot(vec4(v_FragPos, 1.0), u_ClipPlane);
    
    // Final screen position
    gl_Position = u_projection * u_view * vec4(v_FragPos, 1.0);
}