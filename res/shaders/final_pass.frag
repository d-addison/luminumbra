#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D sceneTexture;
uniform sampler2D bloomTexture;
uniform sampler2D godRaysTexture;
uniform sampler2D dofTexture;
uniform sampler2D depthTexture;
uniform float exposure;
uniform float focusDepth;
uniform float focusScale;

float get_blur(float depth)
{
    return clamp(abs(depth - focusDepth) * focusScale, 0.0, 1.0);
}

void main()
{
    const float gamma = 2.2;
    
    vec3 hdrColor = texture(sceneTexture, TexCoords).rgb;
    vec3 bloomColor = texture(bloomTexture, TexCoords).rgb;
    vec3 godRaysColor = texture(godRaysTexture, TexCoords).rgb;
    vec3 dofColor = texture(dofTexture, TexCoords).rgb;
    
    // Combine scene with bloom and god rays
    hdrColor += bloomColor + godRaysColor;
    
    // Apply DoF
    float blur = get_blur(texture(depthTexture, TexCoords).r);
    hdrColor = mix(hdrColor, dofColor, blur);
    
    // Tone mapping
    vec3 result = vec3(1.0) - exp(-hdrColor * exposure);
    
    // Gamma correction
    result = pow(result, vec3(1.0 / gamma));
    
    FragColor = vec4(result, 1.0);
}
