#version 410
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D image;
uniform bool horizontal;

// Optimized weights and offsets for Gaussian blur
float weight[3] = float[] (0.2270270270, 0.3162162162, 0.0702702703);
float offset[3] = float[] (0.0, 1.3846153846, 3.2307692308);

void main()
{
    vec2 tex_offset = 1.0 / textureSize(image, 0);
    vec3 result = texture(image, TexCoords).rgb * weight[0];

    if(horizontal)
    {
        for(int i = 1; i < 3; ++i)
        {
            result += texture(image, TexCoords + vec2(tex_offset.x * offset[i], 0.0)).rgb * weight[i];
            result += texture(image, TexCoords - vec2(tex_offset.x * offset[i], 0.0)).rgb * weight[i];
        }
    }
    else
    {
        for(int i = 1; i < 3; ++i)
        {
            result += texture(image, TexCoords + vec2(0.0, tex_offset.y * offset[i])).rgb * weight[i];
            result += texture(image, TexCoords - vec2(0.0, tex_offset.y * offset[i])).rgb * weight[i];
        }
    }
    FragColor = vec4(result, 1.0);
}
