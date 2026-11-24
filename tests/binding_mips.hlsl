RWByteAddressBuffer src : register(u0);
RWByteAddressBuffer dst : register(u1);
RWTexture2D<float4> recurrent : register(u2);
cbuffer RecurrentPattern : register(b2) { uint outputWidth, outputHeight, seed; }

[RootSignature("DescriptorTable(UAV(u2)),RootConstants(num32BitConstants=3,b2)")]
[numthreads(8,8,1)]
void WriteRecurrent(uint2 xy : SV_DispatchThreadID)
{
    if (xy.x >= outputWidth || xy.y >= outputHeight) return;
    uint4 rgba = uint4(xy.x + seed, xy.y + 3 * seed, (xy.x ^ xy.y) + seed, 193 + seed);
    recurrent[xy] = float4(rgba & 255u) / 255.0;
}
cbuffer Bounds : register(b0) { uint count; }

[RootSignature("UAV(u0),UAV(u1),RootConstants(num32BitConstants=1,b0)")]
[numthreads(64,1,1)]
void DualUav(uint i : SV_DispatchThreadID)
{
    if (i < count) dst.Store(i*4, (src.Load(i*4) + 17u) ^ i);
}

Texture2D<float4> image : register(t0);
SamplerState pointClamp : register(s0);
cbuffer Mip : register(b1) { uint level, width, height, first; }

[RootSignature("DescriptorTable(SRV(t0)),UAV(u1),RootConstants(num32BitConstants=4,b1),"
               "StaticSampler(s0,filter=FILTER_MIN_MAG_MIP_POINT,addressU=TEXTURE_ADDRESS_CLAMP,"
               "addressV=TEXTURE_ADDRESS_CLAMP,addressW=TEXTURE_ADDRESS_CLAMP)")]
[numthreads(8,8,1)]
void SampleMip(uint2 xy : SV_DispatchThreadID)
{
    if (xy.x >= width || xy.y >= height) return;
    float2 uv = (float2(xy) + 0.5) / float2(width, height);
    uint4 rgba = uint4(round(image.SampleLevel(pointClamp, uv, level) * 255.0));
    dst.Store(4*(first + xy.y*width + xy.x), rgba.r | (rgba.g<<8) | (rgba.b<<16) | (rgba.a<<24));
}

SamplerState dynamicSamplers[2] : register(s1);
cbuffer SamplerSelection : register(b3) { uint samplerIndex, forceZero, outputWord; }

[RootSignature("DescriptorTable(SRV(t0)),DescriptorTable(Sampler(s1,numDescriptors=2)),"
               "UAV(u1),RootConstants(num32BitConstants=3,b3)")]
[numthreads(1,1,1)]
void DynamicSamplerIndex()
{
    const uint selected = forceZero != 0 ? 0 : samplerIndex;
    const float value = image.SampleLevel(dynamicSamplers[selected], float2(1.25, 0.5), 0).x;
    dst.Store(4 * outputWord, asuint(value));
}
