// tests/startup_cb_blend.cpp: a full-screen triangle that writes one constant colour, so the render
// target holds exactly the blend of that colour with the cleared destination.
cbuffer Source : register(b0) { float4 source; }

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
}

float4 ps_main(float4 position : SV_Position) : SV_Target0
{
    return source;
}
