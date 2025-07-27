#version 410
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D screenTexture;  // Changed from scene
uniform float threshold;
uniform float intensity;

void main()
{
    vec3 color = texture(screenTexture, TexCoords).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    
    // Apply threshold and intensity
    if(brightness > threshold) {
        FragColor = vec4(color * intensity, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}