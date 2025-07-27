#version 410
out vec4 FragColor;

in vec4
    VertexColor;

void main()
{
    // Output the color directly (e.g., vec4(0.1, 0.3, 0.8, 0.7))
    // This correctly uses the 0.7 alpha for transparency.
    FragColor = VertexColor;
}