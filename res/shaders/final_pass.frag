// In res/shaders/final_pass.frag
#version 410
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform sampler2D bloomTexture;
uniform bool useBloom;
uniform float exposure;

void main()
{
    const float gamma = 2.2;
    vec3 hdrColor = texture(screenTexture, TexCoords).rgb;
    
    if (useBloom) {
        vec3 bloomColor = texture(bloomTexture, TexCoords).rgb;
        hdrColor += bloomColor; // Additive blending
    }
    
    // Tone mapping
    vec3 result = vec3(1.0) - exp(-hdrColor * exposure);
    
    // Gamma correction
    result = pow(result, vec3(1.0 / gamma));
    
    FragColor = vec4(result, 1.0);
}