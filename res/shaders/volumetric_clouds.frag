#version 410
out vec4 FragColor;

in vec2 v_TexCoords;

// --- Uniforms from your Engine ---
uniform mat4 u_inverseView;
uniform mat4 u_inverseProjection;
uniform vec3 u_cameraPos;
uniform sampler2D u_depthMap; // Scene depth
uniform float u_time;
uniform mat4 u_view;
uniform vec3 u_sunDirection; // Ensure this is passed from your world

// --- Shader Constants & Globals ---
const int OCTAVES = 2; // Keep this low (2-3) for performance
vec3 sundir = normalize(u_sunDirection); // Use engine's sun direction

// --- Noise Functions ---
float mod289(float x){return x - floor(x * (1.0 / 289.0)) * 289.0;}
vec4 mod289(vec4 x){return x - floor(x * (1.0 / 289.0)) * 289.0;}
vec4 perm(vec4 x){return mod289(((x * 34.0) + 1.0) * x);}

float noise(vec3 p){
    vec3 a = floor(p);
    vec3 d = p - a;
    d = d * d * (3.0 - 2.0 * d);
    vec4 b = a.xxyy + vec4(0.0, 1.0, 0.0, 1.0);
    vec4 k1 = perm(b.xyxy);
    vec4 k2 = perm(k1.xyxy + b.zzww);
    vec4 c = k2 + a.zzzz;
    vec4 k3 = perm(c);
    vec4 k4 = perm(c + 1.0);
    vec4 o1 = fract(k3 * (1.0 / 41.0));
    vec4 o2 = fract(k4 * (1.0 / 41.0));
    vec4 o3 = o2 * d.z + o1 * (1.0 - d.z);
    vec2 o4 = o3.yw * d.x + o3.xz * (1.0 - d.x);
    return o4.y * d.y + o4.x * (1.0 - d.y);
}

float fbm(vec3 x, int octaves) {
    float v = 0.0;
    float a = 0.5;
    vec3 shift = vec3(100);
    for (int i = 0; i < octaves; ++i) {
        v += a * noise(x);
        x = x * 2.0 + shift;
        a *= 0.5;
    }
    return v;
}

// --- Helper Functions ---
// Reconstructs world position from screen coordinates and depth buffer
vec3 getWorldPos(float depth, vec2 texCoords) {
    vec4 ndc = vec4(texCoords * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = u_inverseProjection * ndc;
    world.xyz /= world.w;
    world = u_inverseView * world;
    return world.xyz;
}

// --- Cloud Logic ---
float sphereSDF(vec3 position, float radius) {
    return length(noise(position * 0.5) - position) - radius;
}

float densityFunc(const vec3 p) {
    vec3 q = p;
    q += vec3(0.0, 0.0, -0.20) * u_time;
    float f = fbm(q, OCTAVES);
    return clamp(f - 0.5, 0.0, 1.0);
}

vec3 lighting(const vec3 pos, const float cloudDensity, const float pathLength) {
    float clampedDensity = clamp(cloudDensity, 0.0, 1.0);
    vec3 lightnessFactor = vec3(0.91, 0.98, 1.0) + vec3(1.0, 0.6, 0.3) * 2.0 * clampedDensity;
    vec3 darknessFactor = mix(vec3(1.0, 0.95, 1.0), vec3(0.0, 0.0, 0.0), cloudDensity);
    
    float transmittance = exp(-0.005 * pathLength * clampedDensity);

    return darknessFactor * lightnessFactor * transmittance;
}


void main() {
    // --- 1. Ray Setup (Adapted for your Engine) ---
    vec3 rayOrigin = u_cameraPos;
    vec3 farPlanePos = getWorldPos(1.0, v_TexCoords);
    vec3 rayDirection = normalize(farPlanePos - rayOrigin);

    float sceneDepth = texture(u_depthMap, v_TexCoords).r;
    float MAX_DISTANCE = distance(rayOrigin, getWorldPos(sceneDepth, v_TexCoords));

    vec4 colorSum = vec4(0.0);
    float rayDistance = 0.0;

    // --- 2. Ray Marching Loop ---
    for (int i = 0; i < 120; i++) {
        if (rayDistance > MAX_DISTANCE || colorSum.a > 0.99) break;

        vec3 currentStep = rayOrigin + rayDirection * rayDistance;
        
        vec3 cameraSpacePos = (u_view * vec4(currentStep, 1.0)).xyz;
        bool insideBoundries = cameraSpacePos.z < -2.0 && cameraSpacePos.z > -150.0;
        vec3 movedStepWithTime = currentStep + vec3(0.0, 0.0, -0.6) * u_time;

        // Define the cloud layer by height
        // 1. Loosen the height check to make clouds appear at all altitudes for now.
        bool inCloudLayer = true; // currentStep.y > 100.0 && currentStep.y < 250.0;
        float dist = 1.0; 
        if (inCloudLayer) {
            dist = sphereSDF(movedStepWithTime, 0.95);
        }

        if (dist < 0.1 && insideBoundries) {
            float cloudDensity = densityFunc(currentStep);
            if(cloudDensity > 0.0) {
                vec3 colorRGB = lighting(movedStepWithTime, cloudDensity, rayDistance);
                
                // 2. Increase the alpha to make them less transparent for testing.
                float alpha = cloudDensity * 0.5; // Changed from 0.1 to 0.5
                
                // Accumulate color using front-to-back blending
                colorSum.rgb += colorRGB * alpha * (1.0 - colorSum.a);
                colorSum.a += alpha * (1.0 - colorSum.a);
            }
        }
        
        // Advance the ray
        rayDistance += max(dist, 0.1);
    }
    
    // --- 3. Final Output ---
    FragColor = vec4(colorSum.rgb, colorSum.a);
}