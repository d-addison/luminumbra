#version 450 core

layout (points) in;
layout (triangle_strip, max_vertices = 4) out;

// Input from vertex shader
in VS_OUT {
    vec3 worldPos;
    vec4 color;
    float size;
    float lifetime;
    float maxLifetime;
    float type;
    float distanceToCamera;
} gs_in[];

// Uniforms
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;
uniform float u_time;

// Output to fragment shader
out GS_OUT {
    vec2 texCoord;
    vec4 color;
    float type;
    float ageFactor;
    vec3 worldPos;
    float distanceToCamera;
} gs_out;

void main() {
    // Early culling for very far or very transparent particles
    if (gs_in[0].distanceToCamera > 200.0 || gs_in[0].color.a < 0.01) {
        return; // Don't generate geometry for invisible particles
    }
    
    vec4 center = gl_in[0].gl_Position;
    float size = gs_in[0].size;
    float ageFactor = gs_in[0].lifetime / gs_in[0].maxLifetime;
    
    // Calculate billboard vectors in view space
    vec3 right = normalize(u_cameraRight) * size;
    vec3 up = normalize(u_cameraUp) * size;
    
    // Different shapes for different particle types
    if (gs_in[0].type == 3.0) { // Crystal particles - diamond shape
        // Create diamond/rhombus shape
        right *= 0.7;
        up *= 1.3;
    } else if (gs_in[0].type == 1.0) { // Ember particles - elongated
        up *= 1.5; // Stretch vertically for ember effect
    }
    
    // Add subtle rotation for some particle types
    float rotation = 0.0;
    if (gs_in[0].type == 2.0) { // Magic particles rotate
        rotation = u_time * 2.0 + gs_in[0].worldPos.x * 0.5;
    } else if (gs_in[0].type == 0.0) { // Sparkles rotate slowly
        rotation = u_time * 0.5 + gs_in[0].worldPos.y * 0.3;
    }
    
    if (rotation != 0.0) {
        float cosR = cos(rotation);
        float sinR = sin(rotation);
        
        vec3 rotatedRight = right * cosR - up * sinR;
        vec3 rotatedUp = right * sinR + up * cosR;
        
        right = rotatedRight;
        up = rotatedUp;
    }
    
    // Generate quad vertices
    // Bottom-left
    gl_Position = center + u_projection * u_view * vec4(-right - up, 0.0);
    gs_out.texCoord = vec2(0.0, 0.0);
    gs_out.color = gs_in[0].color;
    gs_out.type = gs_in[0].type;
    gs_out.ageFactor = ageFactor;
    gs_out.worldPos = gs_in[0].worldPos;
    gs_out.distanceToCamera = gs_in[0].distanceToCamera;
    EmitVertex();
    
    // Bottom-right
    gl_Position = center + u_projection * u_view * vec4(right - up, 0.0);
    gs_out.texCoord = vec2(1.0, 0.0);
    gs_out.color = gs_in[0].color;
    gs_out.type = gs_in[0].type;
    gs_out.ageFactor = ageFactor;
    gs_out.worldPos = gs_in[0].worldPos;
    gs_out.distanceToCamera = gs_in[0].distanceToCamera;
    EmitVertex();
    
    // Top-left
    gl_Position = center + u_projection * u_view * vec4(-right + up, 0.0);
    gs_out.texCoord = vec2(0.0, 1.0);
    gs_out.color = gs_in[0].color;
    gs_out.type = gs_in[0].type;
    gs_out.ageFactor = ageFactor;
    gs_out.worldPos = gs_in[0].worldPos;
    gs_out.distanceToCamera = gs_in[0].distanceToCamera;
    EmitVertex();
    
    // Top-right
    gl_Position = center + u_projection * u_view * vec4(right + up, 0.0);
    gs_out.texCoord = vec2(1.0, 1.0);
    gs_out.color = gs_in[0].color;
    gs_out.type = gs_in[0].type;
    gs_out.ageFactor = ageFactor;
    gs_out.worldPos = gs_in[0].worldPos;
    gs_out.distanceToCamera = gs_in[0].distanceToCamera;
    EmitVertex();
    
    EndPrimitive();
}