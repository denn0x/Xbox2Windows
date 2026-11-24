// Source-built XCL-10 compute list probe. One structured UAV table entry and two root
// constants select a contiguous word range and an addend for each dispatch.
RWStructuredBuffer<uint> targetWords : register(u0);

cbuffer Params : register(b0)
{
    uint baseIndex;
    uint addend;
    uint pad0;
    uint pad1;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint index = baseIndex + id.x;
    targetWords[index] = targetWords[index] + addend;
}
