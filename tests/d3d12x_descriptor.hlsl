// Source-built XCL-09 table probe: one CBV, SRV, UAV and sampler descriptor table.
cbuffer Factor : register(b0)
{
    float4 factor;
};

Texture2D<float4> sourceTexture : register(t0);
RWStructuredBuffer<float4> destinationBuffer : register(u0);
SamplerState pointSampler : register(s0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float4 texel = sourceTexture.SampleLevel(pointSampler, float2(0.5, 0.5), 0);
    destinationBuffer[0] = texel * factor;
}
