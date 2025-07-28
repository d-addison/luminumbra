#version 410 core
out vec4 FragColor;

in vec2 v_TexCoords;

uniform sampler2D u_screenTexture;
uniform float u_threshold;
uniform float u_intensity;

void main()
{
    vec3 color = texture(u_screenTexture, v_TexCoords).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    
    if(brightness > u_threshold) {
        FragColor = vec4(color * u_intensity, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}