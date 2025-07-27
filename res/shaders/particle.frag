#version 410
out vec4 FragColor;

void main()
{
    // Use point coordinates to draw a circle instead of a square
    vec2 circ = gl_PointCoord - vec2(0.5);
    if (dot(circ, circ) > 0.25) {
        discard;
    }
    // A nice leafy green/yellow color with some transparency
    FragColor = vec4(0.4, 0.6, 0.2, 0.6);
}