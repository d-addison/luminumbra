#version 450 core
out vec4 FragColor;

in VS_OUT {
    vec4 color;
    vec3 worldPos;
} fs_in;

uniform vec3 u_viewPos;
uniform float u_time;

void main()
{
    vec3 viewDir = normalize(u_viewPos - fs_in.worldPos);
    vec3 normal = normalize(cross(dFdx(fs_in.worldPos), dFdy(fs_in.worldPos))); // Flat normal for cubes

    // --- Fresnel Effect for a holographic glow on the edges ---
    float fresnel = pow(1.0 - abs(dot(viewDir, normal)), 3.0);
    fresnel = smoothstep(0.0, 1.0, fresnel);

    // --- Pulse Effect for "active" chunks ---
    // The alpha component of the instance color will control the pulse intensity
    float pulse = sin(u_time * 8.0 + fs_in.worldPos.y) * 0.5 + 0.5;
    pulse *= fs_in.color.a; // Use alpha from instance data as a mask/intensity

    vec3 base_color = fs_in.color.rgb;
    vec3 final_color = base_color + (base_color * 2.0 * fresnel) + (base_color * pulse);
    
    // Use the RGB part of the instance color for opacity
    FragColor = vec4(final_color, fs_in.color.r + fs_in.color.g + fs_in.color.b);
}