// Source-built VS/PS pair for the D3D12.X graphics-list fixture.
// The vertex shader passes a clip-space triangle through unchanged; the pixel shader writes one
// constant color so the readback can be compared byte for byte with a CPU reference.

struct VertexIn
{
    float4 position : POSITION;
};

struct PixelIn
{
    float4 position : SV_Position;
};

PixelIn VSMain(VertexIn input)
{
    PixelIn output;
    output.position = input.position;
    return output;
}

float4 PSMain(PixelIn input) : SV_Target
{
    return float4(0.2, 0.4, 0.6, 1.0);
}
