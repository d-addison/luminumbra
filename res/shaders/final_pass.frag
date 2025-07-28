#version 410 core
out vec4 FragColor;

in vec2 v_TexCoords;

uniform sampler2D u_screenTexture;
uniform sampler2D u_bloomTexture;
uniform bool u_useBloom;
uniform float u_exposure;

const float GAMMA = 2.2;

void main()
{
    vec3 hdrColor = texture(u_screenTexture, v_TexCoords).rgb;
    
    if (u_useBloom) {
        vec3 bloomColor = texture(u_bloomTexture, v_TexCoords).rgb;
        hdrColor += bloomColor; // Additive blending
    }
    
    // Reinhard tone mapping
    vec3 mappedColor = hdrColor / (hdrColor + vec3(1.0));
    
    // Alternative: Exposure tone mapping
    // vec3 mappedColor = vec3(1.0) - exp(-hdrColor * u_exposure);
    
    // Gamma correction
    vec3 finalColor = pow(mappedColor, vec3(1.0 / GAMMA));
    
    FragColor = vec4(finalColor, 1.0);
}