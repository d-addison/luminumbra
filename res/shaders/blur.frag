#version 410 core
out vec4 FragColor;

in vec2 v_TexCoords;

uniform sampler2D u_image;
uniform bool u_horizontal;

// Optimized weights and offsets for a 3-tap Gaussian blur
const float weight[3] = float[] (0.227027, 0.316216, 0.070270);
const float offset[3] = float[] (0.0, 1.384615, 3.230769);

void main()
{
    vec2 tex_offset = 1.0 / textureSize(u_image, 0);
    vec3 result = texture(u_image, v_TexCoords).rgb * weight[0];

    if(u_horizontal) {
        for(int i = 1; i < 3; ++i) {
            result += texture(u_image, v_TexCoords + vec2(tex_offset.x * offset[i], 0.0)).rgb * weight[i];
            result += texture(u_image, v_TexCoords - vec2(tex_offset.x * offset[i], 0.0)).rgb * weight[i];
        }
    } else {
        for(int i = 1; i < 3; ++i) {
            result += texture(u_image, v_TexCoords + vec2(0.0, tex_offset.y * offset[i])).rgb * weight[i];
            result += texture(u_image, v_TexCoords - vec2(0.0, tex_offset.y * offset[i])).rgb * weight[i];
        }
    }
    FragColor = vec4(result, 1.0);
}