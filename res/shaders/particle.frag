#version 410 core
out vec4 FragColor;

in vec2 v_TexCoord;
in vec4 v_Color;

void main()
{
    // Procedural soft circle alpha mask
    float dist_from_center = distance(v_TexCoord, vec2(0.5));
    float alpha = 1.0 - smoothstep(0.45, 0.5, dist_from_center);

    if (alpha < 0.01) {
        discard;
    }

    FragColor = vec4(v_Color.rgb, v_Color.a * alpha);
}