#version 450 core

// This shader is intentionally left blank.
// We only need to write to the depth buffer during the shadow pass.
void main()
{
    // gl_FragDepth = gl_FragCoord.z;
}