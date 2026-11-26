#version 450 core
in vec2 TexCoords;
out vec4 FragColor;
uniform sampler2D u_scene;
uniform sampler3D u_lookLut;

void main()
{
    // Clamp to source texel centres so internal-to-destination bilinear sampling
    // matches GL_LINEAR blits even when the source texture uses repeat wrapping.
    vec2 halfTexel = 0.5 / vec2(textureSize(u_scene, 0));
    vec4 scene = texture(u_scene, clamp(TexCoords, halfTexel, 1.0 - halfTexel));
    vec3 size = vec3(textureSize(u_lookLut, 0));
    vec3 uvw = clamp(scene.rgb, 0.0, 1.0) * (size - 1.0) / size + 0.5 / size;
    FragColor = vec4(texture(u_lookLut, uvw).rgb, scene.a);
}
