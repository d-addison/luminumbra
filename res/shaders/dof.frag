#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform sampler2D depthTexture;
uniform float focusDepth;
uniform float focusScale;

float get_blur(float depth)
{
    return clamp(abs(depth - focusDepth) * focusScale, 0.0, 1.0);
}

void main()
{
    float depth = texture(depthTexture, TexCoords).r;
    float blur = get_blur(depth);
    
    if (blur < 0.1)
    {
        FragColor = texture(screenTexture, TexCoords);
        return;
    }
    
    vec2 texelSize = 1.0 / textureSize(screenTexture, 0);
    vec3 result = vec3(0.0);
    int samples = 10;
    
    for (int i = 0; i < samples; i++)
    {
        float angle = float(i) / float(samples) * 2.0 * 3.14159;
        vec2 offset = vec2(cos(angle), sin(angle)) * texelSize * blur * 5.0;
        result += texture(screenTexture, TexCoords + offset).rgb;
    }
    
    result /= float(samples);
    
    FragColor = vec4(result, 1.0);
}
