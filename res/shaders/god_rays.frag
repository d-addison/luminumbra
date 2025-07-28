#version 410 core
out vec4 FragColor;

in vec2 v_TexCoords;

uniform sampler2D u_screenTexture;
uniform vec2 u_lightScreenPos;
uniform float u_density;

// Using constants as the C++ code doesn't set them.
// These could be converted to uniforms for more control.
const float DECAY = 0.97;
const float EXPOSURE = 0.25;
const float WEIGHT = 0.4;
const int SAMPLES = 100;

void main()
{
    vec2 delta = v_TexCoords - u_lightScreenPos;
    delta *= 1.0 / float(SAMPLES) * u_density;

    float illuminationDecay = 1.0;
    vec3 color = texture(u_screenTexture, v_TexCoords).rgb;
    vec2 sampleCoord = v_TexCoords;

    for(int i = 0; i < SAMPLES; i++)
    {
        sampleCoord -= delta;
        vec3 sampleColor = texture(u_screenTexture, sampleCoord).rgb;
        sampleColor *= illuminationDecay * WEIGHT;
        color += sampleColor;
        illuminationDecay *= DECAY;
    }

    FragColor = vec4(color * EXPOSURE, 1.0);
}