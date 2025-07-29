#version 410
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform vec2 lightScreenPos;

uniform float exposure;
uniform float decay;
uniform float density;
uniform float weight;
uniform int samples;

void main()
{
    // The direction from this fragment to the light source
    vec2 delta = lightScreenPos - TexCoords;
    
    // The distance between samples, determined by density
    delta *= density / float(samples);

    // The starting color is from the original texture
    vec3 color = texture(screenTexture, TexCoords).rgb;
    
    // Stores how much light is lost at each step
    float illuminationDecay = 1.0;

    // Loop for the specified number of samples
    for (int i = 0; i < samples; i++)
    {
        // Move the sampling coordinate along the vector to the light
        vec2 sampleCoord = TexCoords + delta * float(i);

        // Stop if we go out of bounds
        if (sampleCoord.x < 0.0 || sampleCoord.x > 1.0 || 
            sampleCoord.y < 0.0 || sampleCoord.y > 1.0) {
            break;
        }

        // Get the color at the sample location
        vec3 sampleColor = texture(screenTexture, sampleCoord).rgb;

        // Apply the weight and decay
        sampleColor *= weight * illuminationDecay;

        // Add to the final color
        color += sampleColor;

        // Reduce the light for the next sample
        illuminationDecay *= decay;
    }

    // Apply the final exposure and output the result
    FragColor = vec4(color * exposure, 1.0);
}