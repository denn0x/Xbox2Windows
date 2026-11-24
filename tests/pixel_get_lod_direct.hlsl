// Independent source for the D3D12 LOD query comparison. The renderer binds
// the same texture, static point-clamp sampler and constants to both shaders.
Texture2D<float4> image : register(t0);
SamplerState pointClamp : register(s0);
cbuffer Footprint : register(b0) {
    float4 scales;
};

float4 lower_scale(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 coord = uv * scales.zw;
    float clamped = image.CalculateLevelOfDetail(pointClamp, coord);
    float raw = image.CalculateLevelOfDetailUnclamped(pointClamp, coord);
    return float4(clamped, raw, 0.5, 1.0);
}

float4 upper_clamp(float2 uv : TEXCOORD0) : SV_Target0 {
    float2 coord = float2(uv.x * scales.z, uv.y);
    float clamped = image.CalculateLevelOfDetail(pointClamp, coord);
    float raw = image.CalculateLevelOfDetailUnclamped(pointClamp, coord);
    return float4(raw - 2.0, clamped, 0.0, 1.0);
}
