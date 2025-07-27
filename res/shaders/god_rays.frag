#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform vec2 lightScreenPos;
uniform float decay;
uniform float exposure;
uniform float density;
uniform float weight;
uniform int samples;

void main()
{
    vec2 delta = TexCoords - lightScreenPos;
    delta *= 1.0 / float(samples) * density;
    float illuminationDecay = 1.0;
    
    vec3 color = texture(screenTexture, TexCoords).rgb;
    
    for(int i=0; i < samples; i++)
    {
        TexCoords -= delta;
        vec3 sampleColor = texture(screenTexture, TexCoords).rgb;
        sampleColor *= illuminationDecay * weight;
        color += sampleColor;
        illuminationDecay *= decay;
    }
    
    FragColor = vec4(color * exposure, 1.0);
}
