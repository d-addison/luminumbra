#version 450 core
out float FragColor; // Output is a single float (occlusion value)

in vec2 TexCoords;

uniform sampler2D gPosition;      // Now contains VIEW-space positions
uniform sampler2D gNormal;        // Now contains VIEW-space normals
uniform sampler2D u_noiseTexture;

uniform vec3 u_samples[64];
uniform mat4 u_projection;
uniform vec2 u_screenSize; // Dynamic screen size for proper noise scaling
// The u_view uniform is no longer needed

// SSAO settings
const int KERNEL_SIZE = 64;
const float RADIUS = 0.8;
const float BIAS = 0.025;

void main()
{
    // Get fragment data from G-buffer (fragPos and normal are in VIEW SPACE)
    vec3 fragPos = texture(gPosition, TexCoords).rgb;
    vec3 normal = normalize(texture(gNormal, TexCoords).rgb);
    
    // Get random vector to rotate the sampling kernel (using dynamic screen size)
    vec2 noiseScale = u_screenSize / 4.0; // 4x4 noise texture tiling
    vec3 randomVec = normalize(texture(u_noiseTexture, TexCoords * noiseScale).rgb);
    
    // Create TBN matrix from view-space vectors to transform samples from tangent space to view space
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 tbn = mat3(tangent, bitangent, normal);
    
    float occlusion = 0.0;
    for(int i = 0; i < KERNEL_SIZE; ++i)
    {
        // Get sample position in VIEW SPACE
        vec3 samplePos = tbn * u_samples[i]; 
        samplePos = fragPos + samplePos * RADIUS; 
        
        // Project sample position from VIEW SPACE to screen space
        vec4 offset = vec4(samplePos, 1.0);
        offset = u_projection * offset;      // From view to clip-space (u_view is removed)
        offset.xyz /= offset.w;              // Perspective divide
        offset.xyz = offset.xyz * 0.5 + 0.5; // Transform to [0,1] range
        
        // Get depth of geometry at the sample's screen location from the view-space position buffer
        float sampleViewZ = texture(gPosition, offset.xy).z;
        
        // Compare depths in VIEW SPACE. This is the correct way to check for occlusion.
        float rangeCheck = smoothstep(0.0, 1.0, RADIUS / abs(fragPos.z - sampleViewZ));
        occlusion += (sampleViewZ >= samplePos.z + BIAS ? 1.0 : 0.0) * rangeCheck;
    }
    
    // Average and invert the result
    occlusion = 1.0 - (occlusion / KERNEL_SIZE);
    FragColor = pow(occlusion, 2.2); // Apply a power to increase contrast
}