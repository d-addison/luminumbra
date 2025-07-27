#version 410
out vec4 FragColor;

// Input texture coordinates from the vertex shader
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
    // A local variable to march along the ray from the fragment to the light source
    vec2 sampleCoord = TexCoords;

    // Calculate the direction and step size for sampling
    vec2 delta = TexCoords - lightScreenPos;
    delta *= 1.0 / float(samples) * density;

    float illuminationDecay = 1.0;

    // Start with the color at the fragment's original position
    vec3 color = texture(screenTexture, sampleCoord).rgb;

    // Loop 'samples' times, stepping towards the light source
    for(int i = 0; i < samples; i++)
    {
        // Move the sampling coordinate for the next sample
        sampleCoord -= delta;

        // Sample the texture at the new coordinate
        vec3 sampleColor = texture(screenTexture, sampleCoord).rgb;

        // Attenuate the sample's contribution by the decay factor
        sampleColor *= illuminationDecay * weight;

        // Add to the total color
        color += sampleColor;

        // The further we are from the fragment, the less light contributes
        illuminationDecay *= decay;
    }

    FragColor = vec4(color * exposure, 1.0);
}