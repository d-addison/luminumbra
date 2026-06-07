#version 450 core

out vec4 o_frag_color;

in VS_OUT {
    vec3 world_pos;
    vec3 world_normal;
} fs_in;

// --- Textures ---
uniform sampler2D u_opaque_scene_color;
uniform sampler2D u_opaque_depth;
uniform sampler2D u_normal_map;
uniform sampler2D u_foam_texture;
uniform sampler2D u_flow_map; // R,G for flow direction, B for foam mask, A for speed
uniform sampler2D u_caustics_texture; // Generated caustics pattern
uniform sampler2D u_underwater_texture; // Underwater environment map

// --- Scene Uniforms ---
uniform mat4  u_inverse_view;
uniform mat4  u_inverse_projection;
uniform mat4  u_view_projection; // <<< OPTIMIZATION: Pre-combined view and projection matrix
uniform vec3  u_camera_pos;
uniform vec2  u_screen_size;
uniform float u_time;

// --- Lighting & Material ---
uniform vec3 u_sun_direction;
uniform vec3 u_sun_color;
uniform vec3 u_shallow_color;
uniform vec3 u_deep_color;
uniform float u_water_depth_scaler;
uniform float u_reflection_power;

vec3 world_pos_from_depth(float depth, vec2 screen_uv) {
    float z = depth * 2.0 - 1.0;
    vec4 clip_space_pos = vec4(screen_uv * 2.0 - 1.0, z, 1.0);
    vec4 view_space_pos = u_inverse_projection * clip_space_pos;
    view_space_pos /= view_space_pos.w;
    vec4 world_pos = u_inverse_view * view_space_pos;
    return world_pos.xyz;
}

void main()
{
    // --- 1. Prepare Base Vectors & UVs ---
    // Use gl_FragCoord for more direct screen UV calculation
    vec2 screen_uv = gl_FragCoord.xy / u_screen_size;
    vec3 view_dir = normalize(fs_in.world_pos - u_camera_pos);

    // --- 2. Surface Normals & Flow ---
    vec4 flow_data = texture(u_flow_map, fs_in.world_pos.xz * 0.05);
    vec2 flow_vector = (flow_data.rg * 2.0 - 1.0) * flow_data.a;

    vec2 normal_uv1 = fs_in.world_pos.xz * 0.1 + u_time * 0.02 + flow_vector;
    vec2 normal_uv2 = fs_in.world_pos.xz * 0.25 - u_time * 0.035 + flow_vector * 0.5;

    vec3 normal1 = texture(u_normal_map, normal_uv1).rgb * 2.0 - 1.0;
    vec3 normal2 = texture(u_normal_map, normal_uv2).rgb * 2.0 - 1.0;
    vec3 surface_normal = normalize(fs_in.world_normal + normal1 * 0.6 + normal2 * 0.4);

    // --- 3. Depth & Scene Reconstruction ---
    float background_depth_sample = texture(u_opaque_depth, screen_uv).r;
    vec3 background_world_pos = world_pos_from_depth(background_depth_sample, screen_uv);
    float water_depth = max(0.0, fs_in.world_pos.y - background_world_pos.y);

    // --- 4. Refraction ---
    vec2 refraction_offset = surface_normal.xz * (0.05 + flow_data.a * 0.02);
    vec3 refracted_color = texture(u_opaque_scene_color, screen_uv + refraction_offset).rgb;
    
    // --- 5. Reflection (Optimized SSR with early exit) ---
    vec3 reflected_color = u_deep_color; // Fallback color
    vec3 reflection_vector = reflect(view_dir, surface_normal);

    // Reduced steps and adaptive quality based on fresnel
    float fresnel_preview = pow(1.0 - max(0.0, dot(-view_dir, surface_normal)), 2.0);
    const int max_steps = 8; // Reduced from 16
    int num_steps = int(mix(4.0, float(max_steps), fresnel_preview)); // Adaptive quality
    const float step_size = 0.15; // Larger steps
    const float thickness = 0.2;

    // Early exit if reflection vector points down
    if (reflection_vector.y < -0.1) {
        reflected_color = u_deep_color * u_reflection_power;
    } else {
        vec3 ray_pos = fs_in.world_pos;
        for (int i = 0; i < num_steps; ++i) {
            ray_pos += reflection_vector * step_size * (1.0 + float(i) * 0.1); // Progressive step size
            
            vec4 ray_clip_pos = u_view_projection * vec4(ray_pos, 1.0);
            vec2 ray_uv = ray_clip_pos.xy / ray_clip_pos.w * 0.5 + 0.5;
            
            // Early boundary check
            if (any(lessThan(ray_uv, vec2(0.05))) || any(greaterThan(ray_uv, vec2(0.95)))) {
                break;
            }
            
            float scene_depth = texture(u_opaque_depth, ray_uv).r;
            vec3 scene_pos = world_pos_from_depth(scene_depth, ray_uv);
            
            if (ray_pos.y < scene_pos.y && scene_pos.y - ray_pos.y < thickness) {
                reflected_color = texture(u_opaque_scene_color, ray_uv).rgb;
                break;
            }
        }
        reflected_color *= u_reflection_power;
    }

    // --- 6. Fresnel Term ---
    float fresnel = pow(1.0 - max(0.0, dot(-view_dir, surface_normal)), 4.0);
    fresnel = clamp(fresnel, 0.05, 0.95);

    // --- 7. Water Color & Absorption ---
    float absorption_factor = 1.0 - exp(-water_depth * u_water_depth_scaler);
    vec3 water_color = mix(u_shallow_color, u_deep_color, absorption_factor);
    
    // --- 8. Specular Highlight ---
    vec3 half_vector = normalize(u_sun_direction - view_dir);
    float specular_power = pow(max(0.0, dot(surface_normal, half_vector)), 64.0);
    vec3 specular_highlight = u_sun_color * specular_power * fresnel * 2.0;

    // --- 9. Enhanced Caustics ---
    vec3 caustics_color = vec3(0.0);
    if (water_depth > 0.1 && fs_in.world_pos.y > -2.0) { // Only for shallow to medium depth
        vec2 caustics_uv1 = fs_in.world_pos.xz * 0.15 + u_time * 0.02 + flow_vector * 0.5;
        vec2 caustics_uv2 = fs_in.world_pos.xz * 0.08 - u_time * 0.015 + flow_vector * 0.3;
        
        float caustics1 = texture(u_caustics_texture, caustics_uv1).r;
        float caustics2 = texture(u_caustics_texture, caustics_uv2).g;
        
        // Combine caustics with depth falloff
        float caustics_strength = (caustics1 + caustics2 * 0.7) * 0.8;
        float caustics_falloff = exp(-water_depth * 0.3);
        
        caustics_color = vec3(0.6, 0.8, 1.0) * caustics_strength * caustics_falloff;
        caustics_color *= max(0.3, dot(u_sun_direction, vec3(0, -1, 0))); // Sun angle modulation
    }
    
    // --- 10. Enhanced Foam ---
    float depth_foam = smoothstep(0.8, 0.0, water_depth);
    float wave_foam = length(surface_normal.xz) * 2.0; // Foam on wave crests
    float flow_foam = flow_data.b;
    
    float foam_factor = clamp(depth_foam + wave_foam + flow_foam, 0.0, 1.0);
    
    // Multi-layer foam for realism
    vec2 foam_uv1 = fs_in.world_pos.xz * 0.3 + flow_vector * u_time * 0.5;
    vec2 foam_uv2 = fs_in.world_pos.xz * 0.8 - flow_vector * u_time * 0.2;
    
    float foam_tex1 = texture(u_foam_texture, foam_uv1).r;
    float foam_tex2 = texture(u_foam_texture, foam_uv2).g;
    
    vec3 foam_color = vec3(1.0, 1.0, 0.95) * (foam_tex1 * 0.7 + foam_tex2 * 0.3) * foam_factor;
    foam_color *= (1.0 + 0.3 * sin(u_time * 2.0 + foam_uv1.x * 10.0)); // Subtle animation
    
    // --- 11. Underwater Environment ---
    vec3 underwater_color = vec3(0.0);
    if (water_depth > 1.0) {
        // Sample underwater environment for deep water areas
        vec2 underwater_uv = reflect(view_dir, surface_normal).xy * 0.5 + 0.5;
        underwater_color = texture(u_underwater_texture, underwater_uv).rgb * 0.4;
        underwater_color *= exp(-water_depth * 0.1); // Depth attenuation
    }
    
    // --- 12. Enhanced Final Composition ---
    vec3 final_color = mix(refracted_color, reflected_color, fresnel);
    final_color = mix(final_color, water_color, absorption_factor * 0.8);
    final_color += underwater_color; // Add underwater environment
    final_color += caustics_color * 0.6; // Add caustics
    final_color += specular_highlight;
    final_color = mix(final_color, foam_color, foam_factor); // Blend foam on top

    o_frag_color = vec4(final_color, 1.0);
}