ByteAddressBuffer windowData : register(t0);
RWByteAddressBuffer outputWords : register(u0);

cbuffer Destination : register(b0)
{
    uint outputByteOffset;
};

[numthreads(1, 1, 1)]
void main()
{
    outputWords.Store(outputByteOffset, windowData.Load(0));
}
