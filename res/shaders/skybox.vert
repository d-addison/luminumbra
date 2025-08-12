#version 450 core
layout (location = 0) in vec3 aPos;

out vec3 WorldPos;

uniform mat4 projection;
uniform mat4 view; // This is the rotation-only matrix from the C++ code

void main()
{
    WorldPos = aPos;
    
    // Calculate the clip-space position as usual.
    vec4 pos = projection * view * vec4(aPos, 1.0);

    // ===================== FIX IS HERE =====================
    // Instead of the .xyww swizzle, we explicitly set the Z component
    // to match the W component. This forces the depth to be 1.0 (the far plane)
    // after the perspective divide, in a more robust way.
    gl_Position = pos;
    gl_Position.z = gl_Position.w;
    // ====================== FIX ENDS HERE ======================
}