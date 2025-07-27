#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D scene;
uniform sampler2D bloomBlur;
uniform float exposure;

void main()
{
    const float gamma = 2.2;
    vec3 hdrColor = texture(scene, TexCoords).rgb;
    vec3 bloomColor = texture(bloomBlur, TexCoords).rgb;
    hdrColor += bloomColor; // Additive blending
    
    // ACES tone mapping approximation
    hdrColor *= exposure;
    hdrColor = (hdrColor * (2.51f * hdrColor + 0.03f)) / (hdrColor * (2.43f * hdrColor + 0.59f) + 0.14f);
    
    // Gamma correction
    hdrColor = pow(hdrColor, vec3(1.0 / gamma));
    
    FragColor = vec4(hdrColor, 1.0);
}
