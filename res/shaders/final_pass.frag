// In res/shaders/final_pass.frag
#version 410
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform sampler2D bloomTexture;
uniform sampler2D godRayTexture;
uniform bool useBloom;
uniform float exposure;
uniform bool useGodRays; 

void main()
{
    const float gamma = 2.2;
    vec3 hdrColor = texture(screenTexture, TexCoords).rgb;
    
    if (useBloom) {
        hdrColor += texture(bloomTexture, TexCoords).rgb;
    }
    if (useGodRays) {
        // Additive blend for the god rays
        hdrColor += texture(godRayTexture, TexCoords).rgb;
    }
    
    // Tone mapping
    vec3 result = vec3(1.0) - exp(-hdrColor * exposure);
    
    // Gamma correction
    result = pow(result, vec3(1.0 / gamma));
    
    FragColor = vec4(result, 1.0);
}