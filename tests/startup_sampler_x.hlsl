// One point-sampled probe per dispatch through a sampler descriptor table. The C++ side owns the
// probe coordinates, the sampler descriptors and the reference values.
Texture2D<float> texels : register(t0);
SamplerState probeSampler : register(s0);
RWByteAddressBuffer outputWords : register(u0);

cbuffer Probe : register(b0)
{
    float2 coordinate;
    float lod;
    uint outputByteOffset;
};

[numthreads(1, 1, 1)]
void main()
{
    outputWords.Store(outputByteOffset, asuint(texels.SampleLevel(probeSampler, coordinate, lod)));
}
