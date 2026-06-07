#version 450 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_sdf_texture;
uniform float u_scanline_y; // Normalized [0, 1], where 0 is the top
uniform float u_time;

// Simple hash function for procedural noise
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main()
{
    // Flip texture coordinates vertically to match OpenGL's standard
    vec2 flipped_coords = vec2(TexCoords.x, 1.0 - TexCoords.y);
    vec3 sdf_color = texture(u_sdf_texture, flipped_coords).rgb;

    // --- Scanline Effect ---
    float scanline_glow = 0.0;
    float scanline_width = 0.015; // How thick the glowing line is
    float scanline_feather = 0.01; // How soft the edges of the glow are

    // Calculate distance from the current fragment to the scanline
    float dist_to_scanline = abs(flipped_coords.y - u_scanline_y);
    
    // Create a glowing effect right at the scanline
    if (dist_to_scanline < scanline_width) {
        scanline_glow = smoothstep(scanline_width, scanline_feather, dist_to_scanline);
    }
    
    // --- Final Composition ---
    vec3 final_color = sdf_color;
    
    // Apply a mask: everything "below" the scanline (higher Y coord) is darkened
    if (flipped_coords.y > u_scanline_y) {
        final_color *= 0.15; // Darken the "scanned" area
        final_color.b *= 1.5; // Add a blue tint to the scanned area
    }
    
    // Add the scanline glow
    final_color += vec3(0.8, 1.0, 0.9) * scanline_glow;

    // Add some subtle noise/static to the whole screen
    float static_noise = hash(flipped_coords + vec2(u_time * 0.1, 0.0)) * 0.05;
    final_color += static_noise;
    
    FragColor = vec4(final_color, 1.0);
}