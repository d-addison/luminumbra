#version 410
layout (location = 0) in vec3 a_Pos;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in vec3 a_Color;

out vec3 v_FragPos;
out vec3 v_Normal;
out vec3 v_Color;
out float v_ClipDistance;
out vec4 v_FragPosLightSpace;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec4 u_ClipPlane;
uniform mat4 u_lightSpaceMatrix;

void main()
{
    v_FragPos = vec3(u_model * vec4(a_Pos, 1.0));
    v_Normal = mat3(transpose(inverse(u_model))) * a_Normal;
    v_Color = a_Color;

    gl_Position = u_projection * u_view * vec4(v_FragPos, 1.0);

    v_FragPosLightSpace = u_lightSpaceMatrix * vec4(v_FragPos, 1.0);

    gl_ClipDistance[0] = dot(vec4(v_FragPos, 1.0), u_ClipPlane);
}