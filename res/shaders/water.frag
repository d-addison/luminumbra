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
uniform vec3 u_sky_color; // approximate sky reflection color (time-of-day driven)

const float FAR_DEPTH_THRESHOLD = 0.9999;

bool has_opaque_depth(float depth)
{
    return depth < FAR_DEPTH_THRESHOLD;
}

// --- Procedural ripple normal -------------------------------------------
// The bound u_normal_map is the engine's FLAT fallback (tangent +Z), so the
// texture bump below contributes no XY tilt and the surface normal collapses
// to the interpolated per-vertex world normal. Across the low-poly water mesh
// that interpolation reads as hard-shaded triangle facets with visible seams
// (the de-facet defect). To give the surface genuine per-pixel detail we add
// an analytic ripple field: a sum of gently animated directional  whose
// horizontal gradient tilts the world normal. Because it is evaluated per
// fragment from world position + time, the lighting/reflection now varies
// smoothly and continuously across each triangle, breaking up the facets.
//
// Returns the XZ-plane gradient (slope.x, slope.z) of the height field; the
// caller folds it into the world normal. Kept subtle (small amplitudes,
// low frequencies) so the water reads as gentle ripples, not chop.
vec2 ripple_gradient(vec2 p, float t, vec2 flow)
{
    // Drift the sample point along the flow so ripples travel with the water.
    p += flow * t * 0.35;

    // A handful of overlapping directional  at increasing frequency and
    // decreasing amplitude (a small "ocean" spectrum). For wave
    //   h = A * sin(dot(dir, p) * freq + speed * t)
    // the horizontal gradient is
    //   dh = A * freq * cos(...) * dir
    // which is exactly the surface slope we add to the normal.
    vec2 grad = vec2(0.0);

    // dir, freq, amp, speed per octave. Directions are spread around the
    // compass so the interference pattern is non-repeating and natural.
    // 1
    vec2  d1 = normalize(vec2( 0.80,  0.60));
    float f1 = 0.55, a1 = 0.085, s1 = 0.9;
    grad += a1 * f1 * cos(dot(d1, p) * f1 + t * s1) * d1;
    // 2
    vec2  d2 = normalize(vec2(-0.60,  0.80));
    float f2 = 0.95, a2 = 0.060, s2 = 1.25;
    grad += a2 * f2 * cos(dot(d2, p) * f2 + t * s2) * d2;
    // 3
    vec2  d3 = normalize(vec2( 0.20, -0.98));
    float f3 = 1.70, a3 = 0.038, s3 = 1.7;
    grad += a3 * f3 * cos(dot(d3, p) * f3 + t * s3) * d3;
    // 4 (fine detail)
    vec2  d4 = normalize(vec2(-0.95, -0.30));
    float f4 = 3.10, a4 = 0.022, s4 = 2.3;
    grad += a4 * f4 * cos(dot(d4, p) * f4 + t * s4) * d4;

    return grad;
}

// Smooth 2D value noise (bilinear-interpolated hash) — replaces the old blocky
// floor-cell foam hash that read as a hard pixel grid on the surface.
float foam_hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float foam_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // smoothstep interpolation
    float a = foam_hash(i);
    float b = foam_hash(i + vec2(1.0, 0.0));
    float c = foam_hash(i + vec2(0.0, 1.0));
    float d = foam_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// Two octaves of smoothly-drifting noise for broken-but-soft foam.
float foam_fbm(vec2 p, float t, vec2 flow) {
    vec2 q = p + flow * t;
    return 0.6 * foam_noise(q) + 0.4 * foam_noise(q * 2.3 + t * 0.5);
}

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

    // --- 1b. Time-of-day light factor (kills the night "emissive cyan" glow) ---
    // The water body colour, caustics and the minimum tint floor below used to
    // be constants, so at night the lake stayed fully bright teal while the sky
    // and terrain went dark -- it read as a self-lit emissive material and was
    // the brightest thing in a night frame. u_sky_color is the time-of-day
    // driven sky tint (night ~(0.02,0.04,0.10) luminance ~0.05, day
    // ~(0.45,0.68,0.95) luminance ~0.68); u_sun_direction.y is the sun height.
    // We fold both into a 0..1 daylight term and use it to dim the water body
    // toward a dark night tint so the surface only carries sky/moon reflection
    // at night, while daytime water keeps its natural blue.
    float sky_luma = dot(u_sky_color, vec3(0.2126, 0.7152, 0.0722));
    // Map the sky luminance (~0.05 night.. ~0.68 day) onto 0..1.
    float sky_light = smoothstep(0.06, 0.45, sky_luma);
    // Sun-above-horizon contribution. u_sun_direction is the light TRAVEL
    // direction (from the sun toward the surface), so the sun is overhead when
    // the light is travelling downward (direction.y < 0). The sun height above
    // the horizon is therefore -u_sun_direction.y: +1 at noon (light straight
    // down), -1 at night (light from below). The earlier version used
    // +u_sun_direction.y, which is +1 at night -- that inverted sign is what
    // kept the water fully lit (glowing) in night frames.
    float sun_height = smoothstep(-0.15, 0.25, -u_sun_direction.y);
    float daylight = clamp(max(sky_light, sun_height), 0.0, 1.0);
    // Never fully zero so the lake does not become a pure-black hole; a faint
    // floor keeps a hint of ambient/moon sheen on the surface.
    float light_scale = mix(0.06, 1.0, daylight);

    // --- 2. Surface Normals & Flow ---
    vec4 flow_data = texture(u_flow_map, fs_in.world_pos.xz * 0.05);
    vec2 flow_vector = (flow_data.rg * 2.0 - 1.0) * flow_data.a;

    vec2 normal_uv1 = fs_in.world_pos.xz * 0.1 + u_time * 0.02 + flow_vector;
    vec2 normal_uv2 = fs_in.world_pos.xz * 0.25 - u_time * 0.035 + flow_vector * 0.5;

    vec3 normal1 = texture(u_normal_map, normal_uv1).rgb * 2.0 - 1.0;
    vec3 normal2 = texture(u_normal_map, normal_uv2).rgb * 2.0 - 1.0;
    // Tangent-space samples (+Z = unperturbed) mapped onto the horizontal
    // water plane: only the XY wobble tilts the world normal. The previous
    // code added the raw tangent vector to the world normal, which tilted a
    // perfectly flat surface 45 degrees toward +Z and broke the fresnel and
    // reflection directions.
    vec3 bump = vec3(normal1.x * 0.6 + normal2.x * 0.4, 0.0, normal1.y * 0.6 + normal2.y * 0.4);

    // Procedural per-pixel ripple detail. The bound normal map is the flat
    // fallback, so the texture bump above is ~zero and the surface would shade
    // as interpolated per-vertex normals -> hard triangle facets. The analytic
    // ripple gradient tilts the world normal continuously per fragment, so the
    // lighting, fresnel and reflection vectors vary smoothly across each
    // triangle and the faceting/seams break up into a rippled surface. The
    // gradient is the XZ slope of the  field; a negative slope in
    // X/Z tilts the +Y normal toward -X/-Z, matching the tangent-space bump
    // convention used above (only XZ wobble, world up stays +Y).
    vec2 ripple = ripple_gradient(fs_in.world_pos.xz, u_time, flow_vector);
    bump += vec3(-ripple.x, 0.0, -ripple.y);

    vec3 surface_normal = normalize(fs_in.world_normal + bump);

    // --- 3. Depth & Scene Reconstruction ---
    float background_depth_sample = texture(u_opaque_depth, screen_uv).r;
    bool background_has_opaque_depth = has_opaque_depth(background_depth_sample);

    // Sky and clouds have resolved color but retain far-plane depth. Treat that
    // case as open water instead of reconstructing a far-plane world position.
    float water_depth = max(6.0, 1.0 / max(u_water_depth_scaler, 0.001));
    if (background_has_opaque_depth) {
        vec3 background_world_pos = world_pos_from_depth(background_depth_sample, screen_uv);
        water_depth = max(0.0, fs_in.world_pos.y - background_world_pos.y);
    }

    // --- 4. Refraction ---
    vec2 refraction_offset = surface_normal.xz * (0.05 + flow_data.a * 0.02);
    vec2 refraction_uv = clamp(screen_uv + refraction_offset, vec2(0.001), vec2(0.999));
    // Light-scaled deep tint used wherever the surface falls back to a constant
    // water colour (refraction/reflection misses). Like the body colour, this
    // must dim at night or the constant blue becomes self-lit in dark frames.
    vec3 deep_fill = u_deep_color * light_scale;
    // The color input is the resolved pre-water scene, including sky. It is
    // valid even when the corresponding depth sample is at the far plane.
    vec3 refracted_color = texture(u_opaque_scene_color, refraction_uv).rgb;

    // --- 5. Reflection (inline SSR: 8-step raymarch + binary refinement + edge fade) ---
    //  decision: the whole water pass (caustics + SSR + shading)
    // measures ~0.2 ms GPU, so the inline march is improved in place instead
    // while keeping reflection sampling local to the water pass.
    vec3 reflection_vector = reflect(view_dir, surface_normal);
    // Rays that leave the screen without hitting geometry reflect the sky for
    // upward directions and the deep water tint for grazing/downward ones.
    vec3 resolved_background_color = texture(u_opaque_scene_color, screen_uv).rgb;
    vec3 sky_miss_color = background_has_opaque_depth ? u_sky_color : resolved_background_color;
    vec3 miss_color = mix(deep_fill, sky_miss_color, clamp(reflection_vector.y * 2.0 + 0.2, 0.0, 1.0));
    vec3 reflected_color = miss_color;

    // Reduced steps and adaptive quality based on fresnel
    float fresnel_preview = pow(1.0 - max(0.0, dot(-view_dir, surface_normal)), 2.0);
    const int max_steps = 8;
    int num_steps = int(mix(4.0, float(max_steps), fresnel_preview)); // Adaptive quality
    const float step_size = 0.15;
    const float thickness = 0.2;

    // Early exit if reflection vector points down
    if (reflection_vector.y < -0.1) {
        reflected_color = deep_fill;
    } else {
        vec3 ray_pos = fs_in.world_pos;
        vec3 prev_pos = ray_pos;
        for (int i = 0; i < num_steps; ++i) {
            prev_pos = ray_pos;
            ray_pos += reflection_vector * step_size * (1.0 + float(i) * 0.1); // Progressive step size

            vec4 ray_clip_pos = u_view_projection * vec4(ray_pos, 1.0);
            vec2 ray_uv = ray_clip_pos.xy / ray_clip_pos.w * 0.5 + 0.5;

            // Boundary check: off-screen rays keep the miss color
            if (any(lessThan(ray_uv, vec2(0.02))) || any(greaterThan(ray_uv, vec2(0.98)))) {
                break;
            }

            float scene_depth = texture(u_opaque_depth, ray_uv).r;
            // No opaque geometry at this sample (sky): keep marching. Without
            // this, far-plane reconstructions register as fake hits and the
            // reflection samples the unrendered region of the opaque buffer
            // (grey-white blotches instead of sky).
            if (!has_opaque_depth(scene_depth)) {
                continue;
            }
            vec3 scene_pos = world_pos_from_depth(scene_depth, ray_uv);

            if (ray_pos.y < scene_pos.y && scene_pos.y - ray_pos.y < thickness) {
                // Binary-search refinement between the last miss and the hit
                // for a sharper intersection (4 bisections ~= 16x the march
                // precision for 4 extra depth samples on hit only).
                vec3 lo = prev_pos;
                vec3 hi = ray_pos;
                for (int j = 0; j < 4; ++j) {
                    vec3 mid = 0.5 * (lo + hi);
                    vec4 mid_clip = u_view_projection * vec4(mid, 1.0);
                    vec2 mid_uv = mid_clip.xy / mid_clip.w * 0.5 + 0.5;
                    float mid_depth = texture(u_opaque_depth, mid_uv).r;
                    if (!has_opaque_depth(mid_depth)) {
                        lo = mid;
                        continue;
                    }
                    vec3 mid_scene = world_pos_from_depth(mid_depth, mid_uv);
                    if (mid.y < mid_scene.y) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                vec4 hit_clip = u_view_projection * vec4(hi, 1.0);
                vec2 hit_uv = clamp(hit_clip.xy / hit_clip.w * 0.5 + 0.5, vec2(0.0), vec2(1.0));

                // Edge fade: hits near the screen border blend back into the
                // miss color so reflections do not cut off harshly where the
                // SSR information runs out.
                float border_distance = min(min(hit_uv.x, 1.0 - hit_uv.x), min(hit_uv.y, 1.0 - hit_uv.y));
                float edge_fade = smoothstep(0.0, 0.08, border_distance);
                reflected_color = mix(miss_color, texture(u_opaque_scene_color, hit_uv).rgb, edge_fade);
                break;
            }
        }
    }
    reflected_color *= u_reflection_power;

    // --- 6. Fresnel Term ---
    float fresnel = pow(1.0 - max(0.0, dot(-view_dir, surface_normal)), 4.0);
    fresnel = clamp(fresnel, 0.05, 0.95);

    // --- 7. Water Color & Absorption ---
    float absorption_factor = 1.0 - exp(-water_depth * u_water_depth_scaler);
    // Smooth depth tint curve: the eased exponential keeps the first couple
    // of meters bright teal and rolls smoothly into the dark deep tint
    // instead of the old quasi-linear ramp.
    float tint_curve = smoothstep(0.0, 1.0, absorption_factor);
    vec3 shallow_tint = u_shallow_color * 1.08; // slight lift so the shallows read bright
    vec3 water_color = mix(shallow_tint, u_deep_color, tint_curve);
    // Dim the water body by the time-of-day light factor. This is the term that
    // made the lake glow electric cyan at night: it is a constant material
    // colour, so without this scale it stays fully bright regardless of how
    // dark the scene is. At noon light_scale ~= 1.0 (natural blue preserved);
    // at night it drops to a faint sheen so only sky/moon reflection remains.
    water_color *= light_scale;

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
        // Caustics are sunlight focused through the surface; at night there is
        // no sun to focus, so dim them with the daylight factor instead of
        // letting them add a constant cyan shimmer in dark frames.
        caustics_color *= light_scale;
    }

    // --- 10. Shoreline Foam (procedural, ) ---
    // The old path multiplied by u_foam_texture, whose engine fallback is
    // solid black, so foam never rendered. The band is now generated
    // procedurally: a depth-bounded shoreline band, animated
    // rolling shoreward (u_time + flow), and hashed sparkle so it reads as
    // broken foam rather than a solid stripe.
    float shoreline_band = smoothstep(0.9, 0.1, water_depth);
    float foam_phase = water_depth * 8.0 - u_time * 1.6 + (flow_vector.x + flow_vector.y) * 4.0;
    float foam_wave = 0.5 + 0.5 * sin(foam_phase);
    // Smooth animated noise (was a blocky floor-cell hash -> hard pixel grid).
    // Sampled in world space at ~1.5 m features so foam reads as soft broken froth
    // at any view distance instead of a fixed-screen grid.
    float foam_sparkle = foam_fbm(fs_in.world_pos.xz * 0.7, u_time * 0.6, flow_vector);
    float flow_foam = flow_data.b;

    float foam_factor = clamp(shoreline_band * (0.55 + 0.55 * foam_wave + 0.55 * foam_sparkle) + flow_foam * 0.5, 0.0, 1.0);
    // Foam is bright wind-whipped froth lit by the sky/sun; it is NOT emissive.
    // The base colour is near-white (luminance ~0.95), so without dimming it was
    // the brightest thing in a night frame -- the self-lit cyan-white shore band
    // the critique flagged. Scale the foam colour by the same daylight factor the
    // body/caustics use so the shoreline goes dark at night and reads as natural
    // foam by day. (Dim the colour, not the coverage, so the foam *shape* -- the
    // animated  and band width -- stays identical between day/night.)
    vec3 foam_color = vec3(0.92, 0.96, 0.94) * light_scale;

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
    final_color = mix(final_color, water_color, clamp(0.22 + absorption_factor * 0.68, 0.22, 0.9));
    final_color += underwater_color; // Add underwater environment
    final_color += caustics_color * 0.6; // Add caustics
    final_color += specular_highlight;
    final_color = mix(final_color, foam_color, foam_factor); // Blend foam on top

    // Minimum tint floor. Previously this clamped the surface up to a constant
    // teal (vec3(0.025,0.14,0.24)) no matter the lighting, which is what kept
    // the lake glowing cyan at night even after everything else darkened. Scale
    // the whole floor by the daylight factor so at night the floor collapses
    // toward black (only sky/moon reflection survives) while daytime water keeps
    // its lifted body colour and reads as natural water.
    vec3 minimum_water_tint = max(
        mix(u_shallow_color, u_deep_color, clamp(absorption_factor, 0.0, 1.0)) * 0.72,
        vec3(0.025, 0.14, 0.24)
    ) * light_scale;
    final_color = max(final_color, minimum_water_tint);

    float alpha = clamp(0.58 + absorption_factor * 0.22 + fresnel * 0.12, 0.58, 0.86);
    // Foam is opaque froth on the surface; lift alpha with the foam factor so
    // the band stays bright over any background.
    alpha = clamp(alpha + foam_factor * 0.25, 0.58, 0.97);
    o_frag_color = vec4(final_color, alpha);
}
