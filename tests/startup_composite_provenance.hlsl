// tests/startup_composite_provenance.cpp (#1628): pass 1 fills the composite source with constant colours
// (ps_fill); the composite (ps_composite) copies one exact source texel per destination pixel. The texel is
// floor(position) * scale + offset, read with Load, so no filtering or rounding enters the readback.
cbuffer Params : register(b0)
{
    float4 fill;          // ps_fill: the colour written
    int2 scale;           // ps_composite: texel = int2(position.xy) * scale + offset
    int2 offset;
}
Texture2D<float4> source : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
}

float4 ps_fill(float4 position : SV_Position) : SV_Target0
{
    return fill;
}

float4 ps_composite(float4 position : SV_Position) : SV_Target0
{
    return source.Load(int3(int2(position.xy) * scale + offset, 0));
}
