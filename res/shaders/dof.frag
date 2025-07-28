#version 410
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform sampler2D depthTexture;
uniform float focusDepth;
uniform float focusScale;
uniform float nearPlane;
uniform float farPlane;

// Converts a non-linear depth value from the depth buffer to a linear distance
float LinearizeDepth(float depth)
{
    float z_ndc = depth * 2.0 - 1.0; // Convert [0,1] range to [-1,1] NDC
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - z_ndc * (farPlane - nearPlane));	
}

// Calculates the amount of blur based on the linearized depth
float get_blur(float nonLinearDepth)
{
    float linearDepth = LinearizeDepth(nonLinearDepth);
    return clamp(abs(linearDepth - focusDepth) * focusScale, 0.0, 1.0);
}

void main()
{
    float depth = texture(depthTexture, TexCoords).r;
    float blur = get_blur(depth);
    
    // If the fragment is in focus, don't blur it
    if (blur < 0.01)
    {
        FragColor = texture(screenTexture, TexCoords);
        return;
    }
    
    // Use a simple radial blur for out-of-focus areas
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