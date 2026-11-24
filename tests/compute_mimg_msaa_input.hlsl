float4 vs_main(uint vertex : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// SV_SampleIndex makes the pixel shader run once per covered sample.
uint ps_main(float4 position : SV_Position, uint sample : SV_SampleIndex) : SV_Target
{
    return 0x91000000u | (sample << 16u) | (uint(position.y) << 8u) | uint(position.x);
}
