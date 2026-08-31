#version 450 core

// Attribute-less fullscreen triangle (no VBO needed): the 3 vertices are derived
// from gl_VertexID and cover the whole screen. TexCoords spans [0,1] across the
// visible region. Used by render-only deferred overlays (e.g. the pheromone
// ground decal) that sample the G-buffer per pixel.
out vec2 TexCoords;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // (0,0)(2,0)(0,2)
    TexCoords = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);            // (-1,-1)(3,-1)(-1,3)
}
